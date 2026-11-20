#include "app-window.h"
#include "mpvx/core.hpp"
#include "mpvx/library.hpp"
#include "mpvx/player.hpp"
#include "mpvx/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

LoopMode to_slint_loop_mode(mpvx::LoopMode mode) noexcept {
    switch (mode) {
        case mpvx::LoopMode::Off:
            return LoopMode::Off;
        case mpvx::LoopMode::File:
            return LoopMode::File;
        case mpvx::LoopMode::Playlist:
            return LoopMode::Playlist;
    }
    return LoopMode::Off;
}

AbLoopState to_slint_ab_state(mpvx::AbLoopState state) noexcept {
    switch (state) {
        case mpvx::AbLoopState::Off:
            return AbLoopState::Off;
        case mpvx::AbLoopState::SetA:
            return AbLoopState::SetA;
        case mpvx::AbLoopState::Active:
            return AbLoopState::Active;
    }
    return AbLoopState::Off;
}

PickerNavKind to_slint_nav_kind(mpvx::LibraryNavKind kind) noexcept {
    switch (kind) {
        case mpvx::LibraryNavKind::Home:
            return PickerNavKind::Home;
        case mpvx::LibraryNavKind::ContinueWatching:
            return PickerNavKind::ContinueWatching;
        case mpvx::LibraryNavKind::Recent:
            return PickerNavKind::Recent;
        case mpvx::LibraryNavKind::Folder:
            return PickerNavKind::Folder;
    }
    return PickerNavKind::Home;
}

mpvx::LibraryNavKind from_slint_nav_kind(PickerNavKind kind) noexcept {
    switch (kind) {
        case PickerNavKind::Home:
            return mpvx::LibraryNavKind::Home;
        case PickerNavKind::ContinueWatching:
            return mpvx::LibraryNavKind::ContinueWatching;
        case PickerNavKind::Recent:
            return mpvx::LibraryNavKind::Recent;
        case PickerNavKind::Folder:
            return mpvx::LibraryNavKind::Folder;
    }
    return mpvx::LibraryNavKind::Home;
}

mpvx::LibrarySortMode from_slint_sort_mode(PickerSortMode mode) noexcept {
    switch (mode) {
        case PickerSortMode::Name:
            return mpvx::LibrarySortMode::Name;
        case PickerSortMode::Recent:
            return mpvx::LibrarySortMode::Recent;
        case PickerSortMode::Progress:
            return mpvx::LibrarySortMode::Progress;
    }
    return mpvx::LibrarySortMode::Name;
}

// GUI-thread-only controller managing persistent nested VectorModels and O(1) cell updates
class PickerUiController {
public:
    explicit PickerUiController(slint::ComponentHandle<MainWindow> ui)
        : ui_(std::move(ui))
        , folders_model_(std::make_shared<slint::VectorModel<FolderNode>>())
        , breadcrumbs_model_(std::make_shared<slint::VectorModel<slint::SharedString>>())
        , continue_model_(std::make_shared<slint::VectorModel<VideoEntry>>())
        , direct_rows_model_(std::make_shared<slint::VectorModel<VideoGridRow>>())
        , shelves_model_(std::make_shared<slint::VectorModel<ShelfSection>>())
    {
        ui_->set_picker_folders(folders_model_);
        ui_->set_picker_breadcrumbs(breadcrumbs_model_);
        ui_->set_picker_continue_watching(continue_model_);
        ui_->set_picker_direct_video_rows(direct_rows_model_);
        ui_->set_picker_shelves(shelves_model_);
    }

    void apply_snapshot(const std::shared_ptr<const mpvx::LibraryViewSnapshot>& snap) {
        if (!snap) return;
        last_snapshot_ = snap;
        key_to_cells_.clear();

        ui_->set_active_nav_kind(to_slint_nav_kind(snap->nav_kind));
        ui_->set_current_folder_path(slint::SharedString(snap->arena.view(snap->current_folder_path)));
        ui_->set_picker_scanning(!snap->shelves_complete);

        // Sidebar folders
        std::vector<FolderNode> fnodes;
        fnodes.reserve(snap->sidebar_folders.size());
        for (const auto& f : snap->sidebar_folders) {
            fnodes.push_back(FolderNode{
                slint::SharedString(snap->arena.view(f.path)),
                slint::SharedString(snap->arena.view(f.name)),
                f.depth,
                f.has_children,
                f.expanded,
                f.selected,
            });
        }
        folders_model_->set_vector(std::move(fnodes));

        // Breadcrumbs
        std::vector<slint::SharedString> crumbs;
        crumbs.reserve(snap->breadcrumbs.size());
        for (const auto& bc : snap->breadcrumbs) {
            crumbs.emplace_back(snap->arena.view(bc));
        }
        breadcrumbs_model_->set_vector(std::move(crumbs));

        // Continue Watching
        std::vector<VideoEntry> cw_items;
        cw_items.reserve(snap->continue_watching.size());
        for (size_t i = 0; i < snap->continue_watching.size(); ++i) {
            const auto& item = snap->continue_watching[i];
            cw_items.push_back(to_video_entry(item, snap->arena));
            key_to_cells_[item.cache_key].push_back(ModelCellRef{continue_model_, i});
        }
        continue_model_->set_vector(std::move(cw_items));

        // Direct Video Rows (chunked by grid_columns_)
        rebuild_direct_rows(*snap);

        // Subcategory Shelves (each backed by a persistent inner VectorModel<VideoEntry>)
        std::vector<ShelfSection> shelf_sections;
        shelf_sections.reserve(snap->shelves.size());
        for (const auto& sh : snap->shelves) {
            auto inner_model = std::make_shared<slint::VectorModel<VideoEntry>>();
            std::vector<VideoEntry> items;
            items.reserve(sh.items.size());
            for (size_t i = 0; i < sh.items.size(); ++i) {
                const auto& item = sh.items[i];
                items.push_back(to_video_entry(item, snap->arena));
                key_to_cells_[item.cache_key].push_back(ModelCellRef{inner_model, i});
            }
            inner_model->set_vector(std::move(items));
            shelf_sections.push_back(ShelfSection{
                slint::SharedString(snap->arena.view(sh.folder_path)),
                slint::SharedString(snap->arena.view(sh.title)),
                sh.total_count,
                inner_model,
            });
        }
        shelves_model_->set_vector(std::move(shelf_sections));
    }

    void apply_thumbnail_updates(std::vector<mpvx::ThumbnailUpdate> updates) {
        for (auto& u : updates) {
            slint::Image img;
            if (u.has_pixels && u.pixels.width() > 0 && u.pixels.height() > 0) {
                img = slint::Image(u.pixels);
                cached_images_[u.cache_key] = img;
            }
            auto it = key_to_cells_.find(u.cache_key);
            if (it == key_to_cells_.end()) continue;

            for (const auto& cell : it->second) {
                if (cell.row_index >= cell.model->row_count()) continue;
                auto entry_opt = cell.model->row_data(cell.row_index);
                if (!entry_opt.has_value()) continue;
                VideoEntry entry = *entry_opt;
                if (u.has_pixels) {
                    entry.thumbnail = img;
                    entry.has_thumbnail = true;
                }
                if (u.duration_sec > 0.0) {
                    entry.duration_sec = static_cast<float>(u.duration_sec);
                    entry.duration_text = slint::SharedString(u.duration_text);
                }
                cell.model->set_row_data(cell.row_index, entry);
            }
        }
    }

    void set_grid_columns(int cols) {
        const int clamped = std::clamp(cols, 2, 8);
        if (clamped == grid_columns_) return;
        grid_columns_ = clamped;
        ui_->set_picker_grid_columns(grid_columns_);
        if (last_snapshot_) {
            apply_snapshot(last_snapshot_);
        }
    }

    [[nodiscard]] std::optional<std::string> breadcrumb_path_at(int index) const {
        if (!last_snapshot_ || index < 0 ||
            static_cast<size_t>(index) >= last_snapshot_->breadcrumb_paths.size()) {
            return std::nullopt;
        }
        return std::string(last_snapshot_->arena.view(last_snapshot_->breadcrumb_paths[index]));
    }

private:
    struct ModelCellRef {
        std::shared_ptr<slint::VectorModel<VideoEntry>> model;
        size_t row_index;
    };

    [[nodiscard]] VideoEntry to_video_entry(
        const mpvx::ScannedVideoItem& item,
        const mpvx::StringArena& arena) const
    {
        VideoEntry ve;
        ve.id = slint::SharedString(arena.view(item.id_hex));
        ve.path = slint::SharedString(arena.view(item.path));
        ve.title = slint::SharedString(arena.view(item.title));
        ve.subtitle = slint::SharedString(arena.view(item.subtitle));
        ve.duration_text = slint::SharedString(arena.view(item.duration_text));
        ve.duration_sec = static_cast<float>(item.duration_sec);
        ve.position_sec = static_cast<float>(item.position_sec);
        ve.progress = item.progress;
        ve.watched = item.watched;
        if (auto it = cached_images_.find(item.cache_key); it != cached_images_.end()) {
            ve.thumbnail = it->second;
            ve.has_thumbnail = true;
        } else {
            ve.has_thumbnail = false;
        }
        return ve;
    }

    void rebuild_direct_rows(const mpvx::LibraryViewSnapshot& snap) {
        std::vector<VideoGridRow> rows;
        const size_t cols = static_cast<size_t>(std::max(1, grid_columns_));
        const size_t total = snap.direct_videos.size();
        const size_t num_rows = (total + cols - 1) / cols;
        rows.reserve(num_rows);

        for (size_t r = 0; r < num_rows; ++r) {
            auto row_model = std::make_shared<slint::VectorModel<VideoEntry>>();
            std::vector<VideoEntry> row_items;
            const size_t start = r * cols;
            const size_t end = std::min(total, start + cols);
            row_items.reserve(end - start);
            for (size_t i = start; i < end; ++i) {
                const auto& item = snap.direct_videos[i];
                row_items.push_back(to_video_entry(item, snap.arena));
                key_to_cells_[item.cache_key].push_back(ModelCellRef{row_model, i - start});
            }
            row_model->set_vector(std::move(row_items));
            rows.push_back(VideoGridRow{static_cast<int>(r), row_model});
        }
        direct_rows_model_->set_vector(std::move(rows));
    }

    slint::ComponentHandle<MainWindow> ui_;
    std::shared_ptr<slint::VectorModel<FolderNode>> folders_model_;
    std::shared_ptr<slint::VectorModel<slint::SharedString>> breadcrumbs_model_;
    std::shared_ptr<slint::VectorModel<VideoEntry>> continue_model_;
    std::shared_ptr<slint::VectorModel<VideoGridRow>> direct_rows_model_;
    std::shared_ptr<slint::VectorModel<ShelfSection>> shelves_model_;

    int grid_columns_{4};
    std::shared_ptr<const mpvx::LibraryViewSnapshot> last_snapshot_;
    std::unordered_map<uint64_t, std::vector<ModelCellRef>> key_to_cells_;
    std::unordered_map<uint64_t, slint::Image> cached_images_;
};

// Render-target state of the mini player (docs/mini-player-ui-spec.md §5.2).
enum class RenderTargetMode : uint8_t { Full, FullToMini, Mini };

// GUI-thread-only state machine for the mini player (docs/mini-player-ui-spec.md §5, §7.4).
// Owns render-target sizing for both full and mini mode. All timers are slint::Timer
// single-shots guarded by generation counters, so a stale timer can never act after a
// later transition. No locks: every method runs on the GUI thread.
class MiniPlayerController : public std::enable_shared_from_this<MiniPlayerController> {
public:
    MiniPlayerController(slint::ComponentWeakHandle<MainWindow> ui,
                         std::shared_ptr<mpvx::Player> player,
                         std::shared_ptr<mpvx::VideoRenderer> renderer,
                         std::shared_ptr<mpvx::LibraryService> library,
                         std::shared_ptr<std::string> active_media_path)
        : ui_(std::move(ui))
        , player_(std::move(player))
        , renderer_(std::move(renderer))
        , library_(std::move(library))
        , active_media_path_(std::move(active_media_path))
    {}

    // --- Render-target sizing ---

    void set_video_size(mpvx::VideoDisplaySize size) {
        aspect_ = size.aspect();
        if (auto ui = lock()) {
            (*ui)->set_video_aspect(size.valid() ? static_cast<float>(aspect_) : 16.0f / 9.0f);
        }
        if (mode_ != RenderTargetMode::FullToMini) {
            apply_target(); // never change the target while the shrink morph runs (§2.3 rule 4)
        }
    }

    void on_window_resized() {
        // Not during the shrink morph either (§2.3 rule 4): settle_shrink() and expand()
        // recompute the target from the current geometry.
        if (mode_ == RenderTargetMode::Full) apply_target();
    }

    void on_mini_geometry_changed() {
        if (mode_ == RenderTargetMode::Mini) apply_target();
    }

    // --- Transitions ---

    // Player -> Picker (Library button, Esc in the player). Playback continues in the tile.
    void enter_picker() {
        auto ui = lock();
        if (!ui) return;
        flush_progress();
        library_->request_refresh();
        (*ui)->set_active_popover(ActivePopover::None);

        const bool video_present = player_->video_display_size().valid() &&
                                   player_->state() != mpvx::PlaybackState::Idle;
        if (!video_present) {
            // Nothing to show in a tile (no file, audio-only, idle): plain crossfade.
            // Audio keeps playing; the top bar's Now Playing pill is the way back.
            (*ui)->set_mini_state(MiniState::Hidden);
            (*ui)->set_view_mode(AppViewMode::Picker);
            return;
        }

        mode_ = RenderTargetMode::FullToMini;
        const uint64_t gen = ++target_gen_;
        ++shadow_gen_;
        (*ui)->set_mini_closing(false);
        (*ui)->set_mini_shadow(0.0f);
        (*ui)->set_mini_state(MiniState::Tile);
        (*ui)->set_view_mode(AppViewMode::Picker);

        // Settle detection (§5.3): the render target shrinks only once the morph is over.
        const auto shrink_ms = (*ui)->global<Theme>().get_mini_shrink_duration();
        slint::Timer::single_shot(std::chrono::milliseconds(shrink_ms + kSettleSlackMs),
                                  [weak = weak_from_this(), gen]() {
                                      if (auto self = weak.lock()) self->settle_shrink(gen);
                                  });
    }

    // Mini (tile or pill) or plain picker -> Player. Playback state is untouched, so
    // expanding from the pill stays paused (§4.2).
    void expand() {
        auto ui = lock();
        if (!ui) return;
        ++target_gen_;
        ++shadow_gen_;
        if (mode_ != RenderTargetMode::Full) {
            mode_ = RenderTargetMode::Full;
            apply_target(); // full size + forced redraw first, even while paused
        }
        (void)player_->set_mini_profile(false);
        renderer_->set_frame_cap(false);
        (*ui)->set_mini_shadow(0.0f);
        (*ui)->set_mini_closing(false);
        (*ui)->set_mini_state(MiniState::Hidden);
        (*ui)->set_view_mode(AppViewMode::Player);
        (*ui)->set_hud_visible(true);
        (*ui)->set_pointer_idle_ms(0);
    }

    // Loading another file from the picker leaves mini mode first (§3.3).
    void leave_for_load() {
        auto ui = lock();
        if (ui && (*ui)->get_mini_active()) expand();
    }

    // Tile ✕ / Ctrl+W: pause and collapse to the pill. The file stays loaded (§3.5).
    void minimize() {
        auto ui = lock();
        if (!ui || (*ui)->get_mini_state() != MiniState::Tile) return;
        ++shadow_gen_;
        flush_progress();
        (void)player_->pause();
        renderer_->set_frame_cap(false);
        (*ui)->set_mini_shadow(0.0f);
        (*ui)->set_mini_state(MiniState::Pill);
    }

    // Pill -> tile. `resume` for ▶ / Space; the pill body restores still paused.
    void restore(bool resume) {
        auto ui = lock();
        if (!ui || (*ui)->get_mini_state() != MiniState::Pill) return;
        (*ui)->set_mini_state(MiniState::Tile);
        if (resume) {
            (void)player_->play();
        }
        update_frame_cap();

        const uint64_t gen = ++shadow_gen_;
        const auto collapse_ms = (*ui)->global<Theme>().get_mini_collapse_duration();
        slint::Timer::single_shot(std::chrono::milliseconds(collapse_ms + kSettleSlackMs),
                                  [weak = weak_from_this(), gen]() {
                                      if (auto self = weak.lock()) self->settle_restore(gen);
                                  });
    }

    void set_fps_cap(bool enabled) {
        fps_cap_ = enabled;
        update_frame_cap();
    }

    // Called on the GUI thread with the latest playback state.
    void on_playback_state(mpvx::PlaybackState state) {
        auto ui = lock();
        if (!ui) return;
        const MiniState mini = (*ui)->get_mini_state();
        if (mini == MiniState::Hidden) return;

        if (state != mpvx::PlaybackState::Idle) {
            // Playback came back while fading out (e.g. a file loaded over IPC): stay mini.
            if ((*ui)->get_mini_closing()) cancel_close();
            // Pill => paused invariant (§3.5): an external resume restores the tile. A
            // pause requested by minimize() but not yet confirmed by mpv still reads as
            // Playing here, so only a resume that is also requested/observed counts.
            if (state == mpvx::PlaybackState::Playing && mini == MiniState::Pill && !player_->is_paused()) {
                restore(false);
            }
        } else if (player_->is_idle_active() && !(*ui)->get_mini_closing()) {
            // Only real idleness closes the mini player. END_FILE also reports Idle between
            // playlist entries (advance, loop-playlist), where playback continues (§3.7, §8).
            close_on_idle();
        }
    }

private:
    static constexpr int64_t kSettleSlackMs = 40;

    std::optional<slint::ComponentHandle<MainWindow>> lock() const { return ui_.lock(); }

    void flush_progress() {
        if (!active_media_path_->empty()) {
            library_->record_playback_progress(*active_media_path_, player_->time_pos(), player_->duration(), true);
        }
    }

    // Largest video-aspect rectangle inside the window (full) or the tile (mini), in
    // physical pixels (§7.2).
    void apply_target() {
        auto ui = lock();
        if (!ui) return;
        auto& win = (*ui)->window();
        int dst_w = 0;
        int dst_h = 0;
        if (mode_ == RenderTargetMode::Mini) {
            const float scale = win.scale_factor();
            dst_w = static_cast<int>(std::lround((*ui)->get_mini_w() * scale));
            dst_h = static_cast<int>(std::lround((*ui)->get_mini_h() * scale));
        } else {
            const auto phys = win.size();
            dst_w = static_cast<int>(phys.width);
            dst_h = static_cast<int>(phys.height);
        }
        if (dst_w <= 0 || dst_h <= 0) return; // window not shown yet
        const auto [w, h] = mpvx::fit_aspect(dst_w, dst_h, aspect_);
        renderer_->set_target_size(w, h);
    }

    void settle_shrink(uint64_t gen) {
        if (gen != target_gen_ || mode_ != RenderTargetMode::FullToMini) return;
        auto ui = lock();
        if (!ui) return;
        mode_ = RenderTargetMode::Mini;
        apply_target();
        (void)player_->set_mini_profile(true);
        if ((*ui)->get_mini_state() == MiniState::Tile) {
            (*ui)->set_mini_shadow(1.0f); // a pill (minimized mid-shrink) has no shadow
        }
        update_frame_cap();
    }

    void settle_restore(uint64_t gen) {
        // While the shrink is still settling, settle_shrink() turns the shadow on instead.
        if (gen != shadow_gen_ || mode_ != RenderTargetMode::Mini) return;
        if (auto ui = lock(); ui && (*ui)->get_mini_state() == MiniState::Tile) {
            (*ui)->set_mini_shadow(1.0f);
        }
    }

    // Playback went idle or failed while mini: fade out in place, then leave mini mode (§8).
    void close_on_idle() {
        auto ui = lock();
        if (!ui) return;
        const uint64_t gen = ++target_gen_;
        ++shadow_gen_;
        renderer_->set_frame_cap(false);
        (*ui)->set_mini_shadow(0.0f);
        (*ui)->set_mini_closing(true);

        const auto fade_ms = (*ui)->global<Theme>().get_mini_close_fade_duration();
        slint::Timer::single_shot(std::chrono::milliseconds(fade_ms + kSettleSlackMs),
                                  [weak = weak_from_this(), gen]() {
                                      auto self = weak.lock();
                                      if (!self || gen != self->target_gen_) return;
                                      auto ui = self->lock();
                                      if (!ui) return;
                                      (*ui)->set_mini_state(MiniState::Hidden);
                                      (*ui)->set_mini_closing(false);
                                      self->mode_ = RenderTargetMode::Full;
                                      self->apply_target();
                                      (void)self->player_->set_mini_profile(false);
                                  });
    }

    // Undo close_on_idle() before its timer fires: fade back in, shadow and cap as before.
    void cancel_close() {
        auto ui = lock();
        if (!ui) return;
        ++target_gen_; // drops the pending close timer
        (*ui)->set_mini_closing(false);
        if (mode_ == RenderTargetMode::FullToMini) {
            // The close dropped the shrink's settle timer: settle once the morph is surely over.
            const uint64_t gen = target_gen_;
            const auto shrink_ms = (*ui)->global<Theme>().get_mini_shrink_duration();
            slint::Timer::single_shot(std::chrono::milliseconds(shrink_ms + kSettleSlackMs),
                                      [weak = weak_from_this(), gen]() {
                                          if (auto self = weak.lock()) self->settle_shrink(gen);
                                      });
            return;
        }
        if (mode_ == RenderTargetMode::Mini && (*ui)->get_mini_state() == MiniState::Tile) {
            (*ui)->set_mini_shadow(1.0f);
        }
        update_frame_cap();
    }

    // The cap applies only to a playing tile (§3.6); every other state runs at full rate.
    void update_frame_cap() {
        auto ui = lock();
        const bool tile = ui && (*ui)->get_mini_state() == MiniState::Tile;
        renderer_->set_frame_cap(fps_cap_ && tile && mode_ == RenderTargetMode::Mini);
    }

    slint::ComponentWeakHandle<MainWindow> ui_;
    std::shared_ptr<mpvx::Player> player_;
    std::shared_ptr<mpvx::VideoRenderer> renderer_;
    std::shared_ptr<mpvx::LibraryService> library_;
    std::shared_ptr<std::string> active_media_path_;

    RenderTargetMode mode_{RenderTargetMode::Full};
    uint64_t target_gen_{0}; // guards render-target settle and idle-close timers
    uint64_t shadow_gen_{0}; // guards shadow fade-in timers
    double aspect_{0.0};
    bool fps_cap_{false};
};

} // namespace

int main(int argc, char* argv[]) {
    std::println("[{}] Starting {} v{} (C++26)", mpvx::APP_NAME, mpvx::APP_NAME, mpvx::APP_VERSION);

    auto main_window = MainWindow::create();
    auto ui_weak = slint::ComponentWeakHandle(main_window);

    auto player = std::make_shared<mpvx::Player>();
    auto renderer = std::make_shared<mpvx::VideoRenderer>();
    auto library = std::make_shared<mpvx::LibraryService>();
    auto picker_ui = std::make_shared<PickerUiController>(main_window);
    auto active_media_path = std::make_shared<std::string>();

    auto init_res = player->initialize();
    if (!init_res.has_value()) {
        std::println(stderr, "[mpvx] Error: Could not initialize MPV core: {}", init_res.error().message);
        return 1;
    }

    // Initialize LibraryService off the GUI thread (Zero UI Locks)
    mpvx::LibraryServiceCallbacks lib_callbacks;
    lib_callbacks.on_snapshot = [picker_ui](std::shared_ptr<const mpvx::LibraryViewSnapshot> snap) {
        picker_ui->apply_snapshot(snap);
    };
    lib_callbacks.on_thumbnail_updates = [picker_ui](std::vector<mpvx::ThumbnailUpdate> updates) {
        picker_ui->apply_thumbnail_updates(std::move(updates));
    };
    std::weak_ptr<mpvx::LibraryService> lib_weak = library;
    lib_callbacks.on_schedule_ui_flush = [lib_weak]() {
        slint::invoke_from_event_loop([lib_weak]() {
            if (auto lib = lib_weak.lock()) {
                lib->flush_pending_updates_on_gui_thread();
            }
        });
    };
    library->initialize(std::move(lib_callbacks));

    // Mini player state machine; also owns render-target sizing (largest video-aspect
    // rectangle in physical pixels, so mpv never paints letterbox bars). GUI-thread only.
    auto mini = std::make_shared<MiniPlayerController>(ui_weak, player, renderer, library, active_media_path);

    // Initialize software video renderer -> Slint Image (coalesced to prevent UI queue saturation)
    auto frame_in_flight = std::make_shared<std::atomic<bool>>(false);
    auto render_init = renderer->initialize(player->handle(), 1280, 720);
    if (render_init.has_value()) {
        renderer->set_frame_callback([ui_weak, frame_in_flight](const mpvx::VideoRenderer::FrameBuffer& buffer) {
            if (frame_in_flight->exchange(true, std::memory_order_acq_rel)) {
                return; // Coalesce frame if GUI thread hasn't painted the previous one yet
            }
            slint::invoke_from_event_loop([ui_weak, frame_in_flight, buffer]() {
                frame_in_flight->store(false, std::memory_order_release);
                if (auto ui_opt = ui_weak.lock()) {
                    auto ui = *ui_opt;
                    ui->set_video_frame(slint::Image(buffer));
                    ui->set_has_video(true);
                }
            });
        });
    }

    // Helper to refresh normalized A/B markers on the UI thread
    auto refresh_ab_markers = [ui_weak, player]() {
        auto state = player->ab_loop_state();
        auto a = player->ab_loop_a();
        auto b = player->ab_loop_b();
        double dur = player->duration();

        float a_norm = (a.has_value() && dur > 0.0) ? static_cast<float>(std::clamp(*a / dur, 0.0, 1.0))
                     : a.has_value() ? 0.0f : -1.0f;
        float b_norm = (b.has_value() && dur > 0.0) ? static_cast<float>(std::clamp(*b / dur, 0.0, 1.0))
                     : b.has_value() ? 1.0f : -1.0f;

        slint::invoke_from_event_loop([ui_weak, state, a_norm, b_norm]() {
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                ui->set_ab_loop_state(to_slint_ab_state(state));
                ui->set_ab_loop_a_norm(a_norm);
                ui->set_ab_loop_b_norm(b_norm);
            }
        });
    };

    // Helper to refresh normalized chapter ticks and active chapter flag on the UI thread
    auto refresh_chapters = [ui_weak, player]() {
        auto chapters = player->chapters();
        double dur = player->duration();
        int active_chapter = player->current_chapter();

        slint::invoke_from_event_loop([ui_weak, chapters, dur, active_chapter]() {
            auto ui_opt = ui_weak.lock();
            if (!ui_opt) return;
            auto ui = *ui_opt;

            std::vector<ChapterItem> items;
            items.reserve(chapters.size());
            for (const auto& ch : chapters) {
                float norm = (dur > 0.0) ? static_cast<float>(std::clamp(ch.time / dur, 0.0, 1.0)) : 0.0f;
                items.push_back(ChapterItem{
                    ch.index,
                    slint::SharedString(ch.title),
                    slint::SharedString(mpvx::format_timestamp(ch.time)),
                    norm,
                    ch.index == active_chapter
                });
            }
            ui->set_chapters(std::make_shared<slint::VectorModel<ChapterItem>>(std::move(items)));
        });
    };

    // Set up player event observers -> Thread-safe UI updates
    mpvx::PlayerCallbacks callbacks;
    callbacks.on_state_changed = [ui_weak, library, player, renderer, mini, active_media_path](mpvx::PlaybackState state) {
        if (state == mpvx::PlaybackState::Paused && !active_media_path->empty()) {
            library->record_playback_progress(*active_media_path, player->time_pos(), player->duration(), true);
        }
        if (state == mpvx::PlaybackState::Idle) {
            // Playback ended without an explicit stop (e.g. error): keep libmpv's clear frame
            // and any resize redraws away from the UI until the next video is configured.
            renderer->suspend_output();
        }
        slint::invoke_from_event_loop([ui_weak, player, mini]() {
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                const auto latest = player->state();
                ui->set_is_playing(latest == mpvx::PlaybackState::Playing);
                if (latest == mpvx::PlaybackState::Idle) {
                    ui->set_has_video(false);
                    ui->set_is_buffering(false);
                }
                mini->on_playback_state(latest);
            }
        });
    };

    callbacks.on_buffering_changed = [ui_weak](bool buffering) {
        slint::invoke_from_event_loop([ui_weak, buffering]() {
            if (auto ui_opt = ui_weak.lock()) {
                (*ui_opt)->set_is_buffering(buffering);
            }
        });
    };

    callbacks.on_time_pos = [ui_weak, player, library, active_media_path](double pos) {
        double dur = player->duration();
        if (!active_media_path->empty() && dur > 0.0) {
            library->record_playback_progress(*active_media_path, pos, dur, false);
        }
        float progress = dur > 0.0 ? static_cast<float>(std::clamp(pos / dur, 0.0, 1.0)) : 0.0f;
        std::string time_str = mpvx::format_time_readout(pos, dur);

        slint::invoke_from_event_loop([ui_weak, progress, time_str]() {
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                ui->set_progress(progress);
                ui->set_time_text(slint::SharedString(time_str));
            }
        });
    };

    callbacks.on_duration = [ui_weak, player, refresh_ab_markers, refresh_chapters](double dur) {
        double pos = player->time_pos();
        std::string time_str = mpvx::format_time_readout(pos, dur);

        slint::invoke_from_event_loop([ui_weak, time_str]() {
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                ui->set_time_text(slint::SharedString(time_str));
            }
        });
        refresh_ab_markers();
        refresh_chapters();
    };

    callbacks.on_media_title = [ui_weak](const std::string& title) {
        slint::invoke_from_event_loop([ui_weak, title]() {
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                ui->set_media_title(slint::SharedString(title));
            }
        });
    };

    callbacks.on_volume = [ui_weak](double vol) {
        slint::invoke_from_event_loop([ui_weak, vol]() {
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                ui->set_volume(static_cast<float>(vol));
            }
        });
    };

    callbacks.on_mute = [ui_weak](bool muted) {
        slint::invoke_from_event_loop([ui_weak, muted]() {
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                ui->set_is_muted(muted);
            }
        });
    };

    callbacks.on_file_loaded = [ui_weak, renderer](const std::string& /*file*/) {
        // Loading a file ends the previous one (END_FILE -> Idle -> suspend_output()), so
        // frame delivery must resume for every newly loaded file. video-out-params alone is
        // not enough: mpv deduplicates property changes, so a next file with the same
        // display size may produce no video-size event at all and output would stay
        // suspended (stuck last frame, "mpv_render_context_render() not being called").
        // FILE_LOADED fires after the old file's clear frame and never on stop, and before
        // the new file's first frame, so anything rendered while suspended is that clear
        // frame: drop it instead of re-rendering it (a black flash before the new file).
        renderer->resume_output(mpvx::VideoRenderer::ResumeFrom::NextFrame);
        slint::invoke_from_event_loop([ui_weak]() {
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                ui->set_has_video(true);
            }
        });
    };

    callbacks.on_tracks_changed = [ui_weak](const std::vector<mpvx::MediaTrack>& tracks) {
        slint::invoke_from_event_loop([ui_weak, tracks]() {
            auto ui_opt = ui_weak.lock();
            if (!ui_opt) return;
            auto ui = *ui_opt;

            std::vector<TrackItem> sub_items;
            std::vector<TrackItem> audio_items;
            bool subs_enabled = false;
            int active_sub_idx = -1;

            for (const auto& t : tracks) {
                TrackItem item{
                    t.id,
                    slint::SharedString(t.display_label()),
                    slint::SharedString(t.lang),
                    slint::SharedString(t.codec),
                    t.selected
                };
                if (t.type == "sub") {
                    if (t.selected) {
                        subs_enabled = true;
                        active_sub_idx = static_cast<int>(sub_items.size());
                    }
                    sub_items.push_back(std::move(item));
                } else if (t.type == "audio") {
                    audio_items.push_back(std::move(item));
                }
            }

            ui->set_subtitle_tracks(std::make_shared<slint::VectorModel<TrackItem>>(std::move(sub_items)));
            ui->set_audio_tracks(std::make_shared<slint::VectorModel<TrackItem>>(std::move(audio_items)));
            ui->set_has_subtitles_enabled(subs_enabled);
            ui->set_active_subtitle_index(active_sub_idx);
        });
    };

    callbacks.on_chapters_changed = [refresh_chapters](const std::vector<mpvx::Chapter>&) {
        refresh_chapters();
    };

    callbacks.on_chapter_changed = [refresh_chapters](int) {
        refresh_chapters();
    };

    callbacks.on_ab_loop_changed = [refresh_ab_markers](mpvx::AbLoopState, std::optional<double>, std::optional<double>) {
        refresh_ab_markers();
    };

    callbacks.on_loop_mode_changed = [ui_weak](mpvx::LoopMode mode) {
        slint::invoke_from_event_loop([ui_weak, mode]() {
            if (auto ui_opt = ui_weak.lock()) {
                (*ui_opt)->set_loop_mode(to_slint_loop_mode(mode));
            }
        });
    };

    callbacks.on_speed_changed = [ui_weak](double speed) {
        slint::invoke_from_event_loop([ui_weak, speed]() {
            if (auto ui_opt = ui_weak.lock()) {
                (*ui_opt)->set_playback_speed(static_cast<float>(speed));
            }
        });
    };

    callbacks.on_video_size = [renderer, mini](mpvx::VideoDisplaySize size) {
        if (size.valid()) {
            renderer->resume_output(); // video (re)configured, e.g. after a track change
        }
        slint::invoke_from_event_loop([mini, size]() { mini->set_video_size(size); });
    };

    player->set_callbacks(std::move(callbacks));

    // --- Bind UI Callbacks -> Player & Window Actions ---
    main_window->on_play_pause_clicked([player, ui_weak]() {
        if (player->state() == mpvx::PlaybackState::Idle) {
            if (auto ui_opt = ui_weak.lock()) {
                (*ui_opt)->set_active_popover(ActivePopover::OpenUrlModal);
            }
            return;
        }
        (void)player->toggle_pause();
    });

    main_window->on_prev_clicked([player]() {
        (void)player->playlist_prev();
    });

    main_window->on_next_clicked([player]() {
        (void)player->playlist_next();
    });

    main_window->on_stop_clicked([player, renderer, ui_weak]() {
        // Suspend before stopping: libmpv renders a black clear frame right after `stop`,
        // which would otherwise set has-video back to true.
        renderer->suspend_output();
        (void)player->stop();
        if (auto ui_opt = ui_weak.lock()) {
            auto ui = *ui_opt;
            ui->set_is_playing(false);
            ui->set_has_video(false);
            ui->set_is_buffering(false);
            ui->set_progress(0.0f);
            ui->set_time_text(slint::SharedString("0:00 | 0:00"));
            ui->set_media_title(slint::SharedString(""));
        }
    });

    main_window->on_seek_requested([player, ui_weak](float fraction) {
        double dur = player->duration();
        if (dur > 0.0) {
            float clamped = std::clamp(fraction, 0.0f, 1.0f);
            double target = static_cast<double>(clamped) * dur;
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                ui->set_is_buffering(true);
                ui->set_progress(clamped);
                ui->set_time_text(slint::SharedString(mpvx::format_time_readout(target, dur)));
            }
            (void)player->seek(target, true);
        }
    });

    main_window->on_relative_seek_requested([player](float delta_secs) {
        (void)player->seek_relative(static_cast<double>(delta_secs));
    });

    main_window->on_volume_changed([player](float vol) {
        (void)player->set_volume(static_cast<double>(vol));
    });

    main_window->on_mute_toggled([player]() {
        (void)player->set_mute(!player->is_muted());
    });

    main_window->on_ab_loop_cycled([player]() {
        (void)player->cycle_ab_loop();
    });

    main_window->on_ab_loop_cleared([player]() {
        (void)player->clear_ab_loop();
    });

    main_window->on_loop_mode_cycled([player]() {
        mpvx::LoopMode next = mpvx::LoopMode::Off;
        switch (player->loop_mode()) {
            case mpvx::LoopMode::Off:
                next = mpvx::LoopMode::File;
                break;
            case mpvx::LoopMode::File:
                next = mpvx::LoopMode::Playlist;
                break;
            case mpvx::LoopMode::Playlist:
                next = mpvx::LoopMode::Off;
                break;
        }
        (void)player->set_loop_mode(next);
    });

    main_window->on_subtitle_track_selected([player](int sid) {
        (void)player->set_subtitle_track(sid);
    });

    main_window->on_load_external_subtitle_clicked([player, library, ui_weak]() {
        library->open_media_file_dialog("Select External Subtitle File", [player, ui_weak](std::optional<std::string> chosen) {
            if (chosen.has_value()) {
                std::string path = std::move(*chosen);
                slint::invoke_from_event_loop([player, path = std::move(path)]() {
                    (void)player->add_subtitle_file(path);
                });
            } else {
                slint::invoke_from_event_loop([ui_weak]() {
                    if (auto ui_opt = ui_weak.lock()) {
                        (*ui_opt)->set_active_popover(ActivePopover::OpenUrlModal);
                    }
                });
            }
        });
    });

    main_window->on_audio_track_selected([player](int aid) {
        (void)player->set_audio_track(aid);
    });

    main_window->on_chapter_selected([player](int index) {
        (void)player->set_chapter(index);
    });

    main_window->on_speed_changed([player](float speed) {
        (void)player->set_speed(static_cast<double>(speed));
    });

    main_window->on_hwdec_changed([player](slint::SharedString mode) {
        (void)player->set_hwdec(std::string_view(mode));
    });

    main_window->on_aspect_changed([player](slint::SharedString aspect) {
        (void)player->set_aspect_override(std::string_view(aspect));
    });

    main_window->on_open_url_submitted([player, library, ui_weak, mini, active_media_path](slint::SharedString url) {
        std::string target(url);
        if (target.empty()) return;
        mini->leave_for_load();
        if (!active_media_path->empty()) {
            library->record_playback_progress(*active_media_path, player->time_pos(), player->duration(), true);
        }
        *active_media_path = target;
        player->clear_pending_resume();
        std::println("[mpvx] Loading media: {}", target);
        if (auto ui_opt = ui_weak.lock()) {
            auto ui = *ui_opt;
            ui->set_media_title(slint::SharedString(target));
            ui->set_view_mode(AppViewMode::Player);
            ui->set_hud_visible(true);
            ui->set_pointer_idle_ms(0);
        }
        (void)player->load_file(target);
        (void)player->play();
    });

    main_window->on_open_file_clicked([player, library, ui_weak, mini, active_media_path]() {
        library->open_media_file_dialog("Open Media File", [player, library, ui_weak, mini, active_media_path](std::optional<std::string> chosen) {
            if (chosen.has_value()) {
                std::string path = std::move(*chosen);
                slint::invoke_from_event_loop([player, library, ui_weak, mini, active_media_path, path = std::move(path)]() {
                    mini->leave_for_load();
                    if (!active_media_path->empty()) {
                        library->record_playback_progress(*active_media_path, player->time_pos(), player->duration(), true);
                    }
                    *active_media_path = path;
                    player->clear_pending_resume();
                    if (auto ui_opt = ui_weak.lock()) {
                        auto ui = *ui_opt;
                        ui->set_media_title(slint::SharedString(path));
                        ui->set_view_mode(AppViewMode::Player);
                        ui->set_hud_visible(true);
                        ui->set_pointer_idle_ms(0);
                    }
                    (void)player->load_file(path);
                    (void)player->play();
                });
            } else {
                slint::invoke_from_event_loop([ui_weak]() {
                    if (auto ui_opt = ui_weak.lock()) {
                        (*ui_opt)->set_active_popover(ActivePopover::OpenUrlModal);
                    }
                });
            }
        });
    });

    // --- Video Picker Callbacks ---
    main_window->on_picker_nav_selected([library](PickerNavKind kind, slint::SharedString path) {
        library->navigate(from_slint_nav_kind(kind), std::string(path));
    });

    main_window->on_picker_folder_toggled([library](slint::SharedString path) {
        library->toggle_folder_expanded(std::string(path));
    });

    main_window->on_picker_breadcrumb_clicked([library, picker_ui](int idx) {
        if (auto p = picker_ui->breadcrumb_path_at(idx)) {
            if (*p == library->videos_root().string()) {
                library->navigate(mpvx::LibraryNavKind::Home);
            } else {
                library->navigate(mpvx::LibraryNavKind::Folder, std::move(*p));
            }
        }
    });

    main_window->on_picker_navigate_up([library]() {
        library->navigate_up();
    });

    main_window->on_picker_video_clicked([player, library, ui_weak, mini, active_media_path](slint::SharedString path, bool resume) {
        std::string target(path);
        if (target.empty()) return;
        mini->leave_for_load(); // from the tile, the morph starts at the tile (§3.3)
        if (!active_media_path->empty() && *active_media_path != target) {
            library->record_playback_progress(*active_media_path, player->time_pos(), player->duration(), true);
        }
        *active_media_path = target;

        if (resume) {
            if (auto entry = library->get_watch_entry(target); entry.has_value() && entry->is_in_progress()) {
                player->set_pending_resume(entry->position_sec);
            } else {
                player->clear_pending_resume();
            }
        } else {
            player->clear_pending_resume();
        }

        if (auto ui_opt = ui_weak.lock()) {
            auto ui = *ui_opt;
            ui->set_media_title(slint::SharedString(mpvx::normalize_video_title(
                std::filesystem::path(target).filename().string())));
            ui->set_view_mode(AppViewMode::Player);
            ui->set_hud_visible(true);
            ui->set_pointer_idle_ms(0);
        }
        (void)player->load_file(target);
        (void)player->play();
    });

    main_window->on_picker_mark_watched([library](slint::SharedString path, bool watched) {
        library->mark_watched(std::string_view(path), watched);
    });

    main_window->on_picker_reveal_in_file_manager([library](slint::SharedString path) {
        library->reveal_in_file_manager(std::string_view(path));
    });

    main_window->on_picker_request_thumbnail([library](slint::SharedString id_hex, slint::SharedString path) {
        library->request_thumbnail(std::string_view(id_hex), std::string_view(path));
    });

    main_window->on_picker_grid_columns_changed([picker_ui](int cols) {
        picker_ui->set_grid_columns(cols);
    });

    main_window->on_picker_search_changed([library](slint::SharedString query) {
        library->set_search_query(std::string(query));
    });

    main_window->on_picker_sort_changed([library](PickerSortMode mode) {
        library->set_sort_mode(from_slint_sort_mode(mode));
    });

    main_window->on_picker_open_external_folder([library]() {
        library->open_external_folder_dialog([library](std::string dir) {
            slint::invoke_from_event_loop([library, dir = std::move(dir)]() {
                library->navigate(mpvx::LibraryNavKind::Folder, dir);
            });
        });
    });

    // --- Mini Player (docs/mini-player-ui-spec.md) ---
    // Playback no longer pauses when the library opens; it continues in the mini player.
    main_window->on_return_to_picker_clicked([mini]() { mini->enter_picker(); });
    main_window->on_return_to_player_clicked([mini]() { mini->expand(); });
    main_window->on_mini_expand_clicked([mini]() { mini->expand(); });
    main_window->on_mini_minimize_clicked([mini]() { mini->minimize(); });
    main_window->on_mini_restore_clicked([mini](bool resume) { mini->restore(resume); });
    main_window->on_mini_geometry_changed([mini]() { mini->on_mini_geometry_changed(); });
    main_window->on_mini_fps_cap_changed([mini](bool enabled) { mini->set_fps_cap(enabled); });

    // Frameless Window Management
    main_window->on_minimize_clicked([ui_weak]() {
        if (auto ui_opt = ui_weak.lock()) {
            (*ui_opt)->window().set_minimized(true);
        }
    });

    main_window->on_maximize_toggled([ui_weak]() {
        if (auto ui_opt = ui_weak.lock()) {
            auto ui = *ui_opt;
            bool next = !ui->window().is_maximized();
            ui->window().set_maximized(next);
            ui->set_is_maximized(next);
        }
    });

    main_window->on_fullscreen_toggled([ui_weak]() {
        if (auto ui_opt = ui_weak.lock()) {
            auto ui = *ui_opt;
            bool next = !ui->window().is_fullscreen();
            ui->window().set_fullscreen(next);
            ui->set_is_fullscreen(next);
        }
    });

    main_window->on_close_clicked([]() {
        slint::quit_event_loop();
    });

    main_window->on_window_drag_requested([ui_weak](float dx, float dy) {
        if (auto ui_opt = ui_weak.lock()) {
            auto& win = (*ui_opt)->window();
            if (win.is_maximized() || win.is_fullscreen()) return;
            float scale = win.scale_factor();
            auto pos = win.position();
            win.set_position(slint::PhysicalPosition({
                pos.x + static_cast<int>(std::lround(dx * scale)),
                pos.y + static_cast<int>(std::lround(dy * scale))
            }));
        }
    });

    // The Slint callback reports logical pixels; the controller reads the window's physical
    // size directly so HiDPI output is rendered at native resolution.
    main_window->on_viewport_resized([mini](int /*logical_w*/, int /*logical_h*/) {
        mini->on_window_resized();
    });

    // Check CLI argument
    if (argc > 1) {
        std::string arg = argv[1];
        std::println("[mpvx] Loading media from argument: {}", arg);
        *active_media_path = arg;
        main_window->set_view_mode(AppViewMode::Player);
        main_window->set_media_title(slint::SharedString(arg));
        (void)player->load_file(arg);
    } else {
        main_window->set_view_mode(AppViewMode::Picker);
    }

    std::println("[mpvx] Launching GUI event loop...");
    main_window->run();

    std::println("[mpvx] Window closed, shutting down...");
    if (!active_media_path->empty()) {
        library->record_playback_progress(*active_media_path, player->time_pos(), player->duration(), true);
    }
    library->shutdown();
    const auto render_stats = renderer->stats();
    std::println("[mpvx] Renderer: {} frames rendered, {} pixel-buffer allocations",
                 render_stats.frames_rendered, render_stats.buffer_allocations);
    renderer->shutdown();
    player->shutdown();

    return 0;
}
