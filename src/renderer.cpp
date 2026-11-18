#include "mpvx/renderer.hpp"

#include <mpv/client.h>
#include <mpv/render.h>

#include <algorithm>
#include <print>

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
    width_.store(std::max(width, 320));
    height_.store(std::max(height, 240));

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
    std::println("[mpvx] mpv software render context created ({}x{})", width_.load(), height_.load());
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
}

void VideoRenderer::resize(int width, int height) {
    if (width > 0 && height > 0) {
        width_.store(width);
        height_.store(height);
    }
}

void VideoRenderer::set_frame_callback(FrameCallback callback) {
    std::lock_guard lock(cb_mutex_);
    frame_callback_ = std::move(callback);
}

void VideoRenderer::notify_update() noexcept {
    update_pending_.store(true, std::memory_order_release);
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

void VideoRenderer::render_frame() {
    if (!render_ctx_) return;

    uint64_t flags = mpv_render_context_update(render_ctx_);
    if ((flags & MPV_RENDER_UPDATE_FRAME) == 0) {
        return;
    }

    const int w = width_.load(std::memory_order_relaxed);
    const int h = height_.load(std::memory_order_relaxed);
    FrameBuffer buffer(static_cast<uint32_t>(w), static_cast<uint32_t>(h));

    int size[2] = {w, h};
    char format[] = "rgb0";
    size_t stride = static_cast<size_t>(w) * 4;
    void* pixels = static_cast<void*>(buffer.begin());
    int block_for_target_time = 0;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, format},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, pixels},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    int err = mpv_render_context_render(render_ctx_, params);
    if (err < 0) {
        return;
    }

    // Ensure alpha channel is 0xFF since "rgb0" leaves the 4th byte uninitialized
    auto* px = buffer.begin();
    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < total; ++i) {
        px[i].a = 255;
    }

    FrameCallback cb;
    {
        std::lock_guard lock(cb_mutex_);
        cb = frame_callback_;
    }

    if (cb) {
        cb(buffer);
    }
}

} // namespace mpvx
