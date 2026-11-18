#include "mpvx/library.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TempDirGuard {
    std::filesystem::path path;

    explicit TempDirGuard(std::string_view name) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / std::format("mpvx_test_{}_{}", name, unique);
        std::filesystem::create_directories(path);
    }

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_dummy_file(const std::filesystem::path& p, std::string_view content = "dummy") {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

TEST_CASE("XDG user-dirs.dirs zero-regex parser", "[library][xdg]") {
    TempDirGuard tmp("xdg");
    const auto home = tmp.path / "home";
    const auto custom_movies = home / "MyMovies";
    const auto fallback_videos = home / "Videos";
    std::filesystem::create_directories(custom_movies);
    std::filesystem::create_directories(fallback_videos);

    // 1. Direct env override takes priority if it exists
    REQUIRE(mpvx::resolve_xdg_videos_dir(custom_movies.string(), "", home.string()) == custom_movies);

    // 2. user-dirs.dirs parsing with comments, whitespace, and $HOME expansion
    const std::string user_dirs =
        "# Commented out XDG_VIDEOS_DIR=\"$HOME/Wrong\"\n"
        "   XDG_DOCUMENTS_DIR=\"$HOME/Documents\"\n"
        "   XDG_VIDEOS_DIR=\"$HOME/MyMovies\"\n";
    REQUIRE(mpvx::resolve_xdg_videos_dir("", user_dirs, home.string()) == custom_movies);

    // 3. ${HOME} expansion
    const std::string user_dirs_braced = "XDG_VIDEOS_DIR=\"${HOME}/MyMovies\"\n";
    REQUIRE(mpvx::resolve_xdg_videos_dir("", user_dirs_braced, home.string()) == custom_movies);

    // 4. Non-existent configured dir falls back to $HOME/Videos
    const std::string missing_dirs = "XDG_VIDEOS_DIR=\"$HOME/DoesNotExist\"\n";
    REQUIRE(mpvx::resolve_xdg_videos_dir("", missing_dirs, home.string()) == fallback_videos);
}

TEST_CASE("Branchless video extension classifier & arena title normalizer", "[library][parser]") {
    REQUIRE(mpvx::is_supported_video_extension("movie.mp4"));
    REQUIRE(mpvx::is_supported_video_extension("episode.MKV"));
    REQUIRE(mpvx::is_supported_video_extension("clip.WebM"));
    REQUIRE(mpvx::is_supported_video_extension("take.m4v"));
    REQUIRE(mpvx::is_supported_video_extension("stream.ts"));
    REQUIRE_FALSE(mpvx::is_supported_video_extension("subs.srt"));
    REQUIRE_FALSE(mpvx::is_supported_video_extension("poster.jpg"));
    REQUIRE_FALSE(mpvx::is_supported_video_extension(".mp4"));
    REQUIRE_FALSE(mpvx::is_supported_video_extension("no_extension"));

    mpvx::StringArena arena;
    const auto s1 = mpvx::normalize_video_title_to("01_Introduction_to_C++26.mp4", arena);
    REQUIRE(arena.view(s1) == "01 Introduction to C++26");

    const auto s2 = mpvx::normalize_video_title_to("Blade.Runner.2049.2160p.mkv", arena);
    REQUIRE(arena.view(s2) == "Blade Runner 2049 2160p");

    const auto s3 = mpvx::normalize_video_title_to("Version.2.5_Release.webm", arena);
    REQUIRE(arena.view(s3) == "Version 2.5 Release");
}

TEST_CASE("64-bit media cache key determinism & invalidation", "[library][cache]") {
    const uint64_t k1 = mpvx::compute_media_cache_key("/videos/test.mkv", 1700000000123456789LL, 1048576ULL);
    const uint64_t k2 = mpvx::compute_media_cache_key("/videos/test.mkv", 1700000000123456789LL, 1048576ULL);
    const uint64_t k_mtime = mpvx::compute_media_cache_key("/videos/test.mkv", 1700000000123456790LL, 1048576ULL);
    const uint64_t k_size = mpvx::compute_media_cache_key("/videos/test.mkv", 1700000000123456789LL, 1048577ULL);
    const uint64_t k_path = mpvx::compute_media_cache_key("/videos/other.mkv", 1700000000123456789LL, 1048576ULL);

    REQUIRE(k1 == k2);
    REQUIRE(k1 != k_mtime);
    REQUIRE(k1 != k_size);
    REQUIRE(k1 != k_path);
}

TEST_CASE("Two-stage scanner: Depth-2 sidebar tree & shelf discovery", "[library][scanner]") {
    TempDirGuard tmp("scanner");
    const auto root = tmp.path / "Videos";
    const auto state_dir = tmp.path / "state";
    const auto cache_dir = tmp.path / "cache";

    write_dummy_file(root / "root_video.mp4");
    write_dummy_file(root / "notes.txt");
    write_dummy_file(root / ".hidden.mkv");
    write_dummy_file(root / "Movies" / "Sci-Fi" / "Dune.mkv");
    write_dummy_file(root / "Movies" / "Sci-Fi" / "Deep" / "Nested" / "HiddenDeep.mp4");
    write_dummy_file(root / "Anime" / "Ep01.mp4");

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::shared_ptr<const mpvx::LibraryViewSnapshot>> snapshots;

    mpvx::LibraryService svc;
    mpvx::LibraryServiceCallbacks cbs;
    cbs.on_snapshot = [&](std::shared_ptr<const mpvx::LibraryViewSnapshot> snap) {
        std::lock_guard lock(mtx);
        snapshots.push_back(std::move(snap));
        cv.notify_all();
    };
    cbs.on_schedule_ui_flush = [&]() {
        svc.flush_pending_updates_on_gui_thread();
    };

    svc.initialize(std::move(cbs), root, state_dir, cache_dir, /*enable_thumb_workers=*/false);

    {
        std::unique_lock lock(mtx);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&]() {
            return !snapshots.empty() && snapshots.back()->shelves_complete;
        }));
    }

    const auto& final_snap = snapshots.back();
    REQUIRE(final_snap->direct_videos.size() == 1);
    REQUIRE(final_snap->arena.view(final_snap->direct_videos[0].title) == "root video");

    // Sidebar has depth-1 folders ("Anime", "Movies") without deep depth-3/4 nodes
    REQUIRE(final_snap->sidebar_folders.size() == 2);
    REQUIRE(final_snap->arena.view(final_snap->sidebar_folders[0].name) == "Anime");
    REQUIRE(final_snap->arena.view(final_snap->sidebar_folders[1].name) == "Movies");
    REQUIRE(final_snap->sidebar_folders[1].has_children == true);

    // Shelves discover Anime & Movies (and recursively find Sci-Fi/Dune.mkv)
    REQUIRE(final_snap->shelves.size() == 2);
    REQUIRE(final_snap->shelves[0].total_count == 1);
    REQUIRE(final_snap->shelves[1].total_count == 2);

    svc.shutdown();
}

TEST_CASE("Watch-history zero-allocation round-trip & 95% watched rule", "[library][history]") {
    std::unordered_map<std::string, mpvx::WatchHistoryEntry> original;
    original["/home/user/Videos/Anime/Episode 01 [1080p] 日本語.mkv"] = mpvx::WatchHistoryEntry{
        600.5, 1440.0, 1700001234LL, false
    };
    original["/home/user/Videos/Completed.mp4"] = mpvx::WatchHistoryEntry{
        960.0, 1000.0, 1700005678LL, false // >= 95% -> becomes watched on parse
    };
    original["/home/user/Videos/AccidentalClick.mp4"] = mpvx::WatchHistoryEntry{
        2.0, 1000.0, 1700009999LL, false // < 5% -> not in progress
    };

    const std::string serialized = mpvx::serialize_watch_history(original);
    const auto parsed = mpvx::parse_watch_history(serialized);

    REQUIRE(parsed.size() == 3);
    const auto& ep1 = parsed.at("/home/user/Videos/Anime/Episode 01 [1080p] 日本語.mkv");
    REQUIRE(ep1.is_in_progress());
    REQUIRE_FALSE(ep1.watched);
    REQUIRE(ep1.last_watched_at == 1700001234LL);

    const auto& done = parsed.at("/home/user/Videos/Completed.mp4");
    REQUIRE(done.watched == true);
    REQUIRE_FALSE(done.is_in_progress());
    REQUIRE(done.progress() == 1.0f);

    const auto& acc = parsed.at("/home/user/Videos/AccidentalClick.mp4");
    REQUIRE_FALSE(acc.watched);
    REQUIRE_FALSE(acc.is_in_progress());
}

TEST_CASE("L2 binary media index & .mpxt thumbnail cache atomic read/write", "[library][cache][io]") {
    TempDirGuard tmp("cache_io");

    // 1. MediaIndexRecord round-trip & CRC rejection
    const auto idx_path = tmp.path / "media-index.bin";
    std::vector<mpvx::MediaIndexRecord> recs{
        mpvx::MediaIndexRecord{0x123456789ABCDEF0ULL, 123.45, 1920, 1080, 1, 0},
        mpvx::MediaIndexRecord{0x0FEDCBA987654321ULL, 456.78, 1280, 720, 1, 0},
    };
    REQUIRE(mpvx::write_media_index_file_atomic(idx_path, recs));

    const auto loaded_recs = mpvx::read_media_index_file(idx_path);
    REQUIRE(loaded_recs.size() == 2);
    REQUIRE(loaded_recs[0].cache_key == recs[0].cache_key);
    REQUIRE(loaded_recs[0].duration_sec == recs[0].duration_sec);
    REQUIRE(loaded_recs[1].width == 1280);

    // Corrupt a byte in payload and verify CRC check rejects it
    {
        std::fstream fs(idx_path, std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(static_cast<std::streamoff>(sizeof(mpvx::MediaIndexHeader) + 4));
        char bad = static_cast<char>(0xFF);
        fs.write(&bad, 1);
    }
    REQUIRE(mpvx::read_media_index_file(idx_path).empty());

    // 2. .mpxt thumbnail pixel round-trip
    const auto mpxt_path = tmp.path / "thumbnails" / "0011223344556677.mpxt";
    slint::SharedPixelBuffer<slint::Rgba8Pixel> pix(mpvx::kThumbnailWidth, mpvx::kThumbnailHeight);
    auto* data = pix.begin();
    for (uint32_t i = 0; i < mpvx::kThumbnailWidth * mpvx::kThumbnailHeight; ++i) {
        data[i] = slint::Rgba8Pixel{
            static_cast<uint8_t>(i & 0xFF),
            static_cast<uint8_t>((i >> 3) & 0xFF),
            static_cast<uint8_t>((i >> 5) & 0xFF),
            255
        };
    }
    REQUIRE(mpvx::write_mpxt_thumbnail_atomic(mpxt_path, pix));

    const auto loaded_pix = mpvx::read_mpxt_thumbnail(mpxt_path);
    REQUIRE(loaded_pix.has_value());
    REQUIRE(loaded_pix->width() == mpvx::kThumbnailWidth);
    REQUIRE(loaded_pix->height() == mpvx::kThumbnailHeight);
    REQUIRE(std::memcmp(pix.begin(), loaded_pix->begin(),
        mpvx::kThumbnailWidth * mpvx::kThumbnailHeight * sizeof(slint::Rgba8Pixel)) == 0);
}

TEST_CASE("Two-tier LIFO viewport priority preemption & zero dropped items", "[library][scheduler]") {
    mpvx::ThumbnailScheduler sched(64);
    const uint64_t epoch = 1;

    for (uint64_t i = 1; i <= 100; ++i) {
        REQUIRE(sched.push_background(mpvx::ThumbnailJob{epoch, i, std::to_string(i), "/bg"}));
    }
    for (uint64_t i = 1001; i <= 1080; ++i) {
        REQUIRE(sched.push_viewport(mpvx::ThumbnailJob{epoch, i, std::to_string(i), "/vp"}));
    }
    REQUIRE(sched.pending_count() == 180);

    // First 64 popped items must be the newest 64 viewport items in LIFO order (1080 down to 1017)
    for (uint64_t expected = 1080; expected >= 1017; --expected) {
        auto job = sched.pop_next(epoch);
        REQUIRE(job.has_value());
        REQUIRE(job->cache_key == expected);
    }

    // Remaining 116 items (100 background + 16 demoted viewport items) must all drain without loss
    size_t remaining = 0;
    while (auto job = sched.pop_next(epoch)) {
        ++remaining;
    }
    REQUIRE(remaining == 116);
    REQUIRE(sched.empty());
}

TEST_CASE("Concurrency stress test: rapid nav_epoch cancellation & zero deadlock", "[library][concurrency]") {
    TempDirGuard tmp("stress");
    const auto root = tmp.path / "Videos";
    for (int i = 0; i < 20; ++i) {
        write_dummy_file(root / std::format("Folder{}", i) / std::format("vid{}.mp4", i));
    }

    mpvx::LibraryService svc;
    std::atomic<int> snapshots_received{0};
    mpvx::LibraryServiceCallbacks cbs;
    cbs.on_snapshot = [&](std::shared_ptr<const mpvx::LibraryViewSnapshot>) {
        snapshots_received.fetch_add(1, std::memory_order_relaxed);
    };
    cbs.on_schedule_ui_flush = [&]() {
        svc.flush_pending_updates_on_gui_thread();
    };

    svc.initialize(std::move(cbs), root, tmp.path / "state", tmp.path / "cache", /*enable_thumb_workers=*/false);

    for (int i = 0; i < 200; ++i) {
        if (i % 3 == 0) {
            svc.set_search_query(std::format("vid{}", i % 10));
        } else if (i % 3 == 1) {
            svc.navigate(mpvx::LibraryNavKind::Folder, (root / std::format("Folder{}", i % 20)).string());
        } else {
            svc.navigate(mpvx::LibraryNavKind::Home);
        }
        svc.record_playback_progress((root / "Folder0" / "vid0.mp4").string(), static_cast<double>(i), 1000.0, false);
    }

    const auto t0 = std::chrono::steady_clock::now();
    svc.shutdown();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    REQUIRE(elapsed_ms < 500);
}
