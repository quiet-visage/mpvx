#include "app-window.h"
#include "mpvx/core.hpp"
#include "mpvx/player.hpp"
#include "mpvx/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <vector>

namespace {

// Attempts to open a native desktop file picker asynchronously via zenity or kdialog if available;
// returns std::nullopt if no GUI picker utility is installed or user cancels.
std::optional<std::string> run_native_file_dialog(const char* title) {
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if ((!display || display[0] == '\0') && (!wayland || wayland[0] == '\0')) {
        return std::nullopt;
    }

    std::string cmd = std::format(
        "if command -v zenity >/dev/null 2>&1; then "
        "zenity --file-selection --title=\"{}\" 2>/dev/null; "
        "elif command -v kdialog >/dev/null 2>&1; then "
        "kdialog --getopenfilename . --title \"{}\" 2>/dev/null; "
        "else exit 127; fi",
        title, title);

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::nullopt;
    }

    char buffer[4096];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    int status = pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }

    if (status == 0 && !output.empty()) {
        return output;
    }
    return std::nullopt;
}

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

} // namespace

int main(int argc, char* argv[]) {
    std::println("[{}] Starting {} v{} (C++26)", mpvx::APP_NAME, mpvx::APP_NAME, mpvx::APP_VERSION);

    auto main_window = MainWindow::create();
    auto ui_weak = slint::ComponentWeakHandle(main_window);

    auto player = std::make_shared<mpvx::Player>();
    auto renderer = std::make_shared<mpvx::VideoRenderer>();

    auto init_res = player->initialize();
    if (!init_res.has_value()) {
        std::println(stderr, "[mpvx] Error: Could not initialize MPV core: {}", init_res.error().message);
        return 1;
    }

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
    callbacks.on_state_changed = [ui_weak](mpvx::PlaybackState state) {
        slint::invoke_from_event_loop([ui_weak, state]() {
            if (auto ui_opt = ui_weak.lock()) {
                auto ui = *ui_opt;
                ui->set_is_playing(state == mpvx::PlaybackState::Playing);
                if (state == mpvx::PlaybackState::Idle) {
                    ui->set_has_video(false);
                    ui->set_is_buffering(false);
                }
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

    callbacks.on_time_pos = [ui_weak, player](double pos) {
        double dur = player->duration();
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

    callbacks.on_file_loaded = [ui_weak](const std::string& /*file*/) {
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

    main_window->on_stop_clicked([player, ui_weak]() {
        (void)player->stop();
        if (auto ui_opt = ui_weak.lock()) {
            auto ui = *ui_opt;
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

    main_window->on_load_external_subtitle_clicked([player, ui_weak]() {
        std::thread([player, ui_weak]() {
            if (auto chosen = run_native_file_dialog("Select External Subtitle File")) {
                std::string path = *chosen;
                slint::invoke_from_event_loop([player, path]() {
                    (void)player->add_subtitle_file(path);
                });
            } else {
                slint::invoke_from_event_loop([ui_weak]() {
                    if (auto ui_opt = ui_weak.lock()) {
                        (*ui_opt)->set_active_popover(ActivePopover::OpenUrlModal);
                    }
                });
            }
        }).detach();
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

    main_window->on_open_url_submitted([player, ui_weak](slint::SharedString url) {
        std::string target(url);
        if (target.empty()) return;
        std::println("[mpvx] Loading media: {}", target);
        if (auto ui_opt = ui_weak.lock()) {
            (*ui_opt)->set_media_title(slint::SharedString(target));
        }
        (void)player->load_file(target);
    });

    main_window->on_open_file_clicked([player, ui_weak]() {
        std::thread([player, ui_weak]() {
            if (auto chosen = run_native_file_dialog("Open Media File")) {
                std::string path = *chosen;
                slint::invoke_from_event_loop([player, ui_weak, path]() {
                    if (auto ui_opt = ui_weak.lock()) {
                        (*ui_opt)->set_media_title(slint::SharedString(path));
                    }
                    (void)player->load_file(path);
                });
            } else {
                slint::invoke_from_event_loop([ui_weak]() {
                    if (auto ui_opt = ui_weak.lock()) {
                        (*ui_opt)->set_active_popover(ActivePopover::OpenUrlModal);
                    }
                });
            }
        }).detach();
    });

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

    main_window->on_viewport_resized([renderer](int w, int h) {
        int clamped_w = std::clamp(w, 320, 3840);
        int clamped_h = std::clamp(h, 240, 2160);
        renderer->resize(clamped_w, clamped_h);
    });

    // Check CLI argument
    if (argc > 1) {
        std::string arg = argv[1];
        std::println("[mpvx] Loading media from argument: {}", arg);
        main_window->set_media_title(slint::SharedString(arg));
        (void)player->load_file(arg);
    }

    std::println("[mpvx] Launching GUI event loop...");
    main_window->run();

    std::println("[mpvx] Window closed, shutting down...");
    renderer->shutdown();
    player->shutdown();

    return 0;
}
