#include "mpvx/library.hpp"

#include <mpv/client.h>
#include <mpv/render.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <sstream>
#include <system_error>

extern char** environ;

namespace mpvx {

namespace {

constexpr uint64_t kCrc64Poly = 0xC96C5795D7870F42ULL;

[[nodiscard]] uint64_t compute_crc64(const void* data, size_t len) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (int b = 0; b < 8; ++b) {
            const uint64_t mask = -(crc & 1ULL);
            crc = (crc >> 1) ^ (kCrc64Poly & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] int64_t now_epoch_seconds() noexcept {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] int64_t now_steady_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] std::string_view trim_ascii_left(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
        sv.remove_prefix(1);
    }
    return sv;
}

[[nodiscard]] std::string_view trim_ascii_right(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
        sv.remove_suffix(1);
    }
    return sv;
}

[[nodiscard]] bool icase_contains(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    auto to_lower = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c | 0x20) : c;
    };
    const size_t limit = haystack.size() - needle.size();
    for (size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (to_lower(static_cast<unsigned char>(haystack[i + j])) !=
                to_lower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

[[nodiscard]] bool icase_less(std::string_view a, std::string_view b) noexcept {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        auto ca = static_cast<unsigned char>(a[i]);
        auto cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca | 0x20);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb | 0x20);
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

[[nodiscard]] std::string uppercase_extension(std::string_view filename) {
    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= filename.size()) {
        return "VIDEO";
    }
    std::string ext(filename.substr(dot + 1));
    for (char& ch : ext) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - ('a' - 'A'));
        }
    }
    return ext;
}

[[nodiscard]] std::string format_file_size(uint64_t bytes) {
    if (bytes >= (1ULL << 30)) {
        return std::format("{:.1f} GB", static_cast<double>(bytes) / static_cast<double>(1ULL << 30));
    }
    if (bytes >= (1ULL << 20)) {
        return std::format("{:.0f} MB", static_cast<double>(bytes) / static_cast<double>(1ULL << 20));
    }
    if (bytes >= (1ULL << 10)) {
        return std::format("{:.0f} KB", static_cast<double>(bytes) / static_cast<double>(1ULL << 10));
    }
    return std::format("{} B", bytes);
}

[[nodiscard]] std::string read_small_file(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::string out;
    ifs.seekg(0, std::ios::end);
    const auto sz = ifs.tellg();
    if (sz > 0 && sz < 1024 * 1024) {
        out.resize(static_cast<size_t>(sz));
        ifs.seekg(0, std::ios::beg);
        ifs.read(out.data(), sz);
    }
    return out;
}

[[nodiscard]] bool write_file_atomic(const std::filesystem::path& target, std::string_view data) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    const auto tmp = std::filesystem::path(target.string() + ".tmp");
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        ofs.flush();
        if (!ofs) return false;
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

[[nodiscard]] std::string escape_tsv_field(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

[[nodiscard]] std::string unescape_tsv_field(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] == '\\' && i + 1 < sv.size()) {
            const char next = sv[i + 1];
            if (next == '\\') {
                out.push_back('\\');
                ++i;
                continue;
            }
            if (next == 't') {
                out.push_back('\t');
                ++i;
                continue;
            }
            if (next == 'n') {
                out.push_back('\n');
                ++i;
                continue;
            }
            if (next == 'r') {
                out.push_back('\r');
                ++i;
                continue;
            }
        }
        out.push_back(sv[i]);
    }
    return out;
}

// Persistent thread-local libmpv software thumbnail extractor
struct ThreadLocalThumbExtractor {
    mpv_handle* mpv{nullptr};
    mpv_render_context* rctx{nullptr};

    ~ThreadLocalThumbExtractor() {
        shutdown();
    }

    bool ensure_init() {
        if (mpv && rctx) return true;
        shutdown();

        mpv = mpv_create();
        if (!mpv) return false;

        mpv_set_option_string(mpv, "vo", "libmpv");
        mpv_set_option_string(mpv, "ao", "null");
        mpv_set_option_string(mpv, "vid", "1");
        mpv_set_option_string(mpv, "aid", "no");
        mpv_set_option_string(mpv, "sid", "no");
        mpv_set_option_string(mpv, "hwdec", "auto-safe");
        mpv_set_option_string(mpv, "hr-seek", "no");
        mpv_set_option_string(mpv, "vd-lavc-fast", "yes");
        mpv_set_option_string(mpv, "vd-lavc-skiploopfilter", "all");
        mpv_set_option_string(mpv, "demuxer-readahead-secs", "0");
        mpv_set_option_string(mpv, "cache", "no");
        mpv_set_option_string(mpv, "pause", "yes");
        mpv_set_option_string(mpv, "quiet", "yes");
        mpv_set_option_string(mpv, "terminal", "no");

        if (mpv_initialize(mpv) < 0) {
            shutdown();
            return false;
        }

        char sw_api[] = MPV_RENDER_API_TYPE_SW;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, sw_api},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_create(&rctx, mpv, params) < 0) {
            shutdown();
            return false;
        }
        return true;
    }

    void shutdown() {
        if (rctx) {
            mpv_render_context_free(rctx);
            rctx = nullptr;
        }
        if (mpv) {
            mpv_terminate_destroy(mpv);
            mpv = nullptr;
        }
    }

    bool extract(
        const std::string& path,
        const std::stop_token& stop,
        const std::atomic<uint64_t>& nav_epoch,
        uint64_t job_epoch,
        double& out_duration_sec,
        uint32_t& out_width,
        uint32_t& out_height,
        slint::SharedPixelBuffer<slint::Rgba8Pixel>& out_pixels)
    {
        if (!ensure_init()) return false;

        // Drain stale events
        while (true) {
            mpv_event* ev = mpv_wait_event(mpv, 0.0);
            if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        }

        const char* cmd[] = {"loadfile", path.c_str(), "replace", nullptr, "start=10%", nullptr};
        // libmpv v2.5 loadfile takes (url, flags, index, options) or 3-arg (url, flags, options)
        // Use 3-arg form with per-file option string "start=10%"
        const char* cmd_load[] = {"loadfile", path.c_str(), "replace", "start=10%", nullptr};
        (void)cmd;
        if (mpv_command(mpv, cmd_load) < 0) {
            return false;
        }

        bool loaded = false;
        bool restarted = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);

        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            if (nav_epoch.load(std::memory_order_relaxed) != job_epoch) {
                const char* stop_cmd[] = {"stop", nullptr};
                mpv_command_async(mpv, 0, stop_cmd);
                return false;
            }
            mpv_event* ev = mpv_wait_event(mpv, 0.02);
            if (!ev) continue;
            if (ev->event_id == MPV_EVENT_FILE_LOADED) {
                loaded = true;
            } else if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) {
                restarted = true;
            } else if (ev->event_id == MPV_EVENT_END_FILE) {
                auto* end_ev = static_cast<mpv_event_end_file*>(ev->data);
                if (end_ev && end_ev->reason == MPV_END_FILE_REASON_ERROR) {
                    return false;
                }
                break;
            }
            if (loaded && restarted) {
                break;
            }
        }

        if (!loaded) {
            const char* stop_cmd[] = {"stop", nullptr};
            mpv_command_async(mpv, 0, stop_cmd);
            return false;
        }

        double dur = 0.0;
        if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur) >= 0 && dur > 0.0) {
            out_duration_sec = dur;
        }
        int64_t vw = 0;
        int64_t vh = 0;
        if (mpv_get_property(mpv, "width", MPV_FORMAT_INT64, &vw) >= 0 && vw > 0) {
            out_width = static_cast<uint32_t>(vw);
        }
        if (mpv_get_property(mpv, "height", MPV_FORMAT_INT64, &vh) >= 0 && vh > 0) {
            out_height = static_cast<uint32_t>(vh);
        }

        slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(kThumbnailWidth, kThumbnailHeight);
        int size[2] = {static_cast<int>(kThumbnailWidth), static_cast<int>(kThumbnailHeight)};
        char format[] = "rgba";
        size_t stride = static_cast<size_t>(kThumbnailWidth) * 4;
        mpv_render_param rparams[] = {
            {MPV_RENDER_PARAM_SW_SIZE, size},
            {MPV_RENDER_PARAM_SW_FORMAT, format},
            {MPV_RENDER_PARAM_SW_STRIDE, &stride},
            {MPV_RENDER_PARAM_SW_POINTER, buffer.begin()},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };

        const int rerr = mpv_render_context_render(rctx, rparams);
        const char* stop_cmd[] = {"stop", nullptr};
        mpv_command_async(mpv, 0, stop_cmd);

        if (rerr < 0) {
            return false;
        }
        out_pixels = std::move(buffer);
        return true;
    }
};

[[nodiscard]] bool extract_thumb_ffmpeg_fallback(
    const std::string& path,
    slint::SharedPixelBuffer<slint::Rgba8Pixel>& out_pixels)
{
    int pipefd[2]{-1, -1};
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    char arg0[] = "ffmpeg";
    char arg1[] = "-nostdin";
    char arg2[] = "-v";
    char arg3[] = "error";
    char arg4[] = "-ss";
    char arg5[] = "5";
    char arg6[] = "-i";
    std::string path_copy = path;
    char arg8[] = "-frames:v";
    char arg9[] = "1";
    char arg10[] = "-vf";
    char arg11[] = "scale=320:180:force_original_aspect_ratio=increase,crop=320:180";
    char arg12[] = "-f";
    char arg13[] = "rawvideo";
    char arg14[] = "-pix_fmt";
    char arg15[] = "rgba";
    char arg16[] = "-";
    char* argv[] = {
        arg0, arg1, arg2, arg3, arg4, arg5, arg6, path_copy.data(),
        arg8, arg9, arg10, arg11, arg12, arg13, arg14, arg15, arg16, nullptr
    };

    pid_t pid = -1;
    const int spawn_res = posix_spawnp(&pid, "ffmpeg", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);

    if (spawn_res != 0) {
        close(pipefd[0]);
        return false;
    }

    slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(kThumbnailWidth, kThumbnailHeight);
    auto* dst = reinterpret_cast<uint8_t*>(buffer.begin());
    const size_t expected = static_cast<size_t>(kThumbnailWidth) * kThumbnailHeight * 4;
    size_t total = 0;
    while (total < expected) {
        const ssize_t n = read(pipefd[0], dst + total, expected - total);
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (total == expected && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        out_pixels = std::move(buffer);
        return true;
    }
    return false;
}

[[nodiscard]] std::optional<std::string> run_native_dialog_blocking(
    const char* title,
    bool directory_mode)
{
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if ((!display || display[0] == '\0') && (!wayland || wayland[0] == '\0')) {
        return std::nullopt;
    }

    std::string cmd = directory_mode
        ? std::format(
              "if command -v zenity >/dev/null 2>&1; then "
              "zenity --file-selection --directory --title=\"{}\" 2>/dev/null; "
              "elif command -v kdialog >/dev/null 2>&1; then "
              "kdialog --getexistingdirectory . --title \"{}\" 2>/dev/null; "
              "else exit 127; fi",
              title, title)
        : std::format(
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
    const int status = pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }

    if (status == 0 && !output.empty()) {
        return output;
    }
    return std::nullopt;
}

} // namespace

// ============================================================================
// Pure Helpers
// ============================================================================

std::string format_cache_key_hex(uint64_t key) {
    return std::format("{:016x}", key);
}

std::optional<uint64_t> parse_cache_key_hex(std::string_view hex) noexcept {
    if (hex.empty() || hex.size() > 16) {
        return std::nullopt;
    }
    uint64_t val = 0;
    const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), val, 16);
    if (ec != std::errc{} || ptr != hex.data() + hex.size()) {
        return std::nullopt;
    }
    return val;
}

std::filesystem::path resolve_xdg_videos_dir(
    std::string_view env_xdg_videos,
    std::string_view user_dirs_content,
    std::string_view home_dir)
{
    if (home_dir.empty()) {
        if (const char* env_home = std::getenv("HOME")) {
            home_dir = env_home;
        }
    }
    if (env_xdg_videos.empty()) {
        if (const char* env_vid = std::getenv("XDG_VIDEOS_DIR")) {
            env_xdg_videos = env_vid;
        }
    }

    if (!env_xdg_videos.empty() && env_xdg_videos.front() == '/') {
        std::filesystem::path p(env_xdg_videos);
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            return p;
        }
    }

    std::string file_buf;
    if (user_dirs_content.empty()) {
        std::filesystem::path cfg_dir;
        if (const char* xdg_cfg = std::getenv("XDG_CONFIG_HOME"); xdg_cfg && xdg_cfg[0] != '\0') {
            cfg_dir = xdg_cfg;
        } else if (!home_dir.empty()) {
            cfg_dir = std::filesystem::path(home_dir) / ".config";
        }
        if (!cfg_dir.empty()) {
            file_buf = read_small_file(cfg_dir / "user-dirs.dirs");
            user_dirs_content = file_buf;
        }
    }

    constexpr std::string_view kKeyPrefix = "XDG_VIDEOS_DIR=\"";
    size_t pos = 0;
    while (pos < user_dirs_content.size()) {
        const size_t nl = user_dirs_content.find('\n', pos);
        const std::string_view line = trim_ascii_right(
            trim_ascii_left(nl == std::string_view::npos
                                ? user_dirs_content.substr(pos)
                                : user_dirs_content.substr(pos, nl - pos)));
        pos = (nl == std::string_view::npos) ? user_dirs_content.size() : (nl + 1);

        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.starts_with(kKeyPrefix) && line.size() > kKeyPrefix.size() && line.back() == '"') {
            std::string_view raw = line.substr(kKeyPrefix.size(), line.size() - kKeyPrefix.size() - 1);
            std::string expanded;
            if (raw.starts_with("$HOME/")) {
                expanded = std::string(home_dir) + std::string(raw.substr(5));
            } else if (raw == "$HOME") {
                expanded = std::string(home_dir);
            } else if (raw.starts_with("${HOME}/")) {
                expanded = std::string(home_dir) + std::string(raw.substr(7));
            } else if (raw == "${HOME}") {
                expanded = std::string(home_dir);
            } else {
                expanded = std::string(raw);
            }
            if (!expanded.empty()) {
                std::filesystem::path candidate(expanded);
                std::error_code ec;
                if (std::filesystem::is_directory(candidate, ec)) {
                    return candidate;
                }
            }
        }
    }

    if (!home_dir.empty()) {
        std::filesystem::path fallback = std::filesystem::path(home_dir) / "Videos";
        std::error_code ec;
        if (std::filesystem::is_directory(fallback, ec)) {
            return fallback;
        }
        return std::filesystem::path(home_dir);
    }
    return std::filesystem::current_path();
}

std::filesystem::path resolve_xdg_state_dir(std::string_view home_dir) {
    if (const char* xdg_state = std::getenv("XDG_STATE_HOME"); xdg_state && xdg_state[0] != '\0') {
        return std::filesystem::path(xdg_state) / "mpvx";
    }
    if (home_dir.empty()) {
        if (const char* env_home = std::getenv("HOME")) {
            home_dir = env_home;
        }
    }
    if (!home_dir.empty()) {
        return std::filesystem::path(home_dir) / ".local" / "state" / "mpvx";
    }
    return std::filesystem::temp_directory_path() / "mpvx-state";
}

std::filesystem::path resolve_xdg_cache_dir(std::string_view home_dir) {
    if (const char* xdg_cache = std::getenv("XDG_CACHE_HOME"); xdg_cache && xdg_cache[0] != '\0') {
        return std::filesystem::path(xdg_cache) / "mpvx";
    }
    if (home_dir.empty()) {
        if (const char* env_home = std::getenv("HOME")) {
            home_dir = env_home;
        }
    }
    if (!home_dir.empty()) {
        return std::filesystem::path(home_dir) / ".cache" / "mpvx";
    }
    return std::filesystem::temp_directory_path() / "mpvx-cache";
}

StringSlice normalize_video_title_to(std::string_view filename, StringArena& arena) {
    const auto dot = filename.rfind('.');
    if (dot != std::string_view::npos && dot > 0) {
        filename = filename.substr(0, dot);
    }

    const uint32_t start_off = static_cast<uint32_t>(arena.buffer.size());
    bool prev_space = true; // Trim leading spaces
    auto is_digit = [](char c) -> bool { return c >= '0' && c <= '9'; };

    for (size_t i = 0; i < filename.size(); ++i) {
        char c = filename[i];
        bool replace_with_space = false;
        if (c == '_') {
            replace_with_space = true;
        } else if (c == '.') {
            const bool left_digit = (i > 0) && is_digit(filename[i - 1]);
            const bool right_digit = (i + 1 < filename.size()) && is_digit(filename[i + 1]);
            const bool right_is_alpha = (i + 2 < filename.size()) &&
                ((filename[i + 2] >= 'a' && filename[i + 2] <= 'z') ||
                 (filename[i + 2] >= 'A' && filename[i + 2] <= 'Z'));
            // Preserve short version decimals like "2.5" while splitting "2049.2160p"
            const bool is_four_digit_year = (i >= 4) && is_digit(filename[i - 2]) &&
                is_digit(filename[i - 3]) && is_digit(filename[i - 4]);
            if (!left_digit || !right_digit || is_four_digit_year || right_is_alpha) {
                replace_with_space = true;
            }
        } else if (c == ' ' || c == '\t') {
            replace_with_space = true;
        }

        if (replace_with_space) {
            if (!prev_space) {
                arena.buffer.push_back(' ');
                prev_space = true;
            }
        } else {
            arena.buffer.push_back(c);
            prev_space = false;
        }
    }

    while (arena.buffer.size() > start_off && arena.buffer.back() == ' ') {
        arena.buffer.pop_back();
    }

    const uint32_t len = static_cast<uint32_t>(arena.buffer.size()) - start_off;
    if (len == 0 && !filename.empty()) {
        return arena.intern(filename);
    }
    return StringSlice{start_off, len};
}

std::string normalize_video_title(std::string_view filename) {
    StringArena arena;
    const StringSlice s = normalize_video_title_to(filename, arena);
    return std::string(arena.view(s));
}

StringSlice format_video_subtitle_to(
    const WatchHistoryEntry& watch,
    std::string_view ext_upper,
    uint64_t file_size_bytes,
    StringArena& arena)
{
    return arena.intern(format_video_subtitle(watch, ext_upper, file_size_bytes));
}

std::string format_video_subtitle(
    const WatchHistoryEntry& watch,
    std::string_view ext_upper,
    uint64_t file_size_bytes)
{
    if (watch.is_in_progress() && watch.duration_sec > watch.position_sec) {
        return std::format("{} left", format_timestamp(watch.duration_sec - watch.position_sec));
    }
    if (watch.watched) {
        return std::format("Watched · {} · {}", ext_upper, format_file_size(file_size_bytes));
    }
    return std::format("{} · {}", ext_upper, format_file_size(file_size_bytes));
}

std::unordered_map<std::string, WatchHistoryEntry> parse_watch_history(std::string_view content) {
    std::unordered_map<std::string, WatchHistoryEntry> result;
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t nl = content.find('\n', pos);
        const std::string_view line = (nl == std::string_view::npos)
            ? content.substr(pos)
            : content.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? content.size() : (nl + 1);

        if (line.empty() || line.front() == '#') {
            continue;
        }

        const size_t t1 = line.find('\t');
        if (t1 == std::string_view::npos) continue;
        const size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string_view::npos) continue;
        const size_t t3 = line.find('\t', t2 + 1);
        if (t3 == std::string_view::npos) continue;
        const size_t t4 = line.find('\t', t3 + 1);
        if (t4 == std::string_view::npos) continue;

        const std::string_view path_sv = line.substr(0, t1);
        const std::string_view pos_sv = line.substr(t1 + 1, t2 - t1 - 1);
        const std::string_view dur_sv = line.substr(t2 + 1, t3 - t2 - 1);
        const std::string_view ts_sv = line.substr(t3 + 1, t4 - t3 - 1);
        const std::string_view watched_sv = trim_ascii_right(line.substr(t4 + 1));

        WatchHistoryEntry entry;
        auto [p1, ec1] = std::from_chars(pos_sv.data(), pos_sv.data() + pos_sv.size(), entry.position_sec);
        auto [p2, ec2] = std::from_chars(dur_sv.data(), dur_sv.data() + dur_sv.size(), entry.duration_sec);
        auto [p3, ec3] = std::from_chars(ts_sv.data(), ts_sv.data() + ts_sv.size(), entry.last_watched_at);
        int w_flag = 0;
        auto [p4, ec4] = std::from_chars(watched_sv.data(), watched_sv.data() + watched_sv.size(), w_flag);

        if (ec1 != std::errc{} || ec2 != std::errc{} || ec3 != std::errc{} || ec4 != std::errc{}) {
            continue;
        }
        entry.watched = (w_flag != 0) || (entry.duration_sec > 0.0 && entry.position_sec / entry.duration_sec >= 0.95);
        std::string key = unescape_tsv_field(path_sv);
        if (!key.empty()) {
            result.emplace(std::move(key), entry);
        }
    }
    return result;
}

std::string serialize_watch_history(const std::unordered_map<std::string, WatchHistoryEntry>& entries) {
    std::string out;
    out.reserve(entries.size() * 96);
    for (const auto& [path, entry] : entries) {
        out += escape_tsv_field(path);
        out.push_back('\t');
        char num_buf[64];
        auto [p1, e1] = std::to_chars(num_buf, num_buf + sizeof(num_buf), entry.position_sec);
        out.append(num_buf, p1);
        out.push_back('\t');
        auto [p2, e2] = std::to_chars(num_buf, num_buf + sizeof(num_buf), entry.duration_sec);
        out.append(num_buf, p2);
        out.push_back('\t');
        auto [p3, e3] = std::to_chars(num_buf, num_buf + sizeof(num_buf), entry.last_watched_at);
        out.append(num_buf, p3);
        out.push_back('\t');
        out.push_back(entry.watched ? '1' : '0');
        out.push_back('\n');
    }
    return out;
}

std::vector<MediaIndexRecord> read_media_index_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};

    MediaIndexHeader hdr{};
    ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!ifs || hdr.magic != 0x5856504DU || hdr.version != 1 || hdr.entry_count > 1'000'000ULL) {
        return {};
    }
    if (hdr.entry_count == 0) {
        return {};
    }

    std::vector<MediaIndexRecord> records(hdr.entry_count);
    const std::streamsize bytes = static_cast<std::streamsize>(hdr.entry_count * sizeof(MediaIndexRecord));
    ifs.read(reinterpret_cast<char*>(records.data()), bytes);
    if (!ifs || ifs.gcount() != bytes) {
        return {};
    }

    const uint64_t actual_crc = compute_crc64(records.data(), static_cast<size_t>(bytes));
    if (actual_crc != hdr.payload_crc64) {
        return {};
    }
    return records;
}

bool write_media_index_file_atomic(
    const std::filesystem::path& path,
    const std::vector<MediaIndexRecord>& records)
{
    MediaIndexHeader hdr{};
    hdr.entry_count = records.size();
    const size_t payload_bytes = records.size() * sizeof(MediaIndexRecord);
    hdr.payload_crc64 = payload_bytes > 0 ? compute_crc64(records.data(), payload_bytes) : 0;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        if (payload_bytes > 0) {
            ofs.write(reinterpret_cast<const char*>(records.data()), static_cast<std::streamsize>(payload_bytes));
        }
        ofs.flush();
        if (!ofs) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<slint::SharedPixelBuffer<slint::Rgba8Pixel>> read_mpxt_thumbnail(
    const std::filesystem::path& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::nullopt;

    MpxtHeader hdr{};
    ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!ifs || hdr.magic != 0x5458504DU || hdr.width != kThumbnailWidth ||
        hdr.height != kThumbnailHeight || hdr.format != 1) {
        return std::nullopt;
    }

    slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(hdr.width, hdr.height);
    const std::streamsize expected = static_cast<std::streamsize>(hdr.width) * hdr.height * 4;
    ifs.read(reinterpret_cast<char*>(buffer.begin()), expected);
    if (!ifs || ifs.gcount() != expected) {
        return std::nullopt;
    }
    return buffer;
}

bool write_mpxt_thumbnail_atomic(
    const std::filesystem::path& path,
    const slint::SharedPixelBuffer<slint::Rgba8Pixel>& buffer)
{
    if (buffer.width() != kThumbnailWidth || buffer.height() != kThumbnailHeight) {
        return false;
    }
    MpxtHeader hdr{};
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tid_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto tmp = std::filesystem::path(std::format("{}.tmp.{}", path.string(), tid_hash));
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        const std::streamsize bytes = static_cast<std::streamsize>(kThumbnailWidth) * kThumbnailHeight * 4;
        ofs.write(reinterpret_cast<const char*>(buffer.begin()), bytes);
        ofs.flush();
        if (!ofs) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// ============================================================================
// ThumbnailScheduler
// ============================================================================

ThumbnailScheduler::ThumbnailScheduler(size_t lifo_capacity)
    : lifo_capacity_(std::max<size_t>(1, lifo_capacity)) {}

void ThumbnailScheduler::reset_for_epoch() {
    viewport_lifo_.clear();
    background_fifo_.clear();
    active_or_done_keys_.clear();
}

bool ThumbnailScheduler::push_viewport(ThumbnailJob job) {
    if (!active_or_done_keys_.insert(job.cache_key).second) {
        return false;
    }
    if (viewport_lifo_.size() >= lifo_capacity_) {
        // Evict oldest viewport entry to background FIFO (never drop!)
        background_fifo_.push_back(std::move(viewport_lifo_.front()));
        viewport_lifo_.pop_front();
    }
    viewport_lifo_.push_back(std::move(job));
    return true;
}

bool ThumbnailScheduler::push_background(ThumbnailJob job) {
    if (!active_or_done_keys_.insert(job.cache_key).second) {
        return false;
    }
    background_fifo_.push_back(std::move(job));
    return true;
}

std::optional<ThumbnailJob> ThumbnailScheduler::pop_next(uint64_t current_epoch) {
    while (!viewport_lifo_.empty()) {
        auto job = std::move(viewport_lifo_.back());
        viewport_lifo_.pop_back();
        if (job.epoch == current_epoch) {
            return job;
        }
    }
    while (!background_fifo_.empty()) {
        auto job = std::move(background_fifo_.front());
        background_fifo_.pop_front();
        if (job.epoch == current_epoch) {
            return job;
        }
    }
    return std::nullopt;
}

bool ThumbnailScheduler::empty() const noexcept {
    return viewport_lifo_.empty() && background_fifo_.empty();
}

size_t ThumbnailScheduler::pending_count() const noexcept {
    return viewport_lifo_.size() + background_fifo_.size();
}

// ============================================================================
// LibraryService
// ============================================================================

LibraryService::LibraryService() = default;

LibraryService::~LibraryService() {
    shutdown();
}

void LibraryService::initialize(
    LibraryServiceCallbacks callbacks,
    std::optional<std::filesystem::path> root_override,
    std::optional<std::filesystem::path> state_dir_override,
    std::optional<std::filesystem::path> cache_dir_override,
    bool enable_thumb_workers)
{
    if (initialized_.exchange(true)) {
        return;
    }
    callbacks_ = std::move(callbacks);
    videos_root_ = root_override.has_value() ? *root_override : resolve_xdg_videos_dir();
    state_dir_ = state_dir_override.has_value() ? *state_dir_override : resolve_xdg_state_dir();
    cache_dir_ = cache_dir_override.has_value() ? *cache_dir_override : resolve_xdg_cache_dir();
    thumb_dir_ = cache_dir_ / "thumbnails";
    history_file_ = state_dir_ / "watch-history.tsv";
    index_file_ = cache_dir_ / "media-index.bin";

    {
        std::lock_guard lock(cmd_mutex_);
        current_folder_ = videos_root_.string();
        nav_kind_ = LibraryNavKind::Home;
        pending_rescan_ = true;
    }

    scanner_thread_ = std::jthread([this](std::stop_token st) { scanner_loop(st); });
    if (enable_thumb_workers) {
        thumb_workers_.reserve(2);
        for (int i = 0; i < 2; ++i) {
            thumb_workers_.emplace_back([this](std::stop_token st) { thumb_worker_loop(st); });
        }
    }
}

void LibraryService::shutdown() {
    if (!initialized_.exchange(false)) {
        return;
    }
    nav_epoch_.fetch_add(1, std::memory_order_acq_rel);

    for (auto& w : thumb_workers_) {
        w.request_stop();
    }
    queue_cv_.notify_all();
    thumb_workers_.clear();

    scanner_thread_.request_stop();
    cmd_cv_.notify_all();
    if (scanner_thread_.joinable()) {
        scanner_thread_.join();
    }

    {
        std::lock_guard lock(dialog_mutex_);
        if (dialog_thread_.joinable()) {
            dialog_thread_.join();
        }
    }
    reap_spawned_children();

    // Final synchronous persistence flush on teardown
    std::string hist_data;
    bool write_hist = false;
    std::vector<MediaIndexRecord> idx_records;
    bool write_idx = false;
    {
        std::lock_guard lock(state_mutex_);
        if (history_dirty_) {
            hist_data = serialize_watch_history(watch_history_);
            history_dirty_ = false;
            write_hist = true;
        }
        if (index_dirty_) {
            idx_records.reserve(media_index_.size());
            for (const auto& [k, rec] : media_index_) {
                idx_records.push_back(rec);
            }
            index_dirty_ = false;
            write_idx = true;
        }
    }
    if (write_hist) {
        (void)write_file_atomic(history_file_, hist_data);
    }
    if (write_idx) {
        (void)write_media_index_file_atomic(index_file_, idx_records);
    }
}

std::filesystem::path LibraryService::videos_root() const {
    return videos_root_;
}

void LibraryService::navigate(LibraryNavKind kind, std::string folder_path) {
    nav_epoch_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(cmd_mutex_);
        nav_kind_ = kind;
        if (kind == LibraryNavKind::Home) {
            current_folder_ = videos_root_.string();
        } else if (kind == LibraryNavKind::Folder && !folder_path.empty()) {
            current_folder_ = std::move(folder_path);
        }
        pending_rescan_ = true;
    }
    cmd_cv_.notify_one();
}

void LibraryService::set_search_query(std::string query) {
    nav_epoch_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(cmd_mutex_);
        search_query_ = std::move(query);
        pending_rescan_ = true;
    }
    cmd_cv_.notify_one();
}

void LibraryService::set_sort_mode(LibrarySortMode mode) {
    nav_epoch_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(cmd_mutex_);
        sort_mode_ = mode;
        pending_rescan_ = true;
    }
    cmd_cv_.notify_one();
}

void LibraryService::toggle_folder_expanded(std::string folder_path) {
    nav_epoch_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(cmd_mutex_);
        if (!expanded_folders_.erase(folder_path)) {
            expanded_folders_.insert(std::move(folder_path));
        }
        pending_rescan_ = true;
    }
    cmd_cv_.notify_one();
}

void LibraryService::navigate_up() {
    std::string next_folder;
    LibraryNavKind next_kind = LibraryNavKind::Folder;
    {
        std::lock_guard lock(cmd_mutex_);
        if (nav_kind_ != LibraryNavKind::Folder) {
            return;
        }
        std::filesystem::path cur(current_folder_);
        if (cur == videos_root_) {
            next_kind = LibraryNavKind::Home;
            next_folder = videos_root_.string();
        } else {
            auto parent = cur.parent_path();
            if (parent.empty() || parent == videos_root_) {
                next_kind = LibraryNavKind::Home;
                next_folder = videos_root_.string();
            } else {
                next_folder = parent.string();
            }
        }
    }
    navigate(next_kind, std::move(next_folder));
}

void LibraryService::request_refresh() {
    nav_epoch_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(cmd_mutex_);
        pending_rescan_ = true;
    }
    cmd_cv_.notify_one();
}

void LibraryService::request_thumbnail(std::string_view cache_key_hex, std::string_view path) {
    auto key_opt = parse_cache_key_hex(cache_key_hex);
    if (!key_opt.has_value() || path.empty()) {
        return;
    }
    const uint64_t key = *key_opt;
    const uint64_t epoch = nav_epoch_.load(std::memory_order_acquire);

    // Fast path: check L1 pixel cache under queue_mutex_ (no I/O)
    std::optional<slint::SharedPixelBuffer<slint::Rgba8Pixel>> hit_pixels;
    bool enqueued = false;
    {
        std::lock_guard lock(queue_mutex_);
        if (auto it = l1_pixels_.find(key); it != l1_pixels_.end()) {
            lru_order_.splice(lru_order_.begin(), lru_order_, it->second.second);
            hit_pixels = it->second.first;
        } else {
            enqueued = scheduler_.push_viewport(ThumbnailJob{
                epoch,
                key,
                std::string(cache_key_hex),
                std::string(path),
            });
        }
    }

    if (hit_pixels.has_value()) {
        bool should_schedule = false;
        {
            std::lock_guard ob_lock(outbox_mutex_);
            pending_ui_updates_.push_back(ThumbnailUpdate{
                epoch,
                key,
                std::string(cache_key_hex),
                std::string(path),
                0.0,
                {},
                true,
                std::move(*hit_pixels),
            });
            should_schedule = take_ui_flush_slot_locked();
        }
        notify_ui_flush(should_schedule);
        return;
    }

    if (enqueued) {
        queue_cv_.notify_one();
    }
}

void LibraryService::record_playback_progress(
    std::string_view path,
    double position_sec,
    double duration_sec,
    bool force_flush)
{
    if (path.empty()) return;

    const int64_t now_sec = now_epoch_seconds();
    const int64_t now_ms = now_steady_ms();
    bool trigger_flush = force_flush;

    {
        std::lock_guard lock(state_mutex_);
        auto& entry = watch_history_[std::string(path)];
        entry.position_sec = std::max(0.0, position_sec);
        if (duration_sec > 0.0) {
            entry.duration_sec = duration_sec;
        }
        entry.last_watched_at = now_sec;
        if (entry.duration_sec > 0.0 && (entry.position_sec / entry.duration_sec) >= 0.95) {
            entry.watched = true;
        }
        history_dirty_ = true;
        if (!trigger_flush && (now_ms - last_history_flush_ms_) >= 10000) {
            trigger_flush = true;
        }
    }

    if (trigger_flush) {
        {
            std::lock_guard lock(cmd_mutex_);
            pending_history_flush_ = true;
        }
        cmd_cv_.notify_one();
    }
}

void LibraryService::mark_watched(std::string_view path, bool watched) {
    if (path.empty()) return;
    const int64_t now_sec = now_epoch_seconds();
    {
        std::lock_guard lock(state_mutex_);
        auto& entry = watch_history_[std::string(path)];
        entry.watched = watched;
        entry.position_sec = watched ? entry.duration_sec : 0.0;
        entry.last_watched_at = now_sec;
        history_dirty_ = true;
    }
    {
        std::lock_guard lock(cmd_mutex_);
        pending_history_flush_ = true;
        pending_rescan_ = true;
    }
    nav_epoch_.fetch_add(1, std::memory_order_acq_rel);
    cmd_cv_.notify_one();
}

std::optional<WatchHistoryEntry> LibraryService::get_watch_entry(std::string_view path) const {
    std::lock_guard lock(state_mutex_);
    if (auto it = watch_history_.find(std::string(path)); it != watch_history_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void LibraryService::open_external_folder_dialog(std::function<void(std::string)> on_selected) {
    if (dialog_open_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard lock(dialog_mutex_);
    if (dialog_thread_.joinable()) {
        dialog_thread_.join();
    }
    dialog_thread_ = std::jthread([this, cb = std::move(on_selected)](std::stop_token) {
        auto chosen = run_native_dialog_blocking("Open Video Folder", true);
        dialog_open_.store(false, std::memory_order_release);
        if (chosen.has_value() && cb) {
            cb(std::move(*chosen));
        }
    });
}

void LibraryService::open_media_file_dialog(
    std::string title,
    std::function<void(std::optional<std::string>)> on_finished)
{
    if (dialog_open_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard lock(dialog_mutex_);
    if (dialog_thread_.joinable()) {
        dialog_thread_.join();
    }
    dialog_thread_ = std::jthread([this, title = std::move(title), cb = std::move(on_finished)](std::stop_token) {
        auto chosen = run_native_dialog_blocking(title.c_str(), false);
        dialog_open_.store(false, std::memory_order_release);
        if (cb) {
            cb(std::move(chosen));
        }
    });
}

void LibraryService::reap_spawned_children() {
    std::lock_guard lock(dialog_mutex_);
    std::erase_if(spawned_pids_, [](pid_t pid) {
        int status = 0;
        const pid_t res = waitpid(pid, &status, WNOHANG);
        return res == pid || (res < 0 && errno != EINTR);
    });
}

void LibraryService::reveal_in_file_manager(std::string_view path) {
    reap_spawned_children();
    if (path.empty()) return;
    std::filesystem::path p(path);
    std::string parent = p.has_parent_path() ? p.parent_path().string() : p.string();
    if (parent.empty()) return;

    char prog[] = "xdg-open";
    char* argv[] = {prog, parent.data(), nullptr};
    pid_t pid = -1;
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0 && pid > 0) {
        std::lock_guard lock(dialog_mutex_);
        spawned_pids_.push_back(pid);
    }
}

bool LibraryService::take_ui_flush_slot_locked() {
    if (!ui_flush_scheduled_) {
        ui_flush_scheduled_ = true;
        return true;
    }
    return false;
}

void LibraryService::notify_ui_flush(bool should_schedule) {
    if (should_schedule && callbacks_.on_schedule_ui_flush) {
        callbacks_.on_schedule_ui_flush();
    }
}

void LibraryService::flush_pending_updates_on_gui_thread() {
    std::shared_ptr<const LibraryViewSnapshot> snap;
    std::vector<ThumbnailUpdate> updates;
    {
        std::lock_guard lock(outbox_mutex_);
        snap = std::move(pending_snapshot_);
        updates.swap(pending_ui_updates_);
        ui_flush_scheduled_ = false;
    }
    if (snap && callbacks_.on_snapshot) {
        callbacks_.on_snapshot(std::move(snap));
    }
    if (!updates.empty() && callbacks_.on_thumbnail_updates) {
        callbacks_.on_thumbnail_updates(std::move(updates));
    }
}

void LibraryService::scanner_loop(std::stop_token stop) {
    // Initial hydration of L2 watch history and media index off T0
    {
        const std::string hist_raw = read_small_file(history_file_);
        auto loaded_hist = parse_watch_history(hist_raw);
        auto loaded_idx = read_media_index_file(index_file_);

        std::lock_guard lock(state_mutex_);
        watch_history_ = std::move(loaded_hist);
        media_index_.reserve(loaded_idx.size());
        for (const auto& rec : loaded_idx) {
            media_index_[rec.cache_key] = rec;
        }
        last_history_flush_ms_ = now_steady_ms();
    }

    while (!stop.stop_requested()) {
        bool do_scan = false;
        bool do_hist_flush = false;
        bool do_idx_flush = false;
        {
            std::unique_lock lock(cmd_mutex_);
            cmd_cv_.wait(lock, stop, [this]() {
                return pending_rescan_ || pending_history_flush_ || pending_index_flush_;
            });
            if (stop.stop_requested()) break;
            do_scan = std::exchange(pending_rescan_, false);
            do_hist_flush = std::exchange(pending_history_flush_, false);
            do_idx_flush = std::exchange(pending_index_flush_, false);
        }

        if (do_hist_flush) {
            std::string serialized;
            bool need_write = false;
            {
                std::lock_guard lock(state_mutex_);
                if (history_dirty_) {
                    serialized = serialize_watch_history(watch_history_);
                    history_dirty_ = false;
                    last_history_flush_ms_ = now_steady_ms();
                    need_write = true;
                }
            }
            if (need_write) {
                (void)write_file_atomic(history_file_, serialized);
            }
        }

        if (do_idx_flush) {
            std::vector<MediaIndexRecord> records;
            bool need_write = false;
            {
                std::lock_guard lock(state_mutex_);
                if (index_dirty_) {
                    records.reserve(media_index_.size());
                    for (const auto& [k, rec] : media_index_) {
                        records.push_back(rec);
                    }
                    index_dirty_ = false;
                    need_write = true;
                }
            }
            if (need_write) {
                (void)write_media_index_file_atomic(index_file_, records);
            }
        }

        if (do_scan) {
            const uint64_t epoch = nav_epoch_.load(std::memory_order_acquire);
            run_scan(epoch);
        }
    }
}

void LibraryService::run_scan(uint64_t epoch) {
    LibraryNavKind nav_kind{};
    LibrarySortMode sort_mode{};
    std::string cur_folder;
    std::string search;
    std::unordered_set<std::string> expanded;
    {
        std::lock_guard lock(cmd_mutex_);
        nav_kind = nav_kind_;
        sort_mode = sort_mode_;
        cur_folder = current_folder_;
        search = search_query_;
        expanded = expanded_folders_;
    }

    std::unordered_map<std::string, WatchHistoryEntry> hist_copy;
    std::unordered_map<uint64_t, MediaIndexRecord> idx_copy;
    {
        std::lock_guard lock(state_mutex_);
        hist_copy = watch_history_;
        idx_copy = media_index_;
    }

    auto make_video_item = [&](
        const std::filesystem::directory_entry& entry,
        StringArena& arena) -> std::optional<ScannedVideoItem>
    {
        std::error_code ec;
        if (!entry.is_regular_file(ec)) return std::nullopt;
        const std::string fname = entry.path().filename().string();
        if (fname.empty() || fname.front() == '.' || !is_supported_video_extension(fname)) {
            return std::nullopt;
        }

        const std::string full_path = entry.path().string();
        if (!search.empty() && !icase_contains(fname, search) && !icase_contains(full_path, search)) {
            return std::nullopt;
        }

        const auto ftime = entry.last_write_time(ec);
        const int64_t mtime_ns = ec ? 0
            : std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count();
        const uint64_t fsize = entry.file_size(ec);
        const uint64_t ckey = compute_media_cache_key(full_path, mtime_ns, fsize);

        WatchHistoryEntry watch;
        if (auto wit = hist_copy.find(full_path); wit != hist_copy.end()) {
            watch = wit->second;
        }
        double dur_sec = watch.duration_sec;
        if (auto iit = idx_copy.find(ckey); iit != idx_copy.end() && iit->second.duration_sec > 0.0) {
            dur_sec = iit->second.duration_sec;
            if (watch.duration_sec <= 0.0) {
                watch.duration_sec = dur_sec;
            }
        }

        ScannedVideoItem item;
        item.cache_key = ckey;
        item.id_hex = arena.intern(format_cache_key_hex(ckey));
        item.path = arena.intern(full_path);
        item.title = normalize_video_title_to(fname, arena);
        item.subtitle = format_video_subtitle_to(watch, uppercase_extension(fname), fsize, arena);
        item.duration_text = arena.intern(dur_sec > 0.0 ? format_timestamp(dur_sec) : std::string("--:--"));
        item.duration_sec = dur_sec;
        item.position_sec = watch.position_sec;
        item.progress = watch.progress();
        item.watched = watch.watched;
        item.mtime_ns = mtime_ns;
        item.file_size = fsize;
        item.last_watched_at = watch.last_watched_at;
        return item;
    };

    auto sort_items = [&](std::vector<ScannedVideoItem>& items, const StringArena& arena) {
        std::sort(items.begin(), items.end(), [&](const ScannedVideoItem& a, const ScannedVideoItem& b) {
            if (sort_mode == LibrarySortMode::Recent) {
                if (a.mtime_ns != b.mtime_ns) return a.mtime_ns > b.mtime_ns;
            } else if (sort_mode == LibrarySortMode::Progress) {
                if (a.progress != b.progress) return a.progress > b.progress;
            }
            return icase_less(arena.view(a.title), arena.view(b.title));
        });
    };

    // Build Stage-1 snapshot
    auto stage1 = std::make_shared<LibraryViewSnapshot>();
    stage1->epoch = epoch;
    stage1->shelves_complete = false;
    stage1->nav_kind = nav_kind;
    stage1->sort_mode = sort_mode;
    stage1->arena.buffer.reserve(64 * 1024);
    stage1->root_path = stage1->arena.intern(videos_root_.string());
    stage1->current_folder_path = stage1->arena.intern(cur_folder);

    // Breadcrumbs
    {
        std::filesystem::path root_p = videos_root_;
        std::filesystem::path cur_p(cur_folder);
        stage1->breadcrumbs.push_back(stage1->arena.intern(
            root_p.filename().empty() ? root_p.string() : root_p.filename().string()));
        stage1->breadcrumb_paths.push_back(stage1->arena.intern(root_p.string()));

        if (nav_kind == LibraryNavKind::ContinueWatching) {
            stage1->breadcrumbs.push_back(stage1->arena.intern("Continue Watching"));
            stage1->breadcrumb_paths.push_back(stage1->arena.intern(cur_folder));
        } else if (nav_kind == LibraryNavKind::Recent) {
            stage1->breadcrumbs.push_back(stage1->arena.intern("Recent"));
            stage1->breadcrumb_paths.push_back(stage1->arena.intern(cur_folder));
        } else if (cur_p != root_p) {
            std::error_code ec;
            auto rel = std::filesystem::relative(cur_p, root_p, ec);
            if (!ec && !rel.empty() && rel.native()[0] != '.') {
                std::filesystem::path accum = root_p;
                for (const auto& part : rel) {
                    if (part == ".") continue;
                    accum /= part;
                    stage1->breadcrumbs.push_back(stage1->arena.intern(part.string()));
                    stage1->breadcrumb_paths.push_back(stage1->arena.intern(accum.string()));
                }
            }
        }
    }

    // Depth-2 Sidebar Folder Tree
    {
        std::error_code ec;
        std::vector<std::filesystem::path> depth1_dirs;
        for (const auto& d1 : std::filesystem::directory_iterator(
                 videos_root_, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (epoch != nav_epoch_.load(std::memory_order_relaxed)) return;
            std::error_code dec;
            if (d1.is_symlink(dec) || !d1.is_directory(dec)) continue;
            const std::string name = d1.path().filename().string();
            if (name.empty() || name.front() == '.') continue;
            depth1_dirs.push_back(d1.path());
        }
        std::sort(depth1_dirs.begin(), depth1_dirs.end(), [](const auto& a, const auto& b) {
            return icase_less(a.filename().string(), b.filename().string());
        });

        for (const auto& d1 : depth1_dirs) {
            std::vector<std::filesystem::path> depth2_dirs;
            std::error_code ec2;
            for (const auto& d2 : std::filesystem::directory_iterator(
                     d1, std::filesystem::directory_options::skip_permission_denied, ec2)) {
                std::error_code dec2;
                if (d2.is_symlink(dec2) || !d2.is_directory(dec2)) continue;
                const std::string n2 = d2.path().filename().string();
                if (n2.empty() || n2.front() == '.') continue;
                depth2_dirs.push_back(d2.path());
            }
            const std::string d1_str = d1.string();
            const bool is_exp = expanded.contains(d1_str);
            stage1->sidebar_folders.push_back(ScannedFolderNode{
                stage1->arena.intern(d1_str),
                stage1->arena.intern(d1.filename().string()),
                1,
                !depth2_dirs.empty(),
                is_exp,
                nav_kind == LibraryNavKind::Folder && cur_folder == d1_str,
            });

            if (is_exp && !depth2_dirs.empty()) {
                std::sort(depth2_dirs.begin(), depth2_dirs.end(), [](const auto& a, const auto& b) {
                    return icase_less(a.filename().string(), b.filename().string());
                });
                for (const auto& d2 : depth2_dirs) {
                    const std::string d2_str = d2.string();
                    stage1->sidebar_folders.push_back(ScannedFolderNode{
                        stage1->arena.intern(d2_str),
                        stage1->arena.intern(d2.filename().string()),
                        2,
                        false,
                        false,
                        nav_kind == LibraryNavKind::Folder && cur_folder == d2_str,
                    });
                }
            }
        }
    }

    if (epoch != nav_epoch_.load(std::memory_order_acquire)) return;

    std::vector<std::filesystem::path> shelf_dirs;
    if (nav_kind == LibraryNavKind::Home || nav_kind == LibraryNavKind::Folder) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(
                 cur_folder, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (epoch != nav_epoch_.load(std::memory_order_relaxed)) return;
            std::error_code tec;
            if (entry.is_symlink(tec)) continue;
            if (entry.is_directory(tec)) {
                const std::string dname = entry.path().filename().string();
                if (!dname.empty() && dname.front() != '.') {
                    shelf_dirs.push_back(entry.path());
                }
                continue;
            }
            if (auto item = make_video_item(entry, stage1->arena)) {
                stage1->direct_videos.push_back(std::move(*item));
            }
        }
        sort_items(stage1->direct_videos, stage1->arena);
        std::sort(shelf_dirs.begin(), shelf_dirs.end(), [](const auto& a, const auto& b) {
            return icase_less(a.filename().string(), b.filename().string());
        });
        for (const auto& sd : shelf_dirs) {
            stage1->shelves.push_back(ScannedShelf{
                stage1->arena.intern(sd.string()),
                stage1->arena.intern(sd.filename().string()),
                0,
                {},
            });
        }
    } else {
        // ContinueWatching or Recent: recursive scan of videos_root_ (bounded)
        std::error_code ec;
        size_t visited = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 videos_root_, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (epoch != nav_epoch_.load(std::memory_order_relaxed)) return;
            if (++visited > 5000) break;
            if (auto item = make_video_item(entry, stage1->arena)) {
                if (nav_kind == LibraryNavKind::ContinueWatching) {
                    if (!item->watched && item->progress >= 0.05f && item->progress < 0.95f) {
                        stage1->direct_videos.push_back(std::move(*item));
                    }
                } else {
                    stage1->direct_videos.push_back(std::move(*item));
                }
            }
        }
        if (nav_kind == LibraryNavKind::ContinueWatching) {
            std::sort(stage1->direct_videos.begin(), stage1->direct_videos.end(),
                [](const ScannedVideoItem& a, const ScannedVideoItem& b) {
                    return a.last_watched_at > b.last_watched_at;
                });
        } else {
            std::sort(stage1->direct_videos.begin(), stage1->direct_videos.end(),
                [](const ScannedVideoItem& a, const ScannedVideoItem& b) {
                    return a.mtime_ns > b.mtime_ns;
                });
        }
    }

    if (epoch != nav_epoch_.load(std::memory_order_acquire)) return;

    // Publish Stage 1 immediately
    {
        bool should_schedule = false;
        {
            std::lock_guard lock(outbox_mutex_);
            pending_snapshot_ = stage1;
            should_schedule = take_ui_flush_slot_locked();
        }
        notify_ui_flush(should_schedule);
    }

    // Stage 2: Populate Continue Watching shelf (Home) & Subcategory Shelves
    const bool need_stage2 = (nav_kind == LibraryNavKind::Home) || !shelf_dirs.empty();
    if (!need_stage2) {
        // Push background thumbnail jobs for direct_videos
        std::lock_guard qlock(queue_mutex_);
        scheduler_.reset_for_epoch();
        for (const auto& item : stage1->direct_videos) {
            scheduler_.push_background(ThumbnailJob{
                epoch,
                item.cache_key,
                std::string(stage1->arena.view(item.id_hex)),
                std::string(stage1->arena.view(item.path)),
            });
        }
        queue_cv_.notify_all();
        return;
    }

    auto stage2 = std::make_shared<LibraryViewSnapshot>();
    stage2->epoch = epoch;
    stage2->shelves_complete = true;
    stage2->nav_kind = stage1->nav_kind;
    stage2->sort_mode = stage1->sort_mode;
    stage2->arena = stage1->arena;
    stage2->root_path = stage1->root_path;
    stage2->current_folder_path = stage1->current_folder_path;
    stage2->breadcrumbs = stage1->breadcrumbs;
    stage2->breadcrumb_paths = stage1->breadcrumb_paths;
    stage2->sidebar_folders = stage1->sidebar_folders;
    stage2->direct_videos = stage1->direct_videos;

    for (const auto& sd : shelf_dirs) {
        if (epoch != nav_epoch_.load(std::memory_order_acquire)) return;
        ScannedShelf shelf;
        shelf.folder_path = stage2->arena.intern(sd.string());
        shelf.title = stage2->arena.intern(sd.filename().string());

        std::vector<ScannedVideoItem> found;
        std::error_code ec;
        size_t budget = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 sd, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (epoch != nav_epoch_.load(std::memory_order_relaxed)) return;
            if (++budget > kShelfTraversalBudget) break;
            if (auto item = make_video_item(entry, stage2->arena)) {
                found.push_back(std::move(*item));
            }
        }
        shelf.total_count = static_cast<int>(found.size());
        sort_items(found, stage2->arena);
        if (found.size() > kShelfPreviewCap) {
            found.resize(kShelfPreviewCap);
        }
        shelf.items = std::move(found);
        if (shelf.total_count > 0 || search.empty()) {
            stage2->shelves.push_back(std::move(shelf));
        }
    }

    if (nav_kind == LibraryNavKind::Home) {
        for (const auto& item : stage2->direct_videos) {
            if (!item.watched && item.progress >= 0.05f && item.progress < 0.95f) {
                stage2->continue_watching.push_back(item);
            }
        }
        for (const auto& shelf : stage2->shelves) {
            for (const auto& item : shelf.items) {
                if (!item.watched && item.progress >= 0.05f && item.progress < 0.95f) {
                    stage2->continue_watching.push_back(item);
                }
            }
        }
        std::sort(stage2->continue_watching.begin(), stage2->continue_watching.end(),
            [](const ScannedVideoItem& a, const ScannedVideoItem& b) {
                return a.last_watched_at > b.last_watched_at;
            });
        if (stage2->continue_watching.size() > kShelfPreviewCap) {
            stage2->continue_watching.resize(kShelfPreviewCap);
        }
    }

    if (epoch != nav_epoch_.load(std::memory_order_acquire)) return;

    {
        bool should_schedule = false;
        {
            std::lock_guard lock(outbox_mutex_);
            pending_snapshot_ = stage2;
            should_schedule = take_ui_flush_slot_locked();
        }
        notify_ui_flush(should_schedule);
    }

    // Enqueue background thumbnail jobs
    {
        std::lock_guard qlock(queue_mutex_);
        scheduler_.reset_for_epoch();
        for (const auto& item : stage2->continue_watching) {
            scheduler_.push_background(ThumbnailJob{
                epoch,
                item.cache_key,
                std::string(stage2->arena.view(item.id_hex)),
                std::string(stage2->arena.view(item.path)),
            });
        }
        for (const auto& item : stage2->direct_videos) {
            scheduler_.push_background(ThumbnailJob{
                epoch,
                item.cache_key,
                std::string(stage2->arena.view(item.id_hex)),
                std::string(stage2->arena.view(item.path)),
            });
        }
        for (const auto& shelf : stage2->shelves) {
            for (const auto& item : shelf.items) {
                scheduler_.push_background(ThumbnailJob{
                    epoch,
                    item.cache_key,
                    std::string(stage2->arena.view(item.id_hex)),
                    std::string(stage2->arena.view(item.path)),
                });
            }
        }
    }
    queue_cv_.notify_all();
}

void LibraryService::thumb_worker_loop(std::stop_token stop) {
    ThreadLocalThumbExtractor extractor;

    while (!stop.stop_requested()) {
        std::optional<ThumbnailJob> job;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, stop, [this]() { return !scheduler_.empty(); });
            if (stop.stop_requested()) break;
            job = scheduler_.pop_next(nav_epoch_.load(std::memory_order_acquire));
        }
        if (!job.has_value()) continue;

        const uint64_t current_ep = nav_epoch_.load(std::memory_order_acquire);
        if (job->epoch != current_ep) continue;

        // 1. Check L1 LRU cache
        std::optional<slint::SharedPixelBuffer<slint::Rgba8Pixel>> pixels;
        {
            std::lock_guard lock(queue_mutex_);
            if (auto it = l1_pixels_.find(job->cache_key); it != l1_pixels_.end()) {
                lru_order_.splice(lru_order_.begin(), lru_order_, it->second.second);
                pixels = it->second.first;
            }
        }

        double dur_sec = 0.0;
        {
            std::lock_guard lock(state_mutex_);
            if (auto it = media_index_.find(job->cache_key); it != media_index_.end()) {
                dur_sec = it->second.duration_sec;
            }
        }

        const std::filesystem::path mpxt_path = thumb_dir_ / std::format("{}.mpxt", job->id_hex);

        // 2. Check L2 .mpxt disk cache
        if (!pixels.has_value()) {
            pixels = read_mpxt_thumbnail(mpxt_path);
        }

        // 3. Generate via thread-local libmpv SW render context (or ffmpeg fallback)
        if (!pixels.has_value()) {
            slint::SharedPixelBuffer<slint::Rgba8Pixel> gen_buf;
            uint32_t vw = 0;
            uint32_t vh = 0;
            double gen_dur = 0.0;
            bool ok = extractor.extract(
                job->path, stop, nav_epoch_, job->epoch, gen_dur, vw, vh, gen_buf);
            if (!ok && !stop.stop_requested() &&
                nav_epoch_.load(std::memory_order_relaxed) == job->epoch) {
                ok = extract_thumb_ffmpeg_fallback(job->path, gen_buf);
            }
            if (ok) {
                pixels = std::move(gen_buf);
                (void)write_mpxt_thumbnail_atomic(mpxt_path, *pixels);
                if (gen_dur > 0.0) {
                    dur_sec = gen_dur;
                }
                {
                    std::lock_guard lock(state_mutex_);
                    auto& rec = media_index_[job->cache_key];
                    rec.cache_key = job->cache_key;
                    if (dur_sec > 0.0) rec.duration_sec = dur_sec;
                    if (vw > 0) rec.width = vw;
                    if (vh > 0) rec.height = vh;
                    rec.flags |= 1U;
                    index_dirty_ = true;
                }
                {
                    std::lock_guard lock(cmd_mutex_);
                    pending_index_flush_ = true;
                }
                cmd_cv_.notify_one();
            }
        }

        if (!pixels.has_value()) {
            continue;
        }

        // Store in L1 LRU
        {
            std::lock_guard lock(queue_mutex_);
            if (auto it = l1_pixels_.find(job->cache_key); it == l1_pixels_.end()) {
                if (l1_pixels_.size() >= kL1PixelCacheCapacity && !lru_order_.empty()) {
                    const uint64_t evict_key = lru_order_.back();
                    lru_order_.pop_back();
                    l1_pixels_.erase(evict_key);
                }
                lru_order_.push_front(job->cache_key);
                l1_pixels_.emplace(job->cache_key, std::make_pair(*pixels, lru_order_.begin()));
            }
        }

        if (job->epoch != nav_epoch_.load(std::memory_order_acquire)) {
            continue;
        }

        {
            bool should_schedule = false;
            {
                std::lock_guard lock(outbox_mutex_);
                pending_ui_updates_.push_back(ThumbnailUpdate{
                    job->epoch,
                    job->cache_key,
                    std::move(job->id_hex),
                    std::move(job->path),
                    dur_sec,
                    dur_sec > 0.0 ? format_timestamp(dur_sec) : std::string{},
                    true,
                    std::move(*pixels),
                });
                should_schedule = take_ui_flush_slot_locked();
            }
            notify_ui_flush(should_schedule);
        }
    }
}

} // namespace mpvx
