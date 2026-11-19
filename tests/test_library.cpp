#include "mpvx/library.hpp"

#include <catch2/catch_test_macros.hpp>
#include <mpv/client.h>

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

TEST_CASE("Embedded thumbnail track-list inspector prioritizes albumart/image over primary video", "[library][thumbnail]") {
    auto make_str_node = [](const char* s) {
        mpv_node n{};
        n.format = MPV_FORMAT_STRING;
        n.u.string = const_cast<char*>(s);
        return n;
    };
    auto make_int_node = [](int64_t v) {
        mpv_node n{};
        n.format = MPV_FORMAT_INT64;
        n.u.int64 = v;
        return n;
    };
    auto make_flag_node = [](bool b) {
        mpv_node n{};
        n.format = MPV_FORMAT_FLAG;
        n.u.flag = b ? 1 : 0;
        return n;
    };

    // Track 1: Primary 1920x1080 video stream (selected by default, not albumart)
    const char* t1_keys[] = {"id", "type", "selected", "albumart", "image", "demux-w", "demux-h"};
    mpv_node t1_vals[] = {
        make_int_node(1),
        make_str_node("video"),
        make_flag_node(true),
        make_flag_node(false),
        make_flag_node(false),
        make_int_node(1920),
        make_int_node(1080),
    };
    mpv_node_list t1_map{7, t1_vals, const_cast<char**>(t1_keys)};
    mpv_node t1_node{};
    t1_node.format = MPV_FORMAT_NODE_MAP;
    t1_node.u.list = &t1_map;

    // Track 2: Audio stream (ignored)
    const char* t2_keys[] = {"id", "type", "selected"};
    mpv_node t2_vals[] = {
        make_int_node(1),
        make_str_node("audio"),
        make_flag_node(true),
    };
    mpv_node_list t2_map{3, t2_vals, const_cast<char**>(t2_keys)};
    mpv_node t2_node{};
    t2_node.format = MPV_FORMAT_NODE_MAP;
    t2_node.u.list = &t2_map;

    // Track 3: Embedded cover art / attached picture (vid=2, albumart=true, 600x900 poster)
    const char* t3_keys[] = {"id", "type", "selected", "albumart", "image", "demux-w", "demux-h"};
    mpv_node t3_vals[] = {
        make_int_node(2),
        make_str_node("video"),
        make_flag_node(false),
        make_flag_node(true),
        make_flag_node(true),
        make_int_node(600),
        make_int_node(900),
    };
    mpv_node_list t3_map{7, t3_vals, const_cast<char**>(t3_keys)};
    mpv_node t3_node{};
    t3_node.format = MPV_FORMAT_NODE_MAP;
    t3_node.u.list = &t3_map;

    // Case A: Container with both primary video and embedded thumbnail
    mpv_node tracks_with_thumb[] = {t1_node, t2_node, t3_node};
    mpv_node_list list_with_thumb{3, tracks_with_thumb, nullptr};
    mpv_node root_with_thumb{};
    root_with_thumb.format = MPV_FORMAT_NODE_ARRAY;
    root_with_thumb.u.list = &list_with_thumb;

    const auto probe_a = mpvx::inspect_track_list_for_thumbnail(root_with_thumb);
    REQUIRE(probe_a.embedded_thumb_vid == 2);
    REQUIRE(probe_a.primary_video_vid == 1);
    REQUIRE(probe_a.selected_vid == 1);
    // Primary video dimensions are preserved rather than overwritten by the cover art dimensions
    REQUIRE(probe_a.primary_width == 1920);
    REQUIRE(probe_a.primary_height == 1080);

    // Case B: Container without embedded thumbnail (fallback to 10% frame on primary video)
    mpv_node tracks_no_thumb[] = {t1_node, t2_node};
    mpv_node_list list_no_thumb{2, tracks_no_thumb, nullptr};
    mpv_node root_no_thumb{};
    root_no_thumb.format = MPV_FORMAT_NODE_ARRAY;
    root_no_thumb.u.list = &list_no_thumb;

    const auto probe_b = mpvx::inspect_track_list_for_thumbnail(root_no_thumb);
    REQUIRE(probe_b.embedded_thumb_vid == -1);
    REQUIRE(probe_b.primary_video_vid == 1);
    REQUIRE(probe_b.selected_vid == 1);
    REQUIRE(probe_b.primary_width == 1920);
    REQUIRE(probe_b.primary_height == 1080);
}

TEST_CASE("End-to-end MKV embedded cover-art thumbnail extraction vs 10% fallback", "[library][thumbnail][integration]") {
    TempDirGuard tmp("mkv_cover");
    const auto root = tmp.path / "Videos";
    const auto state_dir = tmp.path / "state";
    const auto cache_dir = tmp.path / "cache";
    std::filesystem::create_directories(root);

    const auto cover_png = tmp.path / "cover.png";
    const auto cover_webp = tmp.path / "cover.webp";
    const auto cover_jpg = tmp.path / "cover.jpg";
    const auto mkv_with_cover = root / "with_cover.mkv";
    const auto mkv_with_webp = root / "with_webp_cover.mkv";
    const auto mkv_generic_mime = root / "black_with_octet_webp.mkv";
    const auto mp4_with_cover = root / "with_cover.mp4";
    const auto mkv_no_cover = root / "no_cover.mkv";

    // Generate a solid pure-green cover image (R=0, G=255, B=0), a pure-red 2s video (R=255, G=0, B=0),
    // a black video with generic-MIME WebP cover, an MP4 with attached_pic, and an MKV without cover art
    // whose first 0.2s is RED and 0.2s..4s is BLUE (so t=0 would be RED while 10% = 0.4s is BLUE).
    const std::string cmd_cover = std::format(
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0x00FF00:s=320x180 -frames:v 1 \"{}\" && "
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0x00FF00:s=320x180 -frames:v 1 \"{}\" && "
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0x00FF00:s=320x180 -frames:v 1 \"{}\" && "
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0xFF0000:s=320x180:d=2 -f lavfi -i anullsrc=r=48000:cl=stereo -t 2 "
        "-c:v libx264 -pix_fmt yuv420p -c:a aac -disposition:v:0 default "
        "-attach \"{}\" -metadata:s:t mimetype=image/png -metadata:s:t filename=cover.png \"{}\" && "
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0xFF0000:s=320x180:d=2 -f lavfi -i anullsrc=r=48000:cl=stereo -t 2 "
        "-c:v libx264 -pix_fmt yuv420p -c:a aac -disposition:v:0 default "
        "-attach \"{}\" -metadata:s:t mimetype=image/webp -metadata:s:t filename=cover.webp \"{}\" && "
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0x000000:s=320x180:d=2 "
        "-c:v libx264 -pix_fmt yuv420p "
        "-attach \"{}\" -metadata:s:t mimetype=application/octet-stream -metadata:s:t filename=cover.webp \"{}\" && "
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0xFF0000:s=320x180:d=2 -i \"{}\" -map 0 -map 1 "
        "-c:v:0 libx264 -pix_fmt yuv420p -c:v:1 mjpeg -disposition:v:1 attached_pic \"{}\" && "
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0xFF0000:s=320x180:d=0.2 -f lavfi -i color=c=0x0000FF:s=320x180:d=3.8 "
        "-filter_complex \"[0:v][1:v]concat=n=2:v=1:a=0\" -c:v libx264 -g 5 -pix_fmt yuv420p \"{}\"",
        cover_png.string(), cover_webp.string(), cover_jpg.string(),
        cover_png.string(), mkv_with_cover.string(),
        cover_webp.string(), mkv_with_webp.string(),
        cover_webp.string(), mkv_generic_mime.string(),
        cover_jpg.string(), mp4_with_cover.string(),
        mkv_no_cover.string());

    if (std::system(cmd_cover.c_str()) != 0 || !std::filesystem::exists(mkv_with_cover)) {
        SKIP("ffmpeg not available for synthetic MKV attachment generation");
    }

    std::mutex mtx;
    std::condition_variable cv;
    std::unordered_map<std::string, mpvx::ThumbnailUpdate> updates;

    mpvx::LibraryService svc;
    mpvx::LibraryServiceCallbacks cbs;
    std::vector<std::string> snap_files;
    cbs.on_snapshot = [&](std::shared_ptr<const mpvx::LibraryViewSnapshot> snap) {
        std::lock_guard lock(mtx);
        snap_files.clear();
        for (const auto& item : snap->direct_videos) {
            snap_files.push_back(std::string(snap->arena.view(item.path)));
        }
    };
    cbs.on_thumbnail_updates = [&](std::vector<mpvx::ThumbnailUpdate> batch) {
        std::lock_guard lock(mtx);
        for (auto& u : batch) {
            updates[u.path] = std::move(u);
        }
        cv.notify_all();
    };
    cbs.on_schedule_ui_flush = [&]() {
        svc.flush_pending_updates_on_gui_thread();
    };

    svc.initialize(std::move(cbs), root, state_dir, cache_dir, /*enable_thumb_workers=*/true);

    {
        std::unique_lock lock(mtx);
        const bool all_arrived = cv.wait_for(lock, std::chrono::seconds(15), [&]() {
            return updates.contains(mkv_with_cover.string()) &&
                   updates.contains(mkv_with_webp.string()) &&
                   updates.contains(mkv_generic_mime.string()) &&
                   updates.contains(mp4_with_cover.string()) &&
                   updates.contains(mkv_no_cover.string());
        });
        if (!all_arrived) {
            std::string received;
            for (const auto& [k, v] : updates) {
                received += k + (v.has_pixels ? "(ok) " : "(nopix) ");
            }
            std::string scanned;
            for (const auto& s : snap_files) {
                scanned += s + " ";
            }
            FAIL("Timed out waiting for thumbnails."
                 << " scanned=[" << scanned << "] received=[" << received << "]");
        }
    }

    svc.shutdown();

    // Verify black MKV with application/octet-stream MIME cover.webp rendered GREEN cover (not black frame)
    const auto& generic_upd = updates.at(mkv_generic_mime.string());
    REQUIRE(generic_upd.has_pixels);
    const auto center_generic = generic_upd.pixels.begin()[(mpvx::kThumbnailHeight / 2) * mpvx::kThumbnailWidth + (mpvx::kThumbnailWidth / 2)];
    REQUIRE(center_generic.g > 180);
    REQUIRE(center_generic.r < 80);
    REQUIRE(center_generic.a == 255);

    // Verify MKV with embedded PNG cover rendered the GREEN cover attachment (not the RED video stream)
    const auto& cov_upd = updates.at(mkv_with_cover.string());
    REQUIRE(cov_upd.has_pixels);
    const auto center_cov = cov_upd.pixels.begin()[(mpvx::kThumbnailHeight / 2) * mpvx::kThumbnailWidth + (mpvx::kThumbnailWidth / 2)];
    REQUIRE(center_cov.g > 180);
    REQUIRE(center_cov.r < 80);
    REQUIRE(center_cov.a == 255);

    // Verify MKV with embedded WEBP cover (yt-dlp style) rendered the GREEN cover attachment
    const auto& webp_upd = updates.at(mkv_with_webp.string());
    REQUIRE(webp_upd.has_pixels);
    const auto center_webp = webp_upd.pixels.begin()[(mpvx::kThumbnailHeight / 2) * mpvx::kThumbnailWidth + (mpvx::kThumbnailWidth / 2)];
    REQUIRE(center_webp.g > 180);
    REQUIRE(center_webp.r < 80);
    REQUIRE(center_webp.a == 255);

    // Verify MP4 with embedded attached_pic rendered the GREEN cover attachment
    const auto& mp4_upd = updates.at(mp4_with_cover.string());
    REQUIRE(mp4_upd.has_pixels);
    const auto center_mp4 = mp4_upd.pixels.begin()[(mpvx::kThumbnailHeight / 2) * mpvx::kThumbnailWidth + (mpvx::kThumbnailWidth / 2)];
    REQUIRE(center_mp4.g > 180);
    REQUIRE(center_mp4.r < 80);
    REQUIRE(center_mp4.a == 255);

    // Verify MKV without embedded cover fell back to the 10% BLUE video frame (not t=0 RED)
    const auto& nocov_upd = updates.at(mkv_no_cover.string());
    REQUIRE(nocov_upd.has_pixels);
    const auto center_nocov = nocov_upd.pixels.begin()[(mpvx::kThumbnailHeight / 2) * mpvx::kThumbnailWidth + (mpvx::kThumbnailWidth / 2)];
    REQUIRE(center_nocov.b > 180);
    REQUIRE(center_nocov.r < 80);
    REQUIRE(center_nocov.a == 255);
}

TEST_CASE("Zero-decoder EBML & ISOBMFF container probe extracts cover art, duration, and dimensions", "[library][thumbnail][probe]") {
    TempDirGuard tmp("probe_container");
    const auto cover_webp = tmp.path / "cover.webp";
    const auto mkv_cov = tmp.path / "has_cover.mkv";
    const auto mkv_plain = tmp.path / "plain.mkv";
    const auto mp4_covr = tmp.path / "has_covr.mp4";

    const std::string cmd = std::format(
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0x00FF00:s=640x360 -frames:v 1 \"{}\" && "
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0xFF0000:s=640x360:d=3 -c:v libx264 -pix_fmt yuv420p "
        "-attach \"{}\" -metadata:s:t mimetype=image/webp -metadata:s:t filename=cover.webp \"{}\" && "
        "ffmpeg -nostdin -v error -y -f lavfi -i color=c=0x0000FF:s=640x360:d=3 -c:v libx264 -pix_fmt yuv420p \"{}\"",
        cover_webp.string(), cover_webp.string(), mkv_cov.string(), mkv_plain.string());
    if (std::system(cmd.c_str()) != 0 || !std::filesystem::exists(mkv_cov)) {
        SKIP("ffmpeg not available for synthetic container generation");
    }

    // 1. MKV with embedded cover.webp
    const auto p_cov = mpvx::probe_container_for_thumbnail(mkv_cov);
    REQUIRE(p_cov.parsed);
    REQUIRE(p_cov.has_embedded_cover);
    REQUIRE(p_cov.cover_format == "webp");
    REQUIRE(!p_cov.cover_data.empty());
    REQUIRE(p_cov.duration_sec > 2.5);
    REQUIRE(p_cov.width == 640);
    REQUIRE(p_cov.height == 360);

    const auto decoded = mpvx::decode_and_resize_cover_image(p_cov.cover_data, p_cov.cover_format);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->width() == mpvx::kThumbnailWidth);
    REQUIRE(decoded->height() == mpvx::kThumbnailHeight);
    const auto px = decoded->begin()[(mpvx::kThumbnailHeight / 2) * mpvx::kThumbnailWidth + (mpvx::kThumbnailWidth / 2)];
    REQUIRE(px.g > 180);
    REQUIRE(px.r < 80);

    // 2. MKV without embedded cover: still parses duration & dimensions cleanly
    const auto p_plain = mpvx::probe_container_for_thumbnail(mkv_plain);
    REQUIRE(p_plain.parsed);
    REQUIRE_FALSE(p_plain.has_embedded_cover);
    REQUIRE(p_plain.cover_data.empty());
    REQUIRE(p_plain.duration_sec > 2.5);
    REQUIRE(p_plain.width == 640);
    REQUIRE(p_plain.height == 360);

    // 3. Synthetic ISOBMFF MP4 with `moov` after `mdat` and iTunes `udta/meta/ilst/covr` PNG payload
    auto append_be32 = [](std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto make_box = [&](const char tag[4], const std::vector<uint8_t>& body) {
        std::vector<uint8_t> box;
        append_be32(box, static_cast<uint32_t>(8 + body.size()));
        box.insert(box.end(), tag, tag + 4);
        box.insert(box.end(), body.begin(), body.end());
        return box;
    };

    // Minimal valid 1x1 green PNG
    const std::vector<uint8_t> png_1x1 = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE,
        0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54,
        0x08, 0xD7, 0x63, 0x60, 0xF8, 0x0F, 0x00, 0x01,
        0x04, 0x01, 0x00, 0xA8, 0x6B, 0x77, 0x68,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };

    std::vector<uint8_t> data_body(8, 0);
    data_body[3] = 14; // PNG type indicator
    data_body.insert(data_body.end(), png_1x1.begin(), png_1x1.end());
    auto covr_box = make_box("covr", make_box("data", data_body));
    auto ilst_box = make_box("ilst", covr_box);
    std::vector<uint8_t> meta_body(4, 0); // FullBox version+flags
    meta_body.insert(meta_body.end(), ilst_box.begin(), ilst_box.end());
    auto udta_box = make_box("udta", make_box("meta", meta_body));

    std::vector<uint8_t> mvhd_body(100, 0);
    // timescale = 1000 at +12, duration = 45000 (45.0s) at +16
    mvhd_body[14] = 0x03; mvhd_body[15] = 0xE8;
    mvhd_body[18] = 0xAF; mvhd_body[19] = 0xC8;
    auto mvhd_box = make_box("mvhd", mvhd_body);

    std::vector<uint8_t> moov_body = mvhd_box;
    moov_body.insert(moov_body.end(), udta_box.begin(), udta_box.end());
    auto moov_box = make_box("moov", moov_body);

    auto ftyp_box = make_box("ftyp", {'i', 's', 'o', 'm', 0, 0, 2, 0});
    auto mdat_box = make_box("mdat", std::vector<uint8_t>(256, 0));

    {
        std::ofstream ofs(mp4_covr, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(ftyp_box.data()), static_cast<std::streamsize>(ftyp_box.size()));
        ofs.write(reinterpret_cast<const char*>(mdat_box.data()), static_cast<std::streamsize>(mdat_box.size()));
        ofs.write(reinterpret_cast<const char*>(moov_box.data()), static_cast<std::streamsize>(moov_box.size()));
    }

    const auto p_mp4 = mpvx::probe_container_for_thumbnail(mp4_covr);
    REQUIRE(p_mp4.parsed);
    REQUIRE(p_mp4.has_embedded_cover);
    REQUIRE(p_mp4.cover_format == "png");
    REQUIRE(p_mp4.cover_data == png_1x1);
    REQUIRE(p_mp4.duration_sec == 45.0);

    // 4. Classic QuickTime (.mov) starting with `wide` + `moov` (no `ftyp`) still probes cleanly
    const auto mov_no_ftyp = tmp.path / "classic.mov";
    auto wide_box = make_box("wide", {});
    {
        std::ofstream ofs(mov_no_ftyp, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(wide_box.data()), static_cast<std::streamsize>(wide_box.size()));
        ofs.write(reinterpret_cast<const char*>(moov_box.data()), static_cast<std::streamsize>(moov_box.size()));
    }
    const auto p_mov = mpvx::probe_container_for_thumbnail(mov_no_ftyp);
    REQUIRE(p_mov.parsed);
    REQUIRE(p_mov.has_embedded_cover);
    REQUIRE(p_mov.cover_format == "png");
    REQUIRE(p_mov.duration_sec == 45.0);

    // 5. Minimal MKV without a Tracks element must NOT set parsed=true (so Tier 2 inspects track-list)
    const auto mkv_no_tracks = tmp.path / "no_tracks.mkv";
    {
        // EBML header (1A 45 DF A3, size 0) + Segment (18 53 80 67, unknown size) + Void (EC, size 0)
        const std::vector<uint8_t> raw = {
            0x1A, 0x45, 0xDF, 0xA3, 0x80,
            0x18, 0x53, 0x80, 0x67, 0xFF,
            0xEC, 0x80, 0x00, 0x00, 0x00, 0x00
        };
        std::ofstream ofs(mkv_no_tracks, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    }
    const auto p_notracks = mpvx::probe_container_for_thumbnail(mkv_no_tracks);
    REQUIRE_FALSE(p_notracks.parsed);
    REQUIRE_FALSE(p_notracks.has_embedded_cover);
}

TEST_CASE("resize_cover_rgba8 performs aspect-fill center crop and box downsample", "[library][thumbnail]") {
    // 640x480 image: top/bottom 60px bands are pure RED, middle 640x360 (16:9) band is pure GREEN
    slint::SharedPixelBuffer<slint::Rgba8Pixel> src(640, 480);
    auto* p = src.begin();
    for (uint32_t y = 0; y < 480; ++y) {
        const bool in_center_16_9 = (y >= 60 && y < 420);
        for (uint32_t x = 0; x < 640; ++x) {
            p[y * 640 + x] = in_center_16_9
                ? slint::Rgba8Pixel{0, 240, 0, 255}
                : slint::Rgba8Pixel{240, 0, 0, 255};
        }
    }

    const auto dst = mpvx::resize_cover_rgba8(src, 320, 180);
    REQUIRE(dst.width() == 320);
    REQUIRE(dst.height() == 180);
    // Top row & center must both come strictly from the cropped 16:9 green region
    REQUIRE(dst.begin()[0].g == 240);
    REQUIRE(dst.begin()[0].r == 0);
    REQUIRE(dst.begin()[179 * 320 + 319].g == 240);
    REQUIRE(dst.begin()[179 * 320 + 319].r == 0);
}
