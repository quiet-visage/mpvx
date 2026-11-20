#pragma once

#include "mpvx/core.hpp"

#include <slint.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// Forward declaration from libmpv
struct mpv_handle;
struct mpv_render_context;

namespace mpvx {

// Largest rectangle with the given display aspect ratio (width / height) that fits
// inside dst_w x dst_h. Rendering mpv into this rectangle (instead of the full
// destination) means mpv never paints letterbox bars into the frame; the UI's
// `image-fit: contain` letterboxes on the GPU instead. A non-positive or non-finite
// aspect returns the destination unchanged.
[[nodiscard]] constexpr std::pair<int, int> fit_aspect(int dst_w, int dst_h, double aspect) noexcept {
    if (dst_w <= 0 || dst_h <= 0 || !(aspect > 0.0) || aspect > 1e6) {
        return {dst_w, dst_h};
    }
    const auto w_from_h = static_cast<int>(static_cast<double>(dst_h) * aspect + 0.5);
    if (w_from_h <= dst_w) {
        return {w_from_h > 0 ? w_from_h : 1, dst_h};
    }
    const auto h_from_w = static_cast<int>(static_cast<double>(dst_w) / aspect + 0.5);
    return {dst_w, h_from_w > 0 ? (h_from_w < dst_h ? h_from_w : dst_h) : 1};
}

// Clamps a render target to the renderer's supported range while preserving its
// aspect ratio (uniform downscale when either side exceeds the maximum).
[[nodiscard]] constexpr std::pair<int, int> clamp_render_target(int w, int h) noexcept {
    constexpr int kMinSide = 16;
    constexpr int kMaxW = 3840;
    constexpr int kMaxH = 2160;
    if (w <= 0 || h <= 0) {
        return {kMinSide, kMinSide};
    }
    if (w > kMaxW || h > kMaxH) {
        const double scale = std::min(static_cast<double>(kMaxW) / w, static_cast<double>(kMaxH) / h);
        w = static_cast<int>(w * scale);
        h = static_cast<int>(h * scale);
    }
    return {w < kMinSide ? kMinSide : w, h < kMinSide ? kMinSide : h};
}

class VideoRenderer {
public:
    using FrameBuffer = slint::SharedPixelBuffer<slint::Rgba8Pixel>;
    using FrameCallback = std::function<void(const FrameBuffer&)>;

    struct Stats {
        uint64_t frames_rendered{0};
        // Pixel-buffer allocations, including copy-on-write detaches that happen when a
        // ring slot is still referenced by the GUI. Stays flat in steady state.
        uint64_t buffer_allocations{0};
        // Frames handed to the frame callback (lower than frames_rendered under a frame cap).
        uint64_t frames_delivered{0};
    };

    VideoRenderer();
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    [[nodiscard]] Result<void> initialize(mpv_handle* mpv, int width = 1280, int height = 720);
    void shutdown();

    // Sets the render target size in *physical* pixels (clamped, aspect preserved).
    // If the size changes, the current frame is re-rendered at the new size even while
    // playback is paused (see request_redraw()).
    void set_target_size(int width, int height);

    // Re-renders the current frame without waiting for mpv to produce a new one.
    // No-op until mpv has rendered at least one frame since the last suspend_output() or
    // resume_output(ResumeFrom::NextFrame), and while output is suspended.
    void request_redraw() noexcept;

    // Stops delivering frames to the frame callback. Call *before* stopping playback:
    // after `stop`, libmpv usually renders one black clear frame, which must not reach
    // the UI. Frames are still rendered (vo_libmpv blocks until every queued frame is
    // rendered), only their delivery is suppressed.
    void suspend_output() noexcept;

    // Resumes frame delivery.
    // - ResumeFrom::Latest (a video was configured): re-renders the latest frame, so a first
    //   frame rendered just before resuming is not lost.
    // - ResumeFrom::NextFrame (a new file was loaded): forgets frames rendered while
    //   suspended, which are libmpv's clear frame from the previous file's stop/END_FILE,
    //   and delivers from the next frame mpv renders.
    enum class ResumeFrom : uint8_t { Latest, NextFrame };
    void resume_output(ResumeFrom from = ResumeFrom::Latest) noexcept;

    void set_frame_callback(FrameCallback callback);

    // Mini player 30 fps cap: when enabled, frames rendered less than 29 ms after the last
    // delivered one are not handed to the frame callback (60 -> 30, 50 -> 25; sources at
    // 30 fps or below are unaffected). mpv still renders every frame, so A/V pacing is unchanged.
    void set_frame_cap(bool enabled) noexcept;

    [[nodiscard]] int width() const noexcept { return unpack_w(target_.load(std::memory_order_relaxed)); }
    [[nodiscard]] int height() const noexcept { return unpack_h(target_.load(std::memory_order_relaxed)); }
    [[nodiscard]] bool is_initialized() const noexcept { return render_ctx_ != nullptr; }
    [[nodiscard]] Stats stats() const noexcept;

    void notify_update() noexcept;
    void render_frame();

private:
    static constexpr uint64_t pack(int w, int h) noexcept {
        return (static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32) | static_cast<uint32_t>(h);
    }
    static constexpr int unpack_w(uint64_t v) noexcept { return static_cast<int>(v >> 32); }
    static constexpr int unpack_h(uint64_t v) noexcept { return static_cast<int>(v & 0xFFFFFFFFu); }

    void render_loop();
    FrameBuffer& acquire_slot(int w, int h);

    mpv_handle* mpv_{nullptr};
    mpv_render_context* render_ctx_{nullptr};

    // Width and height packed into one atomic so the render thread never sees a torn size.
    std::atomic<uint64_t> target_{pack(1280, 720)};

    std::atomic<bool> running_{false};
    std::atomic<bool> update_pending_{false};
    std::atomic<bool> force_redraw_{false};
    std::atomic<bool> has_frame_{false};
    std::atomic<bool> output_enabled_{true};
    std::mutex cv_mutex_;
    std::condition_variable cv_;
    std::thread render_thread_;

    // Render-thread only: 3 slots cover one frame displayed by the GUI, one in flight
    // through invoke_from_event_loop, and one being written.
    std::array<FrameBuffer, 3> ring_{};
    size_t ring_index_{0};

    std::atomic<uint64_t> frames_rendered_{0};
    std::atomic<uint64_t> buffer_allocations_{0};
    std::atomic<uint64_t> frames_delivered_{0};

    std::atomic<int64_t> min_handoff_interval_ns_{0};
    std::chrono::steady_clock::time_point last_handoff_{}; // render thread only

    std::mutex cb_mutex_;
    FrameCallback frame_callback_;
};

} // namespace mpvx
