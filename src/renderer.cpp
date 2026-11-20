#include "mpvx/renderer.hpp"

#include <mpv/client.h>
#include <mpv/render.h>

#include <print>
#include <utility>

namespace mpvx {

namespace {

void on_mpv_render_update(void* ctx) {
    auto* self = static_cast<VideoRenderer*>(ctx);
    if (self) {
        self->notify_update();
    }
}

} // namespace

VideoRenderer::VideoRenderer() = default;

VideoRenderer::~VideoRenderer() {
    shutdown();
}

Result<void> VideoRenderer::initialize(mpv_handle* mpv, int width, int height) {
    if (!mpv) {
        return std::unexpected(PlayerError{-1, "Null mpv handle passed to renderer"});
    }

    shutdown();

    mpv_ = mpv;
    const auto [w, h] = clamp_render_target(width, height);
    target_.store(pack(w, h), std::memory_order_relaxed);
    has_frame_.store(false, std::memory_order_relaxed);
    force_redraw_.store(false, std::memory_order_relaxed);
    output_enabled_.store(true, std::memory_order_relaxed);

    char sw_api[] = MPV_RENDER_API_TYPE_SW;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, sw_api},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    int err = mpv_render_context_create(&render_ctx_, mpv_, params);
    if (err < 0) {
        std::println(stderr, "[mpvx] Warning: Failed to create mpv software render context: {}", mpv_error_string(err));
        return std::unexpected(PlayerError{err, mpv_error_string(err)});
    }

    running_.store(true);
    update_pending_.store(false);
    render_thread_ = std::thread(&VideoRenderer::render_loop, this);

    mpv_render_context_set_update_callback(render_ctx_, on_mpv_render_update, this);
    std::println("[mpvx] mpv software render context created ({}x{})", w, h);
    return {};
}

void VideoRenderer::shutdown() {
    if (render_ctx_) {
        mpv_render_context_set_update_callback(render_ctx_, nullptr, nullptr);
    }

    if (running_.exchange(false)) {
        cv_.notify_all();
        if (render_thread_.joinable()) {
            render_thread_.join();
        }
    }

    if (render_ctx_) {
        mpv_render_context_free(render_ctx_);
        render_ctx_ = nullptr;
    }
    mpv_ = nullptr;
    ring_ = {};
    ring_index_ = 0;
    last_handoff_ = {};
}

void VideoRenderer::set_target_size(int width, int height) {
    const auto [w, h] = clamp_render_target(width, height);
    const uint64_t next = pack(w, h);
    if (target_.exchange(next, std::memory_order_acq_rel) != next) {
        request_redraw();
    }
}

void VideoRenderer::request_redraw() noexcept {
    force_redraw_.store(true, std::memory_order_release);
    notify_update();
}

void VideoRenderer::suspend_output() noexcept {
    output_enabled_.store(false, std::memory_order_release);
    has_frame_.store(false, std::memory_order_release);
}

void VideoRenderer::resume_output(ResumeFrom from) noexcept {
    if (from == ResumeFrom::NextFrame) {
        has_frame_.store(false, std::memory_order_release); // before enabling delivery
    }
    output_enabled_.store(true, std::memory_order_release);
    request_redraw(); // no-op without a frame since the last suspend/NextFrame resume
}

void VideoRenderer::set_frame_callback(FrameCallback callback) {
    std::lock_guard lock(cb_mutex_);
    frame_callback_ = std::move(callback);
}

void VideoRenderer::set_frame_cap(bool enabled) noexcept {
    constexpr int64_t kCapIntervalNs = 29'000'000; // 4 ms of slack under 33.3 ms
    min_handoff_interval_ns_.store(enabled ? kCapIntervalNs : 0, std::memory_order_release);
}

VideoRenderer::Stats VideoRenderer::stats() const noexcept {
    return Stats{
        .frames_rendered = frames_rendered_.load(std::memory_order_relaxed),
        .buffer_allocations = buffer_allocations_.load(std::memory_order_relaxed),
        .frames_delivered = frames_delivered_.load(std::memory_order_relaxed),
    };
}

void VideoRenderer::notify_update() noexcept {
    {
        // Taking the mutex orders this store against the render thread's predicate check,
        // so a wakeup can never be lost between the check and the wait.
        std::lock_guard lock(cv_mutex_);
        update_pending_.store(true, std::memory_order_release);
    }
    cv_.notify_one();
}

void VideoRenderer::render_loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock(cv_mutex_);
            cv_.wait(lock, [this]() {
                return !running_.load(std::memory_order_acquire) ||
                       update_pending_.load(std::memory_order_acquire);
            });
        }

        if (!running_.load(std::memory_order_acquire)) {
            break;
        }

        if (update_pending_.exchange(false, std::memory_order_acq_rel)) {
            render_frame();
        }
    }
}

VideoRenderer::FrameBuffer& VideoRenderer::acquire_slot(int w, int h) {
    ring_index_ = (ring_index_ + 1) % ring_.size();
    FrameBuffer& slot = ring_[ring_index_];
    if (slot.width() != static_cast<uint32_t>(w) || slot.height() != static_cast<uint32_t>(h)) {
        slot = FrameBuffer(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        buffer_allocations_.fetch_add(1, std::memory_order_relaxed);
    }
    return slot;
}

void VideoRenderer::render_frame() {
    if (!render_ctx_) return;

    // mpv requires an update() call after every update callback, even for forced redraws.
    const uint64_t flags = mpv_render_context_update(render_ctx_);
    const bool new_frame = (flags & MPV_RENDER_UPDATE_FRAME) != 0;
    const bool deliver = output_enabled_.load(std::memory_order_acquire);
    const bool forced = force_redraw_.exchange(false, std::memory_order_acq_rel) && deliver &&
                        has_frame_.load(std::memory_order_acquire);
    // Suspended frames are still rendered: vo_libmpv blocks until each queued frame has
    // gone through mpv_render_context_render() ("not being called or stuck"), so skipping
    // the render would stall playback of the next file. They are just not delivered.
    const bool discard = new_frame && !deliver;
    if (!new_frame && !forced) {
        return;
    }

    const uint64_t target = target_.load(std::memory_order_acquire);
    const int w = unpack_w(target);
    const int h = unpack_h(target);

    FrameBuffer& buffer = acquire_slot(w, h);

    // Mutable begin() detaches (copies) if the GUI still references this slot, so a
    // frame on screen is never overwritten. Compare against the shared pointer to count it.
    const auto* shared_px = std::as_const(buffer).begin();
    auto* px = buffer.begin();
    if (px != shared_px) {
        buffer_allocations_.fetch_add(1, std::memory_order_relaxed);
    }

    int size[2] = {w, h};
    char format[] = "rgb0";
    size_t stride = static_cast<size_t>(w) * 4;
    int block_for_target_time = 0;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, format},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, static_cast<void*>(px)},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if (mpv_render_context_render(render_ctx_, params) < 0) {
        return;
    }

    if (new_frame) {
        // Remember that a frame exists so resume_output() can re-render it.
        has_frame_.store(true, std::memory_order_release);
    }
    if (discard) {
        return;
    }

    // "rgb0" leaves the 4th byte uninitialized; Slint expects opaque alpha.
    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < total; ++i) {
        px[i].a = 255;
    }

    frames_rendered_.fetch_add(1, std::memory_order_relaxed);

    // Forced redraws (resize, resume) always go through so the UI never shows a stale size.
    const auto now = std::chrono::steady_clock::now();
    const int64_t min_interval = min_handoff_interval_ns_.load(std::memory_order_acquire);
    if (min_interval > 0 && !forced &&
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_handoff_).count() < min_interval) {
        return;
    }
    last_handoff_ = now;

    FrameCallback cb;
    {
        std::lock_guard lock(cb_mutex_);
        cb = frame_callback_;
    }

    if (cb) {
        frames_delivered_.fetch_add(1, std::memory_order_relaxed);
        cb(buffer);
    }
}

} // namespace mpvx
