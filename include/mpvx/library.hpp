#pragma once

#include "mpvx/core.hpp"

#include <slint.h>
#include <sys/types.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mpvx {

inline constexpr uint32_t kThumbnailWidth = 320;
inline constexpr uint32_t kThumbnailHeight = 180;
inline constexpr size_t kL1PixelCacheCapacity = 128;
inline constexpr size_t kViewportLifoCapacity = 64;
inline constexpr size_t kShelfPreviewCap = 20;
inline constexpr size_t kShelfTraversalBudget = 500;

enum class LibraryNavKind : uint8_t {
    Home,
    ContinueWatching,
    Recent,
    Folder,
};

enum class LibrarySortMode : uint8_t {
    Name,
    Recent,
    Progress,
};

struct StringSlice {
    uint32_t offset{0};
    uint32_t length{0};
};

struct StringArena {
    std::vector<char> buffer;

    [[nodiscard]] StringSlice intern(std::string_view sv) {
        const auto off = static_cast<uint32_t>(buffer.size());
        buffer.insert(buffer.end(), sv.begin(), sv.end());
        return StringSlice{off, static_cast<uint32_t>(sv.size())};
    }

    [[nodiscard]] std::string_view view(StringSlice s) const noexcept {
        if (s.length == 0 || static_cast<size_t>(s.offset) + s.length > buffer.size()) {
            return {};
        }
        return std::string_view(buffer.data() + s.offset, s.length);
    }
};

struct WatchHistoryEntry {
    double position_sec{0.0};
    double duration_sec{0.0};
    int64_t last_watched_at{0};
    bool watched{false};

    [[nodiscard]] constexpr float progress() const noexcept {
        if (watched) return 1.0f;
        if (duration_sec <= 0.0 || position_sec <= 0.0) return 0.0f;
        const double ratio = position_sec / duration_sec;
        return static_cast<float>(ratio > 1.0 ? 1.0 : ratio);
    }

    [[nodiscard]] constexpr bool is_in_progress() const noexcept {
        const float p = progress();
        return !watched && p >= 0.05f && p < 0.95f;
    }
};

struct ScannedVideoItem {
    uint64_t cache_key{0};
    StringSlice id_hex;
    StringSlice path;
    StringSlice title;
    StringSlice subtitle;
    StringSlice duration_text;
    double duration_sec{0.0};
    double position_sec{0.0};
    float progress{0.0f};
    bool watched{false};
    int64_t mtime_ns{0};
    uint64_t file_size{0};
    int64_t last_watched_at{0};
};

struct ScannedFolderNode {
    StringSlice path;
    StringSlice name;
    int depth{1};
    bool has_children{false};
    bool expanded{false};
    bool selected{false};
};

struct ScannedShelf {
    StringSlice folder_path;
    StringSlice title;
    int total_count{0};
    std::vector<ScannedVideoItem> items;
};

struct LibraryViewSnapshot {
    uint64_t epoch{0};
    bool shelves_complete{false};
    LibraryNavKind nav_kind{LibraryNavKind::Home};
    LibrarySortMode sort_mode{LibrarySortMode::Name};
    StringArena arena;
    StringSlice root_path;
    StringSlice current_folder_path;
    std::vector<StringSlice> breadcrumbs;
    std::vector<StringSlice> breadcrumb_paths;
    std::vector<ScannedFolderNode> sidebar_folders;
    std::vector<ScannedVideoItem> continue_watching;
    std::vector<ScannedVideoItem> direct_videos;
    std::vector<ScannedShelf> shelves;
};

struct ThumbnailUpdate {
    uint64_t epoch{0};
    uint64_t cache_key{0};
    std::string id_hex;
    std::string path;
    double duration_sec{0.0};
    std::string duration_text;
    bool has_pixels{false};
    slint::SharedPixelBuffer<slint::Rgba8Pixel> pixels;
};

struct alignas(8) MediaIndexHeader {
    uint32_t magic{0x5856504D}; // "MPVX"
    uint32_t version{1};
    uint64_t entry_count{0};
    uint64_t payload_crc64{0};
};
static_assert(sizeof(MediaIndexHeader) == 24);

struct alignas(8) MediaIndexRecord {
    uint64_t cache_key{0};
    double duration_sec{0.0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t flags{0}; // Bit 0: has_thumbnail_on_disk
    uint32_t reserved{0};
};
static_assert(sizeof(MediaIndexRecord) == 32);

struct alignas(4) MpxtHeader {
    uint32_t magic{0x5458504D}; // "MPXT"
    uint32_t width{kThumbnailWidth};
    uint32_t height{kThumbnailHeight};
    uint32_t format{1}; // 1 = RGBA8
};
static_assert(sizeof(MpxtHeader) == 16);

// ============================================================================
// Pure, Zero-Allocation & Deterministic Helpers
// ============================================================================

[[nodiscard]] constexpr uint64_t compute_media_cache_key(
    std::string_view canonical_path,
    int64_t mtime_ns,
    uint64_t file_size) noexcept
{
    uint64_t h = 14695981039346656037ULL; // FNV-1a 64-bit offset basis
    for (unsigned char c : canonical_path) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    h ^= static_cast<uint64_t>(mtime_ns) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= file_size + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

[[nodiscard]] std::string format_cache_key_hex(uint64_t key);
[[nodiscard]] std::optional<uint64_t> parse_cache_key_hex(std::string_view hex) noexcept;

[[nodiscard]] constexpr uint64_t pack_ext_lower(std::string_view ext) noexcept {
    if (ext.empty() || ext.size() > 8) {
        return 0;
    }
    uint64_t packed = 0;
    for (size_t i = 0; i < ext.size(); ++i) {
        auto c = static_cast<unsigned char>(ext[i]);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c | 0x20);
        }
        packed |= (static_cast<uint64_t>(c) << (i * 8));
    }
    return packed;
}

[[nodiscard]] constexpr bool is_supported_video_extension(std::string_view filename) noexcept {
    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return false;
    }
    switch (pack_ext_lower(filename.substr(dot))) {
        case pack_ext_lower(".mp4"):
        case pack_ext_lower(".mkv"):
        case pack_ext_lower(".webm"):
        case pack_ext_lower(".mov"):
        case pack_ext_lower(".avi"):
        case pack_ext_lower(".m4v"):
        case pack_ext_lower(".ts"):
        case pack_ext_lower(".flv"):
        case pack_ext_lower(".wmv"):
            return true;
        default:
            return false;
    }
}

[[nodiscard]] std::filesystem::path resolve_xdg_videos_dir(
    std::string_view env_xdg_videos = {},
    std::string_view user_dirs_content = {},
    std::string_view home_dir = {});
[[nodiscard]] std::filesystem::path resolve_xdg_state_dir(std::string_view home_dir = {});
[[nodiscard]] std::filesystem::path resolve_xdg_cache_dir(std::string_view home_dir = {});

[[nodiscard]] StringSlice normalize_video_title_to(std::string_view filename, StringArena& arena);
[[nodiscard]] std::string normalize_video_title(std::string_view filename);

[[nodiscard]] StringSlice format_video_subtitle_to(
    const WatchHistoryEntry& watch,
    std::string_view ext_upper,
    uint64_t file_size_bytes,
    StringArena& arena);
[[nodiscard]] std::string format_video_subtitle(
    const WatchHistoryEntry& watch,
    std::string_view ext_upper,
    uint64_t file_size_bytes);

[[nodiscard]] std::unordered_map<std::string, WatchHistoryEntry>
parse_watch_history(std::string_view content);
[[nodiscard]] std::string
serialize_watch_history(const std::unordered_map<std::string, WatchHistoryEntry>& entries);

[[nodiscard]] std::vector<MediaIndexRecord>
read_media_index_file(const std::filesystem::path& path);
[[nodiscard]] bool write_media_index_file_atomic(
    const std::filesystem::path& path,
    const std::vector<MediaIndexRecord>& records);

[[nodiscard]] std::optional<slint::SharedPixelBuffer<slint::Rgba8Pixel>>
read_mpxt_thumbnail(const std::filesystem::path& path);
[[nodiscard]] bool write_mpxt_thumbnail_atomic(
    const std::filesystem::path& path,
    const slint::SharedPixelBuffer<slint::Rgba8Pixel>& buffer);

// ============================================================================
// Two-Tier LIFO Viewport + FIFO Background Thumbnail Scheduler
// ============================================================================

struct ThumbnailJob {
    uint64_t epoch{0};
    uint64_t cache_key{0};
    std::string id_hex;
    std::string path;
};

class ThumbnailScheduler {
public:
    explicit ThumbnailScheduler(size_t lifo_capacity = kViewportLifoCapacity);

    void reset_for_epoch();
    bool push_viewport(ThumbnailJob job);
    bool push_background(ThumbnailJob job);
    [[nodiscard]] std::optional<ThumbnailJob> pop_next(uint64_t current_epoch);
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] size_t pending_count() const noexcept;

private:
    size_t lifo_capacity_;
    std::deque<ThumbnailJob> viewport_lifo_;
    std::deque<ThumbnailJob> background_fifo_;
    std::unordered_set<uint64_t> active_or_done_keys_;
};

// ============================================================================
// LibraryService (Background Scanner, Cache & Thumbnail Pool)
// ============================================================================

struct LibraryServiceCallbacks {
    std::function<void(std::shared_ptr<const LibraryViewSnapshot>)> on_snapshot;
    std::function<void(std::vector<ThumbnailUpdate>)> on_thumbnail_updates;
    std::function<void()> on_schedule_ui_flush;
};

class LibraryService {
public:
    LibraryService();
    ~LibraryService();

    LibraryService(const LibraryService&) = delete;
    LibraryService& operator=(const LibraryService&) = delete;

    void initialize(
        LibraryServiceCallbacks callbacks,
        std::optional<std::filesystem::path> root_override = std::nullopt,
        std::optional<std::filesystem::path> state_dir_override = std::nullopt,
        std::optional<std::filesystem::path> cache_dir_override = std::nullopt,
        bool enable_thumb_workers = true);
    void shutdown();

    // GUI-thread-safe non-blocking commands (T0 -> T3/T4)
    void navigate(LibraryNavKind kind, std::string folder_path = {});
    void set_search_query(std::string query);
    void set_sort_mode(LibrarySortMode mode);
    void toggle_folder_expanded(std::string folder_path);
    void navigate_up();
    void request_refresh();

    void request_thumbnail(std::string_view cache_key_hex, std::string_view path);

    void record_playback_progress(
        std::string_view path,
        double position_sec,
        double duration_sec,
        bool force_flush = false);
    void mark_watched(std::string_view path, bool watched);
    [[nodiscard]] std::optional<WatchHistoryEntry> get_watch_entry(std::string_view path) const;

    void open_external_folder_dialog(std::function<void(std::string)> on_selected);
    void open_media_file_dialog(
        std::string title,
        std::function<void(std::optional<std::string>)> on_finished);
    void reveal_in_file_manager(std::string_view path);

    // Called on T0 inside slint::invoke_from_event_loop to drain coalesced outbox
    void flush_pending_updates_on_gui_thread();

    [[nodiscard]] uint64_t current_epoch() const noexcept {
        return nav_epoch_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::filesystem::path videos_root() const;

private:
    enum class ScannerCmdKind : uint8_t {
        Rescan,
        FlushHistory,
        FlushIndex,
    };

    void scanner_loop(std::stop_token stop);
    void thumb_worker_loop(std::stop_token stop);
    void run_scan(uint64_t epoch);
    void reap_spawned_children();
    [[nodiscard]] bool take_ui_flush_slot_locked();
    void notify_ui_flush(bool should_schedule);

    std::filesystem::path videos_root_;
    std::filesystem::path state_dir_;
    std::filesystem::path cache_dir_;
    std::filesystem::path thumb_dir_;
    std::filesystem::path history_file_;
    std::filesystem::path index_file_;

    LibraryServiceCallbacks callbacks_;
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> nav_epoch_{1};

    // Navigation & filter state (protected by cmd_mutex_, micro-lock only)
    mutable std::mutex cmd_mutex_;
    std::condition_variable_any cmd_cv_;
    LibraryNavKind nav_kind_{LibraryNavKind::Home};
    LibrarySortMode sort_mode_{LibrarySortMode::Name};
    std::string current_folder_;
    std::string search_query_;
    std::unordered_set<std::string> expanded_folders_;
    bool pending_rescan_{true};
    bool pending_history_flush_{false};
    bool pending_index_flush_{false};

    // Watch history & media index (owned by T3; short lock only for snapshot reads)
    mutable std::mutex state_mutex_;
    std::unordered_map<std::string, WatchHistoryEntry> watch_history_;
    std::unordered_map<uint64_t, MediaIndexRecord> media_index_;
    bool history_dirty_{false};
    bool index_dirty_{false};
    int64_t last_history_flush_ms_{0};

    // Thumbnail queue & L1 pixel LRU cache (protected by queue_mutex_, never held across I/O)
    mutable std::mutex queue_mutex_;
    std::condition_variable_any queue_cv_;
    ThumbnailScheduler scheduler_;
    std::list<uint64_t> lru_order_;
    std::unordered_map<uint64_t, std::pair<slint::SharedPixelBuffer<slint::Rgba8Pixel>, std::list<uint64_t>::iterator>> l1_pixels_;

    // Coalesced outbox to GUI thread (protected by outbox_mutex_, <50ns swap only)
    std::mutex outbox_mutex_;
    std::shared_ptr<const LibraryViewSnapshot> pending_snapshot_;
    std::vector<ThumbnailUpdate> pending_ui_updates_;
    bool ui_flush_scheduled_{false};

    // Dialog & external process worker guard
    std::atomic<bool> dialog_open_{false};
    std::mutex dialog_mutex_;
    std::jthread dialog_thread_;
    std::vector<pid_t> spawned_pids_;

    // Background worker threads
    std::jthread scanner_thread_;
    std::vector<std::jthread> thumb_workers_;
};

} // namespace mpvx
