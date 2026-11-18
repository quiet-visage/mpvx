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

VideoEntry make_test_entry(std::string id, std::string path, std::string title, float progress) {
    VideoEntry e;
    e.id = slint::SharedString(id);
    e.path = slint::SharedString(path);
    e.title = slint::SharedString(title);
    e.subtitle = slint::SharedString("MKV · 1.2 GB");
    e.duration_text = slint::SharedString("10:00");
    e.duration_sec = 600.0f;
    e.position_sec = 600.0f * progress;
    e.progress = progress;
    e.watched = (progress >= 0.95f);
    e.has_thumbnail = false;
    return e;
}

} // namespace

TEST_CASE("Picker UI boot mode and view transitions", "[ui][picker]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();

    REQUIRE(ui->get_view_mode() == AppViewMode::Picker);
    REQUIRE(ui->get_sidebar_open() == true);
    REQUIRE(ui->get_active_nav_kind() == PickerNavKind::Home);

    ui->set_view_mode(AppViewMode::Player);
    REQUIRE(ui->get_view_mode() == AppViewMode::Player);

    bool return_to_picker_fired = false;
    ui->on_return_to_picker_clicked([&]() {
        return_to_picker_fired = true;
        ui->set_view_mode(AppViewMode::Picker);
    });
    ui->invoke_return_to_picker_clicked();
    REQUIRE(return_to_picker_fired);
    REQUIRE(ui->get_view_mode() == AppViewMode::Picker);
}

TEST_CASE("Picker UI sidebar collapse via Ctrl+B", "[ui][picker]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();
    REQUIRE(ui->get_sidebar_open() == true);

    ui->set_sidebar_open(!ui->get_sidebar_open());
    REQUIRE(ui->get_sidebar_open() == false);

    ui->set_sidebar_open(!ui->get_sidebar_open());
    REQUIRE(ui->get_sidebar_open() == true);
}

TEST_CASE("Picker UI card click and resume callback", "[ui][picker]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();

    std::string clicked_path;
    bool clicked_resume = false;
    ui->on_picker_video_clicked([&](slint::SharedString path, bool resume) {
        clicked_path = std::string(path);
        clicked_resume = resume;
    });

    auto row_model = std::make_shared<slint::VectorModel<VideoEntry>>(std::vector<VideoEntry>{
        make_test_entry("0001", "/videos/sample.mkv", "Sample Video", 0.45f)
    });
    auto rows = std::make_shared<slint::VectorModel<VideoGridRow>>(std::vector<VideoGridRow>{
        VideoGridRow{0, row_model}
    });
    ui->set_picker_direct_video_rows(rows);

    ui->invoke_picker_video_clicked(slint::SharedString("/videos/sample.mkv"), true);
    REQUIRE(clicked_path == "/videos/sample.mkv");
    REQUIRE(clicked_resume == true);
}

TEST_CASE("Picker UI fine-grained nested set_row_data thumbnail update", "[ui][picker]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();

    auto shelf_items = std::make_shared<slint::VectorModel<VideoEntry>>(std::vector<VideoEntry>{
        make_test_entry("aaaa", "/videos/Movies/A.mkv", "Movie A", 0.0f),
        make_test_entry("bbbb", "/videos/Movies/B.mkv", "Movie B", 0.5f),
    });
    auto shelves = std::make_shared<slint::VectorModel<ShelfSection>>(std::vector<ShelfSection>{
        ShelfSection{slint::SharedString("/videos/Movies"), slint::SharedString("Movies"), 2, shelf_items}
    });
    ui->set_picker_shelves(shelves);

    // Mutate only cell 1 inside the inner VectorModel without resetting parent shelf
    slint::SharedPixelBuffer<slint::Rgba8Pixel> pix(4, 4);
    auto entry_opt = shelf_items->row_data(1);
    REQUIRE(entry_opt.has_value());
    VideoEntry updated = *entry_opt;
    updated.thumbnail = slint::Image(pix);
    updated.has_thumbnail = true;
    shelf_items->set_row_data(1, updated);

    auto after = shelf_items->row_data(1);
    REQUIRE(after.has_value());
    REQUIRE(after->has_thumbnail == true);
    REQUIRE(shelf_items->row_data(0)->has_thumbnail == false);
    REQUIRE(ui->get_picker_shelves()->row_count() == 1);
}

TEST_CASE("Picker UI See all drill-down and search callbacks", "[ui][picker]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();

    PickerNavKind last_kind = PickerNavKind::Home;
    std::string last_path;
    ui->on_picker_nav_selected([&](PickerNavKind k, slint::SharedString p) {
        last_kind = k;
        last_path = std::string(p);
    });

    ui->invoke_picker_nav_selected(PickerNavKind::Folder, slint::SharedString("/videos/Anime"));
    REQUIRE(last_kind == PickerNavKind::Folder);
    REQUIRE(last_path == "/videos/Anime");

    std::string search_q;
    ui->on_picker_search_changed([&](slint::SharedString q) {
        search_q = std::string(q);
    });
    ui->invoke_picker_search_changed(slint::SharedString("Dune"));
    REQUIRE(search_q == "Dune");
}
