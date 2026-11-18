#pragma once

#include "mpvx/core.hpp"

#include <slint.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// Forward declaration from libmpv
struct mpv_handle;
struct mpv_render_context;

namespace mpvx {

class VideoRenderer {
public:
    using FrameBuffer = slint::SharedPixelBuffer<slint::Rgba8Pixel>;
    using FrameCallback = std::function<void(const FrameBuffer&)>;

    VideoRenderer();
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    [[nodiscard]] Result<void> initialize(mpv_handle* mpv, int width = 1280, int height = 720);
    void shutdown();

    void resize(int width, int height);
    void set_frame_callback(FrameCallback callback);

    [[nodiscard]] int width() const noexcept { return width_.load(); }
    [[nodiscard]] int height() const noexcept { return height_.load(); }
    [[nodiscard]] bool is_initialized() const noexcept { return render_ctx_ != nullptr; }

    void notify_update() noexcept;
    void render_frame();

private:
    void render_loop();

    mpv_handle* mpv_{nullptr};
    mpv_render_context* render_ctx_{nullptr};

    std::atomic<int> width_{1280};
    std::atomic<int> height_{720};

    std::atomic<bool> running_{false};
    std::atomic<bool> update_pending_{false};
    std::mutex cv_mutex_;
    std::condition_variable cv_;
    std::thread render_thread_;

    std::mutex cb_mutex_;
    FrameCallback frame_callback_;
};

} // namespace mpvx
