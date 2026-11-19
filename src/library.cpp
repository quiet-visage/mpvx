#include "mpvx/library.hpp"

#include <mpv/client.h>
#include <mpv/render.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
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

// ============================================================================
// Zero-Decoder Container Cover-Art & Metadata Probing (EBML + ISOBMFF)
// ============================================================================

constexpr size_t kMaxCoverPayloadBytes = 8ULL * 1024ULL * 1024ULL;
constexpr size_t kMaxMetaSectionBytes = 512ULL * 1024ULL;

[[nodiscard]] std::string sniff_image_format(std::span<const uint8_t> bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return "jpg";
    }
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E &&
        bytes[3] == 0x47 && bytes[4] == 0x0D && bytes[5] == 0x0A && bytes[6] == 0x1A &&
        bytes[7] == 0x0A) {
        return "png";
    }
    if (bytes.size() >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' &&
        bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' &&
        bytes[11] == 'P') {
        return "webp";
    }
    if (bytes.size() >= 4 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' &&
        bytes[3] == '8') {
        return "gif";
    }
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
        return "bmp";
    }
    return {};
}

[[nodiscard]] uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

[[nodiscard]] uint64_t read_be64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(read_be32(p)) << 32) |
           static_cast<uint64_t>(read_be32(p + 4));
}

[[nodiscard]] uint64_t read_be_uint(std::span<const uint8_t> bytes) noexcept {
    uint64_t v = 0;
    const size_t n = std::min<size_t>(bytes.size(), 8);
    for (size_t i = 0; i < n; ++i) {
        v = (v << 8) | bytes[i];
    }
    return v;
}

[[nodiscard]] double read_ebml_float(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() == 4) {
        const uint32_t raw = read_be32(bytes.data());
        float f = 0.0f;
        std::memcpy(&f, &raw, sizeof(f));
        return static_cast<double>(f);
    }
    if (bytes.size() == 8) {
        const uint64_t raw = read_be64(bytes.data());
        double d = 0.0;
        std::memcpy(&d, &raw, sizeof(d));
        return d;
    }
    return 0.0;
}

bool read_ebml_id(std::span<const uint8_t> buf, size_t& pos, uint32_t& out_id) noexcept {
    if (pos >= buf.size()) return false;
    const uint8_t b = buf[pos];
    if (b == 0) return false;
    int len = 0;
    for (int i = 0; i < 4; ++i) {
        if (b & (0x80U >> i)) {
            len = i + 1;
            break;
        }
    }
    if (len == 0 || pos + static_cast<size_t>(len) > buf.size()) return false;
    uint32_t id = 0;
    for (int i = 0; i < len; ++i) {
        id = (id << 8) | buf[pos + static_cast<size_t>(i)];
    }
    pos += static_cast<size_t>(len);
    out_id = id;
    return true;
}

bool read_ebml_size(
    std::span<const uint8_t> buf,
    size_t& pos,
    uint64_t& out_size,
    bool& out_unknown) noexcept
{
    if (pos >= buf.size()) return false;
    const uint8_t b = buf[pos];
    if (b == 0) return false;
    int len = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & (0x80U >> i)) {
            len = i + 1;
            break;
        }
    }
    if (len == 0 || pos + static_cast<size_t>(len) > buf.size()) return false;
    const uint8_t first_mask = static_cast<uint8_t>((1U << (8 - len)) - 1U);
    uint64_t val = buf[pos] & first_mask;
    uint64_t all_ones = first_mask;
    for (int i = 1; i < len; ++i) {
        val = (val << 8) | buf[pos + static_cast<size_t>(i)];
        all_ones = (all_ones << 8) | 0xFFULL;
    }
    pos += static_cast<size_t>(len);
    out_unknown = (val == all_ones);
    out_size = val;
    return true;
}

bool read_ebml_element(
    std::span<const uint8_t> buf,
    size_t& pos,
    uint32_t& out_id,
    std::span<const uint8_t>& out_payload) noexcept
{
    uint64_t sz = 0;
    bool unk = false;
    if (!read_ebml_id(buf, pos, out_id) || !read_ebml_size(buf, pos, sz, unk) || unk) {
        return false;
    }
    if (sz > buf.size() - pos) {
        return false;
    }
    out_payload = buf.subspan(pos, static_cast<size_t>(sz));
    pos += static_cast<size_t>(sz);
    return true;
}

void parse_mkv_info(std::span<const uint8_t> info_data, double& out_duration_sec) noexcept {
    uint64_t timecode_scale = 1'000'000ULL;
    double raw_duration = 0.0;
    size_t pos = 0;
    int iters = 0;
    while (pos < info_data.size() && ++iters < 512) {
        uint32_t id = 0;
        std::span<const uint8_t> payload;
        if (!read_ebml_element(info_data, pos, id, payload)) {
            break;
        }
        if (id == 0x2AD7B1U) {
            const uint64_t ts = read_be_uint(payload);
            if (ts > 0) timecode_scale = ts;
        } else if (id == 0x4489U) {
            raw_duration = read_ebml_float(payload);
        }
    }
    if (raw_duration > 0.0) {
        out_duration_sec = raw_duration * (static_cast<double>(timecode_scale) / 1'000'000'000.0);
    }
}

void parse_mkv_tracks(
    std::span<const uint8_t> tracks_data,
    uint32_t& out_width,
    uint32_t& out_height,
    int& out_video_tracks) noexcept
{
    size_t pos = 0;
    int iters = 0;
    while (pos < tracks_data.size() && ++iters < 256) {
        uint32_t id = 0;
        std::span<const uint8_t> entry;
        if (!read_ebml_element(tracks_data, pos, id, entry)) {
            break;
        }
        if (id != 0xAEU) continue; // TrackEntry

        uint64_t track_type = 0;
        uint32_t vw = 0;
        uint32_t vh = 0;
        size_t epos = 0;
        int eiters = 0;
        while (epos < entry.size() && ++eiters < 128) {
            uint32_t eid = 0;
            std::span<const uint8_t> epay;
            if (!read_ebml_element(entry, epos, eid, epay)) {
                break;
            }
            if (eid == 0x83U) {
                track_type = read_be_uint(epay);
            } else if (eid == 0xE0U) { // Video
                size_t vpos = 0;
                int viters = 0;
                while (vpos < epay.size() && ++viters < 64) {
                    uint32_t vid = 0;
                    std::span<const uint8_t> vpay;
                    if (!read_ebml_element(epay, vpos, vid, vpay)) {
                        break;
                    }
                    if (vid == 0xB0U) {
                        vw = static_cast<uint32_t>(read_be_uint(vpay));
                    } else if (vid == 0xBAU) {
                        vh = static_cast<uint32_t>(read_be_uint(vpay));
                    }
                }
            }
        }
        if (track_type == 1) {
            ++out_video_tracks;
            if (out_width == 0 && out_height == 0 && vw > 0 && vh > 0) {
                out_width = vw;
                out_height = vh;
            }
        }
    }
}

[[nodiscard]] int score_cover_candidate(std::string_view fname, std::string_view mime) noexcept {
    // Magic bytes are already verified by sniff_image_format(), so accept generic
    // MIME types like application/octet-stream with a lower preference score.
    int score = (!mime.empty() && !icase_contains(mime, "image/")) ? 6 : 10;
    if (icase_contains(fname, "cover") || icase_contains(fname, "folder") ||
        icase_contains(fname, "poster") || icase_contains(fname, "thumb")) {
        score += 100;
    }
    if (icase_contains(fname, "small")) {
        score -= 5;
    }
    return score;
}

void parse_mkv_attachments(
    std::span<const uint8_t> att_data,
    ContainerProbeResult& out)
{
    int best_score = -1;
    size_t pos = 0;
    int iters = 0;
    while (pos < att_data.size() && ++iters < 256) {
        uint32_t id = 0;
        std::span<const uint8_t> attached;
        if (!read_ebml_element(att_data, pos, id, attached)) {
            break;
        }
        if (id != 0x61A7U) continue; // AttachedFile

        std::string_view fname;
        std::string_view mime;
        std::span<const uint8_t> file_data;
        size_t apos = 0;
        int aiters = 0;
        while (apos < attached.size() && ++aiters < 64) {
            uint32_t aid = 0;
            std::span<const uint8_t> apay;
            if (!read_ebml_element(attached, apos, aid, apay)) {
                break;
            }
            if (aid == 0x466EU) {
                fname = std::string_view(reinterpret_cast<const char*>(apay.data()), apay.size());
            } else if (aid == 0x4660U) {
                mime = std::string_view(reinterpret_cast<const char*>(apay.data()), apay.size());
            } else if (aid == 0x465CU) {
                file_data = apay;
            }
        }

        if (file_data.empty() || file_data.size() > kMaxCoverPayloadBytes) continue;
        std::string fmt = sniff_image_format(file_data);
        if (fmt.empty()) continue;

        const int score = score_cover_candidate(fname, mime);
        if (score > best_score) {
            best_score = score;
            out.has_embedded_cover = true;
            out.cover_format = std::move(fmt);
            out.cover_data.assign(file_data.begin(), file_data.end());
        }
    }
}

[[nodiscard]] bool read_file_slice(
    std::ifstream& ifs,
    uint64_t offset,
    size_t max_bytes,
    std::vector<uint8_t>& out)
{
    ifs.clear();
    ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!ifs) return false;
    out.resize(max_bytes);
    ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(max_bytes));
    const auto got = ifs.gcount();
    if (got <= 0) {
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(got));
    return true;
}

[[nodiscard]] ContainerProbeResult probe_matroska_container(
    std::ifstream& ifs,
    std::span<const uint8_t> initial_hdr,
    uint64_t file_size)
{
    ContainerProbeResult res;
    size_t pos = 0;
    uint32_t ebml_id = 0;
    uint64_t ebml_sz = 0;
    bool ebml_unk = false;
    if (!read_ebml_id(initial_hdr, pos, ebml_id) || ebml_id != 0x1A45DFA3U ||
        !read_ebml_size(initial_hdr, pos, ebml_sz, ebml_unk) || ebml_unk ||
        ebml_sz > initial_hdr.size() - pos) {
        return res;
    }
    pos += static_cast<size_t>(ebml_sz);
    if (pos >= initial_hdr.size()) return res;

    uint32_t seg_id = 0;
    uint64_t seg_sz = 0;
    bool seg_unk = false;
    if (!read_ebml_id(initial_hdr, pos, seg_id) || seg_id != 0x18538067U ||
        !read_ebml_size(initial_hdr, pos, seg_sz, seg_unk)) {
        return res;
    }

    res.parsed = true;
    const uint64_t seg_data_offset = pos;
    bool got_info = false;
    bool got_tracks = false;
    bool got_attachments = false;
    int mkv_video_tracks = 0;

    uint64_t seek_info_off = 0;
    uint64_t seek_tracks_off = 0;
    uint64_t seek_att_off = 0;
    bool has_seek_info = false;
    bool has_seek_tracks = false;
    bool has_seek_att = false;

    auto parse_seek_head = [&](std::span<const uint8_t> sh_data) {
        size_t spos = 0;
        int siters = 0;
        while (spos < sh_data.size() && ++siters < 128) {
            uint32_t sid = 0;
            std::span<const uint8_t> entry;
            if (!read_ebml_element(sh_data, spos, sid, entry)) {
                break;
            }
            if (sid != 0x4DBBU) continue;

            uint32_t target_id = 0;
            uint64_t target_pos = 0;
            bool have_id = false;
            bool have_pos = false;
            size_t epos = 0;
            while (epos < entry.size()) {
                uint32_t eid = 0;
                std::span<const uint8_t> epay;
                if (!read_ebml_element(entry, epos, eid, epay)) {
                    break;
                }
                if (eid == 0x53ABU && epay.size() <= 4) {
                    target_id = static_cast<uint32_t>(read_be_uint(epay));
                    have_id = true;
                } else if (eid == 0x53ACU && epay.size() <= 8) {
                    target_pos = read_be_uint(epay);
                    have_pos = true;
                }
            }
            if (have_id && have_pos && seg_data_offset <= file_size &&
                target_pos <= file_size - seg_data_offset) {
                const uint64_t abs_off = seg_data_offset + target_pos;
                if (target_id == 0x1549A966U && !has_seek_info) {
                    seek_info_off = abs_off;
                    has_seek_info = true;
                } else if (target_id == 0x1654AE6BU && !has_seek_tracks) {
                    seek_tracks_off = abs_off;
                    has_seek_tracks = true;
                } else if (target_id == 0x1941A469U && !has_seek_att) {
                    seek_att_off = abs_off;
                    has_seek_att = true;
                }
            }
        }
    };

    auto dispatch_top_element = [&](uint32_t cid, std::span<const uint8_t> payload) {
        if (cid == 0x114D9B74U) {
            parse_seek_head(payload);
        } else if (cid == 0x1549A966U && !got_info) {
            parse_mkv_info(payload, res.duration_sec);
            got_info = true;
        } else if (cid == 0x1654AE6BU && !got_tracks) {
            parse_mkv_tracks(payload, res.width, res.height, mkv_video_tracks);
            got_tracks = true;
        } else if (cid == 0x1941A469U && !got_attachments) {
            parse_mkv_attachments(payload, res);
            got_attachments = true;
        }
    };

    // Scan top-level elements present in initial header buffer
    int iters = 0;
    while (pos < initial_hdr.size() && ++iters < 64) {
        const size_t elem_start = pos;
        uint32_t cid = 0;
        uint64_t csz = 0;
        bool cunk = false;
        if (!read_ebml_id(initial_hdr, pos, cid) || !read_ebml_size(initial_hdr, pos, csz, cunk)) {
            break;
        }
        if (cid == 0x1F43B675U) { // Cluster reached
            break;
        }
        if (cunk) break;
        if (csz <= initial_hdr.size() - pos) {
            dispatch_top_element(cid, initial_hdr.subspan(pos, static_cast<size_t>(csz)));
            pos += static_cast<size_t>(csz);
        } else {
            // Element extends past initial buffer; record SeekHead/element offset for targeted read
            if (cid == 0x1941A469U && !has_seek_att) {
                seek_att_off = elem_start;
                has_seek_att = true;
            } else if (cid == 0x1549A966U && !has_seek_info) {
                seek_info_off = elem_start;
                has_seek_info = true;
            } else if (cid == 0x1654AE6BU && !has_seek_tracks) {
                seek_tracks_off = elem_start;
                has_seek_tracks = true;
            }
            break;
        }
    }

    auto read_element_at = [&](uint32_t expected_id, uint64_t offset, size_t cap) {
        if (offset >= file_size) return;
        std::vector<uint8_t> hdr_peek;
        if (!read_file_slice(ifs, offset, 16, hdr_peek)) return;
        size_t p = 0;
        uint32_t cid = 0;
        uint64_t csz = 0;
        bool unk = false;
        if (!read_ebml_id(hdr_peek, p, cid) || cid != expected_id ||
            !read_ebml_size(hdr_peek, p, csz, unk) || unk || csz == 0 || csz > cap) {
            return;
        }
        if (p > file_size - offset || csz > (file_size - offset) - p) {
            return;
        }
        std::vector<uint8_t> payload;
        if (read_file_slice(ifs, offset + p, static_cast<size_t>(csz), payload) &&
            payload.size() == csz) {
            dispatch_top_element(cid, payload);
        }
    };

    if (!got_info && has_seek_info) {
        read_element_at(0x1549A966U, seek_info_off, kMaxMetaSectionBytes);
    }
    if (!got_tracks && has_seek_tracks) {
        read_element_at(0x1654AE6BU, seek_tracks_off, kMaxMetaSectionBytes);
    }
    if (!got_attachments && has_seek_att) {
        read_element_at(0x1941A469U, seek_att_off, kMaxCoverPayloadBytes + 4096);
    }

    // Only allow skipping libmpv track-list inspection when Tracks was actually parsed
    // and confirmed a single video stream without embedded cover art.
    if ((!got_tracks || mkv_video_tracks != 1) && !res.has_embedded_cover) {
        res.parsed = false;
    }

    return res;
}

constexpr uint32_t fourcc(char a, char b, char c, char d) noexcept {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(d));
}

struct Mp4BoxHeader {
    uint32_t type{0};
    uint64_t header_size{0};
    uint64_t box_size{0};
};

bool read_mp4_box_header(
    std::ifstream& ifs,
    uint64_t offset,
    uint64_t limit,
    Mp4BoxHeader& out)
{
    if (offset > limit || limit - offset < 8) return false;
    uint8_t buf[16];
    ifs.clear();
    ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    const size_t to_read = (limit - offset >= 16) ? 16 : 8;
    ifs.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(to_read));
    if (ifs.gcount() < 8) return false;

    const uint32_t sz32 = read_be32(buf);
    const uint32_t type = read_be32(buf + 4);
    uint64_t hdr_sz = 8;
    uint64_t full_sz = sz32;
    if (sz32 == 1) {
        if (ifs.gcount() < 16) return false;
        full_sz = read_be64(buf + 8);
        hdr_sz = 16;
    } else if (sz32 == 0) {
        full_sz = limit - offset;
    }
    if (full_sz < hdr_sz || full_sz > limit - offset) return false;
    out = Mp4BoxHeader{type, hdr_sz, full_sz};
    return true;
}

void parse_mp4_meta_box(
    std::ifstream& ifs,
    uint64_t meta_payload_off,
    uint64_t meta_payload_end,
    ContainerProbeResult& res)
{
    // `meta` is normally a FullBox (4 bytes version/flags), though some muxers omit it
    uint64_t start = meta_payload_off;
    Mp4BoxHeader probe{};
    if (start <= meta_payload_end && meta_payload_end - start >= 4 &&
        read_mp4_box_header(ifs, start + 4, meta_payload_end, probe) &&
        (probe.type == fourcc('h', 'd', 'l', 'r') || probe.type == fourcc('i', 'l', 's', 't'))) {
        start += 4;
    }

    uint64_t pos = start;
    int iters = 0;
    while (pos <= meta_payload_end && meta_payload_end - pos >= 8 && ++iters < 128) {
        Mp4BoxHeader child{};
        if (!read_mp4_box_header(ifs, pos, meta_payload_end, child)) break;
        if (child.type == fourcc('i', 'l', 's', 't')) {
            uint64_t ipos = pos + child.header_size;
            const uint64_t iend = pos + child.box_size;
            int iiters = 0;
            while (ipos <= iend && iend - ipos >= 8 && ++iiters < 256) {
                Mp4BoxHeader item{};
                if (!read_mp4_box_header(ifs, ipos, iend, item)) break;
                if (item.type == fourcc('c', 'o', 'v', 'r')) {
                    uint64_t cpos = ipos + item.header_size;
                    const uint64_t cend = ipos + item.box_size;
                    int citers = 0;
                    while (cpos <= cend && cend - cpos >= 8 && ++citers < 32) {
                        Mp4BoxHeader dbox{};
                        if (!read_mp4_box_header(ifs, cpos, cend, dbox)) break;
                        const uint64_t data_hdr = dbox.header_size + 8; // type(4) + locale(4)
                        if (dbox.type == fourcc('d', 'a', 't', 'a') && dbox.box_size > data_hdr) {
                            const uint64_t img_sz = dbox.box_size - data_hdr;
                            if (img_sz <= kMaxCoverPayloadBytes) {
                                std::vector<uint8_t> img_bytes;
                                if (read_file_slice(ifs, cpos + data_hdr, static_cast<size_t>(img_sz), img_bytes) &&
                                    img_bytes.size() == img_sz) {
                                    std::string fmt = sniff_image_format(img_bytes);
                                    if (!fmt.empty()) {
                                        res.has_embedded_cover = true;
                                        res.cover_format = std::move(fmt);
                                        res.cover_data = std::move(img_bytes);
                                        return;
                                    }
                                }
                            }
                        }
                        cpos += dbox.box_size;
                    }
                }
                ipos += item.box_size;
            }
        }
        pos += child.box_size;
    }
}

[[nodiscard]] ContainerProbeResult probe_isobmff_container(
    std::ifstream& ifs,
    uint64_t file_size)
{
    ContainerProbeResult res;
    uint64_t pos = 0;
    uint64_t moov_off = 0;
    uint64_t moov_sz = 0;
    bool has_moov = false;

    int top_iters = 0;
    while (pos <= file_size && file_size - pos >= 8 && ++top_iters < 256) {
        Mp4BoxHeader box{};
        if (!read_mp4_box_header(ifs, pos, file_size, box)) break;
        if (box.type == fourcc('m', 'o', 'o', 'v')) {
            moov_off = pos + box.header_size;
            moov_sz = box.box_size - box.header_size;
            has_moov = true;
            break;
        }
        pos += box.box_size;
    }

    if (!has_moov) {
        return res;
    }
    res.parsed = true;

    const uint64_t moov_end = moov_off + moov_sz;
    uint64_t mpos = moov_off;
    int miters = 0;
    int vide_track_count = 0;
    while (mpos <= moov_end && moov_end - mpos >= 8 && ++miters < 512) {
        Mp4BoxHeader child{};
        if (!read_mp4_box_header(ifs, mpos, moov_end, child)) break;
        const uint64_t cstart = mpos + child.header_size;
        const uint64_t cend = mpos + child.box_size;

        if (child.type == fourcc('m', 'v', 'h', 'd')) {
            std::vector<uint8_t> mv;
            if (read_file_slice(ifs, cstart, std::min<size_t>(128, static_cast<size_t>(cend - cstart)), mv) &&
                mv.size() >= 20) {
                const uint8_t version = mv[0];
                if (version == 0 && mv.size() >= 20) {
                    const uint32_t ts = read_be32(mv.data() + 12);
                    const uint32_t dur = read_be32(mv.data() + 16);
                    if (ts > 0 && dur > 0) {
                        res.duration_sec = static_cast<double>(dur) / static_cast<double>(ts);
                    }
                } else if (version == 1 && mv.size() >= 32) {
                    const uint32_t ts = read_be32(mv.data() + 20);
                    const uint64_t dur = read_be64(mv.data() + 24);
                    if (ts > 0 && dur > 0) {
                        res.duration_sec = static_cast<double>(dur) / static_cast<double>(ts);
                    }
                }
            }
        } else if (child.type == fourcc('t', 'r', 'a', 'k')) {
            uint64_t tpos = cstart;
            int titers = 0;
            uint32_t tkhd_w = 0;
            uint32_t tkhd_h = 0;
            bool is_vide = false;
            while (tpos + 8 <= cend && ++titers < 64) {
                Mp4BoxHeader tb{};
                if (!read_mp4_box_header(ifs, tpos, cend, tb)) break;
                const uint64_t tb_start = tpos + tb.header_size;
                const uint64_t tb_end = tpos + tb.box_size;
                if (tb.type == fourcc('t', 'k', 'h', 'd')) {
                    std::vector<uint8_t> tk;
                    if (read_file_slice(ifs, tb_start, std::min<size_t>(128, static_cast<size_t>(tb_end - tb_start)), tk) &&
                        !tk.empty()) {
                        const uint8_t ver = tk[0];
                        if (ver == 0 && tk.size() >= 84) {
                            tkhd_w = read_be32(tk.data() + 76) >> 16;
                            tkhd_h = read_be32(tk.data() + 80) >> 16;
                        } else if (ver == 1 && tk.size() >= 96) {
                            tkhd_w = read_be32(tk.data() + 88) >> 16;
                            tkhd_h = read_be32(tk.data() + 92) >> 16;
                        }
                    }
                } else if (tb.type == fourcc('m', 'd', 'i', 'a')) {
                    uint64_t mdpos = tb_start;
                    int mditers = 0;
                    while (mdpos + 8 <= tb_end && ++mditers < 32) {
                        Mp4BoxHeader mb{};
                        if (!read_mp4_box_header(ifs, mdpos, tb_end, mb)) break;
                        if (mb.type == fourcc('h', 'd', 'l', 'r')) {
                            std::vector<uint8_t> hd;
                            if (read_file_slice(ifs, mdpos + mb.header_size, 16, hd) && hd.size() >= 12) {
                                if (read_be32(hd.data() + 8) == fourcc('v', 'i', 'd', 'e')) {
                                    is_vide = true;
                                }
                            }
                            break;
                        }
                        mdpos += mb.box_size;
                    }
                }
                tpos += tb.box_size;
            }
            if (is_vide) {
                ++vide_track_count;
                if ((res.width == 0 || res.height == 0) && tkhd_w > 0 && tkhd_h > 0) {
                    res.width = tkhd_w;
                    res.height = tkhd_h;
                }
            }
        } else if (child.type == fourcc('u', 'd', 't', 'a') && !res.has_embedded_cover) {
            uint64_t upos = cstart;
            int uiters = 0;
            while (upos + 8 <= cend && ++uiters < 64) {
                Mp4BoxHeader ub{};
                if (!read_mp4_box_header(ifs, upos, cend, ub)) break;
                if (ub.type == fourcc('m', 'e', 't', 'a')) {
                    parse_mp4_meta_box(ifs, upos + ub.header_size, upos + ub.box_size, res);
                    if (res.has_embedded_cover) break;
                }
                upos += ub.box_size;
            }
        } else if (child.type == fourcc('m', 'e', 't', 'a') && !res.has_embedded_cover) {
            parse_mp4_meta_box(ifs, cstart, cend, res);
        }

        mpos += child.box_size;
    }

    // Multiple (or zero) video tracks without an explicit `covr` atom may contain an
    // `attached_pic` track; let libmpv's track-list inspector handle those rather than skipping Load #1.
    if (vide_track_count != 1 && !res.has_embedded_cover) {
        res.parsed = false;
    }

    return res;
}

[[nodiscard]] std::optional<slint::SharedPixelBuffer<slint::Rgba8Pixel>> try_load_sidecar_cover(
    const std::filesystem::path& video_path)
{
    static constexpr std::array<std::string_view, 4> kExts = {".webp", ".jpg", ".jpeg", ".png"};
    const std::filesystem::path parent = video_path.parent_path();
    const std::string stem = video_path.stem().string();
    if (stem.empty()) return std::nullopt;

    for (std::string_view ext : kExts) {
        const std::filesystem::path candidate = parent / (stem + std::string(ext));
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) continue;
        const uint64_t fsz = std::filesystem::file_size(candidate, ec);
        if (ec || fsz == 0 || fsz > kMaxCoverPayloadBytes) continue;

        std::ifstream ifs(candidate, std::ios::binary);
        if (!ifs) continue;
        std::vector<uint8_t> bytes(static_cast<size_t>(fsz));
        ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(fsz));
        if (!ifs || static_cast<uint64_t>(ifs.gcount()) != fsz) continue;

        std::string_view fmt = ext.substr(1);
        if (fmt == "jpeg") fmt = "jpg";
        if (auto pix = decode_and_resize_cover_image(bytes, fmt)) {
            return pix;
        }
    }
    return std::nullopt;
}

// Persistent thread-local libmpv software thumbnail extractor (Tier-2 10% frame fallback)
struct ThreadLocalThumbExtractor {
    mpv_handle* mpv{nullptr};
    mpv_render_context* rctx{nullptr};

    static void on_render_update(void* ctx) noexcept {
        auto* self = static_cast<ThreadLocalThumbExtractor*>(ctx);
        if (self && self->mpv) {
            mpv_wakeup(self->mpv);
        }
    }

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
        mpv_set_option_string(mpv, "vid", "auto");
        mpv_set_option_string(mpv, "aid", "no");
        mpv_set_option_string(mpv, "sid", "no");
        // Software render context (MPV_RENDER_API_TYPE_SW) always reads CPU frames;
        // disabling hwdec, Lua scripts, OSD, and autoloading minimizes per-file overhead.
        mpv_set_option_string(mpv, "hwdec", "no");
        mpv_set_option_string(mpv, "load-scripts", "no");
        mpv_set_option_string(mpv, "osc", "no");
        mpv_set_option_string(mpv, "osd-level", "0");
        mpv_set_option_string(mpv, "sub-auto", "no");
        mpv_set_option_string(mpv, "audio-file-auto", "no");
        mpv_set_option_string(mpv, "autoload-files", "no");
        mpv_set_option_string(mpv, "vd-lavc-threads", "2");
        mpv_set_option_string(mpv, "hr-seek", "no");
        mpv_set_option_string(mpv, "vd-lavc-fast", "yes");
        mpv_set_option_string(mpv, "vd-lavc-skiploopfilter", "all");
        mpv_set_option_string(mpv, "demuxer-readahead-secs", "0");
        mpv_set_option_string(mpv, "cache", "no");
        mpv_set_option_string(mpv, "pause", "yes");
        mpv_set_option_string(mpv, "keep-open", "yes");
        mpv_set_option_string(mpv, "image-display-duration", "inf");
        mpv_set_option_string(mpv, "video-timing-offset", "0");
        mpv_set_option_string(mpv, "quiet", "yes");
        mpv_set_option_string(mpv, "msg-level", "all=no");
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
        mpv_render_context_set_update_callback(rctx, on_render_update, this);
        return true;
    }

    void shutdown() {
        if (rctx) {
            mpv_render_context_set_update_callback(rctx, nullptr, nullptr);
            mpv_render_context_free(rctx);
            rctx = nullptr;
        }
        if (mpv) {
            mpv_terminate_destroy(mpv);
            mpv = nullptr;
        }
    }

    void drain_events() {
        while (true) {
            mpv_event* ev = mpv_wait_event(mpv, 0.0);
            if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        }
        if (rctx) {
            (void)mpv_render_context_update(rctx);
        }
    }

    void stop_and_wait_idle() {
        if (!mpv) return;
        const char* stop_cmd[] = {"stop", nullptr};
        (void)mpv_command(mpv, stop_cmd);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            int idle = 0;
            if (mpv_get_property(mpv, "idle-active", MPV_FORMAT_FLAG, &idle) >= 0 && idle != 0) {
                break;
            }
            mpv_event* ev = mpv_wait_event(mpv, 0.01);
            if (ev && ev->event_id == MPV_EVENT_IDLE) {
                break;
            }
        }
        drain_events();
    }

    bool load_and_wait(
        const std::string& path,
        const char* per_file_options,
        const std::stop_token& stop,
        const std::atomic<uint64_t>& nav_epoch,
        uint64_t job_epoch)
    {
        stop_and_wait_idle();
        int rc = -1;
        if (per_file_options && per_file_options[0] != '\0') {
            const char* cmd_opts[] = {"loadfile", path.c_str(), "replace", "-1", per_file_options, nullptr};
            rc = mpv_command(mpv, cmd_opts);
            if (rc < 0) {
                const char* cmd_opts_legacy[] = {"loadfile", path.c_str(), "replace", per_file_options, nullptr};
                rc = mpv_command(mpv, cmd_opts_legacy);
            }
        } else {
            const char* cmd_plain[] = {"loadfile", path.c_str(), "replace", nullptr};
            rc = mpv_command(mpv, cmd_plain);
        }
        if (rc < 0) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        bool started = false;
        bool loaded = false;
        bool restarted = false;

        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            if (nav_epoch.load(std::memory_order_relaxed) != job_epoch) {
                stop_and_wait_idle();
                return false;
            }
            mpv_event* ev = mpv_wait_event(mpv, 0.02);
            if (!ev) continue;

            if (!started) {
                if (ev->event_id == MPV_EVENT_START_FILE) {
                    started = true;
                    loaded = false;
                    restarted = false;
                    if (rctx) {
                        (void)mpv_render_context_update(rctx);
                    }
                }
                continue;
            }

            if (ev->event_id == MPV_EVENT_FILE_LOADED) {
                loaded = true;
            } else if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) {
                if (rctx) {
                    (void)mpv_render_context_update(rctx);
                }
                restarted = true;
            } else if (ev->event_id == MPV_EVENT_END_FILE) {
                auto* end_ev = static_cast<mpv_event_end_file*>(ev->data);
                if (!end_ev || end_ev->reason != MPV_END_FILE_REASON_EOF) {
                    return false;
                }
                return loaded;
            }

            if (loaded && restarted) {
                return true;
            }
        }
        return loaded && restarted;
    }

    bool seek_percent_in_place(
        const std::stop_token& stop,
        const std::atomic<uint64_t>& nav_epoch,
        uint64_t job_epoch)
    {
        drain_events();
        const char* seek_cmd[] = {"seek", "10", "absolute-percent", "keyframes", nullptr};
        if (mpv_command(mpv, seek_cmd) < 0) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            if (nav_epoch.load(std::memory_order_relaxed) != job_epoch) {
                return false;
            }
            mpv_event* ev = mpv_wait_event(mpv, 0.02);
            if (!ev) continue;
            if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) {
                if (rctx) {
                    (void)mpv_render_context_update(rctx);
                }
                return true;
            }
            if (ev->event_id == MPV_EVENT_END_FILE) {
                return false;
            }
        }
        return false;
    }

    bool render_current_frame(slint::SharedPixelBuffer<slint::Rgba8Pixel>& out_pixels) {
        if (rctx) {
            (void)mpv_render_context_update(rctx);
        }
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(kThumbnailWidth, kThumbnailHeight);
        int size[2] = {static_cast<int>(kThumbnailWidth), static_cast<int>(kThumbnailHeight)};
        char format[] = "rgb0";
        size_t stride = static_cast<size_t>(kThumbnailWidth) * 4;
        int block_for_target_time = 0;
        mpv_render_param rparams[] = {
            {MPV_RENDER_PARAM_SW_SIZE, size},
            {MPV_RENDER_PARAM_SW_FORMAT, format},
            {MPV_RENDER_PARAM_SW_STRIDE, &stride},
            {MPV_RENDER_PARAM_SW_POINTER, buffer.begin()},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_render(rctx, rparams) < 0) {
            return false;
        }
        auto* px = buffer.begin();
        const size_t total = static_cast<size_t>(kThumbnailWidth) * static_cast<size_t>(kThumbnailHeight);
        for (size_t i = 0; i < total; ++i) {
            px[i].a = 255;
        }
        out_pixels = std::move(buffer);
        return true;
    }

    void read_mpv_dimensions(uint32_t& out_width, uint32_t& out_height) const {
        if (out_width == 0) {
            int64_t vw = 0;
            if (mpv_get_property(mpv, "width", MPV_FORMAT_INT64, &vw) >= 0 && vw > 0) {
                out_width = static_cast<uint32_t>(vw);
            }
        }
        if (out_height == 0) {
            int64_t vh = 0;
            if (mpv_get_property(mpv, "height", MPV_FORMAT_INT64, &vh) >= 0 && vh > 0) {
                out_height = static_cast<uint32_t>(vh);
            }
        }
    }

    bool extract(
        const std::string& path,
        bool skip_probe_if_parsed,
        double known_duration,
        const std::stop_token& stop,
        const std::atomic<uint64_t>& nav_epoch,
        uint64_t job_epoch,
        double& out_duration_sec,
        uint32_t& out_width,
        uint32_t& out_height,
        slint::SharedPixelBuffer<slint::Rgba8Pixel>& out_pixels)
    {
        if (!ensure_init()) return false;

        // Fast single-pass path: container header probe already confirmed no embedded
        // cover exists and extracted duration/dimensions, so jump straight to 10%!
        if (skip_probe_if_parsed) {
            const char* opts = (known_duration > 0.0) ? "vid=1,start=10%" : "vid=1";
            bool ok = load_and_wait(path, opts, stop, nav_epoch, job_epoch);
            if (ok) {
                double dur = 0.0;
                if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur) >= 0 && dur > 0.0) {
                    out_duration_sec = dur;
                    if (known_duration <= 0.0) {
                        (void)seek_percent_in_place(stop, nav_epoch, job_epoch);
                    }
                }
                read_mpv_dimensions(out_width, out_height);
                ok = render_current_frame(out_pixels);
            }
            stop_and_wait_idle();
            return ok;
        }

        // Fallback path for containers without a fast header probe (.avi, .flv, .ts, .wmv)
        if (!load_and_wait(path, "vid=auto", stop, nav_epoch, job_epoch)) {
            stop_and_wait_idle();
            return false;
        }

        double dur = 0.0;
        if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur) >= 0 && dur > 0.0) {
            out_duration_sec = dur;
        }

        VideoTrackProbeResult track_probe;
        mpv_node track_list_node{};
        if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &track_list_node) >= 0) {
            track_probe = inspect_track_list_for_thumbnail(track_list_node);
            mpv_free_node_contents(&track_list_node);
        }

        if (track_probe.primary_width > 0 && track_probe.primary_height > 0) {
            out_width = track_probe.primary_width;
            out_height = track_probe.primary_height;
        } else {
            read_mpv_dimensions(out_width, out_height);
        }

        bool rendered = false;

        // Priority 1: Embedded thumbnail track in track-list
        if (track_probe.embedded_thumb_vid > 0) {
            bool thumb_ready = (track_probe.selected_vid == track_probe.embedded_thumb_vid);
            if (!thumb_ready) {
                const std::string vid_opt = std::format("vid={}", track_probe.embedded_thumb_vid);
                thumb_ready = load_and_wait(path, vid_opt.c_str(), stop, nav_epoch, job_epoch);
            }
            if (thumb_ready) {
                rendered = render_current_frame(out_pixels);
            }
        }

        // Priority 2 (Fallback): Seek to 10% in-place on primary video track (or reload if vid was switched)
        if (!rendered && !stop.stop_requested() &&
            nav_epoch.load(std::memory_order_relaxed) == job_epoch) {
            const int target_vid = track_probe.primary_video_vid > 0 ? track_probe.primary_video_vid : 1;
            const bool already_on_primary =
                (track_probe.embedded_thumb_vid <= 0) &&
                (track_probe.selected_vid == target_vid);

            if (already_on_primary) {
                if (out_duration_sec > 0.0) {
                    (void)seek_percent_in_place(stop, nav_epoch, job_epoch);
                }
                rendered = render_current_frame(out_pixels);
            } else {
                const std::string fallback_opts = (out_duration_sec > 0.0)
                    ? std::format("vid={},start=10%", target_vid)
                    : std::format("vid={}", target_vid);
                if (load_and_wait(path, fallback_opts.c_str(), stop, nav_epoch, job_epoch)) {
                    rendered = render_current_frame(out_pixels);
                }
            }
        }

        stop_and_wait_idle();
        return rendered;
    }
};

[[nodiscard]] bool run_ffmpeg_raw_rgba(
    char* const argv[],
    const std::stop_token& stop,
    const std::atomic<uint64_t>& nav_epoch,
    uint64_t job_epoch,
    slint::SharedPixelBuffer<slint::Rgba8Pixel>& out_pixels)
{
    int pipefd[2]{-1, -1};
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        return false;
    }
    const int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    pid_t pid = -1;
    const int spawn_res = posix_spawnp(&pid, "ffmpeg", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);

    if (spawn_res != 0 || pid <= 0) {
        close(pipefd[0]);
        return false;
    }

    slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(kThumbnailWidth, kThumbnailHeight);
    auto* dst = reinterpret_cast<uint8_t*>(buffer.begin());
    const size_t expected = static_cast<size_t>(kThumbnailWidth) * kThumbnailHeight * 4;
    size_t total = 0;
    bool aborted = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);

    while (total < expected) {
        if (stop.stop_requested() || nav_epoch.load(std::memory_order_relaxed) != job_epoch ||
            std::chrono::steady_clock::now() >= deadline) {
            aborted = true;
            break;
        }
        pollfd pfd{pipefd[0], POLLIN, 0};
        const int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        const ssize_t n = read(pipefd[0], dst + total, expected - total);
        if (n > 0) {
            total += static_cast<size_t>(n);
        } else if (n == 0) {
            break;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
    }
    close(pipefd[0]);

    if (aborted) {
        (void)kill(pid, SIGKILL);
    }

    int status = 0;
    pid_t wres = 0;
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (true) {
        wres = waitpid(pid, &status, WNOHANG);
        if (wres == pid || (wres < 0 && errno != EINTR)) {
            break;
        }
        if (std::chrono::steady_clock::now() >= wait_deadline) {
            (void)kill(pid, SIGKILL);
            do {
                wres = waitpid(pid, &status, 0);
            } while (wres < 0 && errno == EINTR);
            break;
        }
        poll(nullptr, 0, 10);
    }

    if (!aborted && total == expected && wres == pid &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        out_pixels = std::move(buffer);
        return true;
    }
    return false;
}

[[nodiscard]] bool extract_thumb_ffmpeg_fallback(
    const std::string& path,
    double duration_sec,
    const std::stop_token& stop,
    const std::atomic<uint64_t>& nav_epoch,
    uint64_t job_epoch,
    slint::SharedPixelBuffer<slint::Rgba8Pixel>& out_pixels)
{
    std::string path_copy = path;
    char arg_ffmpeg[] = "ffmpeg";
    char arg_nostdin[] = "-nostdin";
    char arg_v[] = "-v";
    char arg_error[] = "error";
    char arg_i[] = "-i";
    char arg_frames[] = "-frames:v";
    char arg_one[] = "1";
    char arg_vf[] = "-vf";
    char arg_scale[] = "scale=320:180:force_original_aspect_ratio=increase,crop=320:180";
    char arg_f[] = "-f";
    char arg_rawvideo[] = "rawvideo";
    char arg_pix_fmt[] = "-pix_fmt";
    char arg_rgba[] = "rgba";
    char arg_stdout[] = "-";

    // Seek to 10% of duration (or 1s, then frame 0 if duration is short/unknown)
    const double seek_sec = (duration_sec > 0.0) ? (duration_sec * 0.10) : 1.0;
    std::string seek_str = std::format("{:.2f}", seek_sec);
    char arg_ss[] = "-ss";
    char* argv_fallback[] = {
        arg_ffmpeg, arg_nostdin, arg_v, arg_error,
        arg_ss, seek_str.data(),
        arg_i, path_copy.data(),
        arg_frames, arg_one,
        arg_vf, arg_scale,
        arg_f, arg_rawvideo,
        arg_pix_fmt, arg_rgba,
        arg_stdout, nullptr
    };
    if (run_ffmpeg_raw_rgba(argv_fallback, stop, nav_epoch, job_epoch, out_pixels)) {
        return true;
    }
    char* argv_frame0[] = {
        arg_ffmpeg, arg_nostdin, arg_v, arg_error,
        arg_i, path_copy.data(),
        arg_frames, arg_one,
        arg_vf, arg_scale,
        arg_f, arg_rawvideo,
        arg_pix_fmt, arg_rgba,
        arg_stdout, nullptr
    };
    return run_ffmpeg_raw_rgba(argv_frame0, stop, nav_epoch, job_epoch, out_pixels);
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

VideoTrackProbeResult inspect_track_list_for_thumbnail(const mpv_node& root) noexcept {
    VideoTrackProbeResult res;
    if (root.format != MPV_FORMAT_NODE_ARRAY || !root.u.list) {
        return res;
    }

    const mpv_node_list* list = root.u.list;
    for (int i = 0; i < list->num; ++i) {
        const mpv_node& entry = list->values[i];
        if (entry.format != MPV_FORMAT_NODE_MAP || !entry.u.list) {
            continue;
        }

        const mpv_node_list* map = entry.u.list;
        int id = -1;
        bool is_video = false;
        bool is_albumart = false;
        bool is_image = false;
        bool is_selected = false;
        uint32_t w = 0;
        uint32_t h = 0;

        for (int k = 0; k < map->num; ++k) {
            if (!map->keys || !map->keys[k]) continue;
            std::string_view key(map->keys[k]);
            const mpv_node& val = map->values[k];

            if (key == "id" && val.format == MPV_FORMAT_INT64) {
                id = static_cast<int>(val.u.int64);
            } else if (key == "type" && val.format == MPV_FORMAT_STRING && val.u.string) {
                is_video = (std::string_view(val.u.string) == "video");
            } else if (key == "albumart" && val.format == MPV_FORMAT_FLAG) {
                is_albumart = (val.u.flag != 0);
            } else if (key == "image" && val.format == MPV_FORMAT_FLAG) {
                is_image = (val.u.flag != 0);
            } else if (key == "selected" && val.format == MPV_FORMAT_FLAG) {
                is_selected = (val.u.flag != 0);
            } else if (key == "demux-w" && val.format == MPV_FORMAT_INT64 && val.u.int64 > 0) {
                w = static_cast<uint32_t>(val.u.int64);
            } else if (key == "demux-h" && val.format == MPV_FORMAT_INT64 && val.u.int64 > 0) {
                h = static_cast<uint32_t>(val.u.int64);
            }
        }

        if (!is_video || id <= 0) {
            continue;
        }

        if (is_selected && res.selected_vid < 0) {
            res.selected_vid = id;
        }

        if (is_albumart || is_image) {
            if (res.embedded_thumb_vid < 0) {
                res.embedded_thumb_vid = id;
            }
        } else {
            if (res.primary_video_vid < 0) {
                res.primary_video_vid = id;
                res.primary_width = w;
                res.primary_height = h;
            }
        }
    }

    return res;
}

ContainerProbeResult probe_container_for_thumbnail(const std::filesystem::path& path) {
    std::error_code ec;
    const uint64_t fsz = std::filesystem::file_size(path, ec);
    if (ec || fsz < 16) {
        return {};
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }

    std::vector<uint8_t> hdr;
    if (!read_file_slice(ifs, 0, static_cast<size_t>(std::min<uint64_t>(fsz, 65536)), hdr) ||
        hdr.size() < 12) {
        return {};
    }

    // Matroska / WebM EBML header magic
    if (hdr[0] == 0x1A && hdr[1] == 0x45 && hdr[2] == 0xDF && hdr[3] == 0xA3) {
        return probe_matroska_container(ifs, hdr, fsz);
    }

    // ISOBMFF (.mp4, .m4v, .mov) starts with a box header (e.g. ftyp / free / wide / moov)
    const uint32_t box_type = read_be32(hdr.data() + 4);
    if (box_type == fourcc('f', 't', 'y', 'p') || box_type == fourcc('m', 'o', 'o', 'v') ||
        box_type == fourcc('f', 'r', 'e', 'e') || box_type == fourcc('w', 'i', 'd', 'e')) {
        return probe_isobmff_container(ifs, fsz);
    }

    return {};
}

slint::SharedPixelBuffer<slint::Rgba8Pixel> resize_cover_rgba8(
    const slint::SharedPixelBuffer<slint::Rgba8Pixel>& src,
    uint32_t dst_w,
    uint32_t dst_h)
{
    const uint32_t sw = src.width();
    const uint32_t sh = src.height();
    if (sw == 0 || sh == 0 || dst_w == 0 || dst_h == 0) {
        return {};
    }
    if (sw == dst_w && sh == dst_h) {
        return src;
    }

    slint::SharedPixelBuffer<slint::Rgba8Pixel> dst(dst_w, dst_h);
    const auto* spx = src.begin();
    auto* dpx = dst.begin();

    // Compute centered aspect-fill (cover) crop window inside source image
    const double src_aspect = static_cast<double>(sw) / static_cast<double>(sh);
    const double dst_aspect = static_cast<double>(dst_w) / static_cast<double>(dst_h);
    double crop_x = 0.0;
    double crop_y = 0.0;
    double crop_w = static_cast<double>(sw);
    double crop_h = static_cast<double>(sh);

    if (src_aspect > dst_aspect) {
        crop_w = static_cast<double>(sh) * dst_aspect;
        crop_x = (static_cast<double>(sw) - crop_w) * 0.5;
    } else if (src_aspect < dst_aspect) {
        crop_h = static_cast<double>(sw) / dst_aspect;
        crop_y = (static_cast<double>(sh) - crop_h) * 0.5;
    }

    const double scale_x = crop_w / static_cast<double>(dst_w);
    const double scale_y = crop_h / static_cast<double>(dst_h);

    if (scale_x >= 1.0 && scale_y >= 1.0) {
        // Area-averaging box downsample: crisp and free of aliasing
        for (uint32_t dy = 0; dy < dst_h; ++dy) {
            const uint32_t sy0 = static_cast<uint32_t>(std::clamp(crop_y + dy * scale_y, 0.0, static_cast<double>(sh - 1)));
            uint32_t sy1 = static_cast<uint32_t>(std::clamp(std::ceil(crop_y + (dy + 1) * scale_y), 1.0, static_cast<double>(sh)));
            if (sy1 <= sy0) sy1 = std::min(sh, sy0 + 1);
            sy1 = std::min(sy1, sh);

            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                const uint32_t sx0 = static_cast<uint32_t>(std::clamp(crop_x + dx * scale_x, 0.0, static_cast<double>(sw - 1)));
                uint32_t sx1 = static_cast<uint32_t>(std::clamp(std::ceil(crop_x + (dx + 1) * scale_x), 1.0, static_cast<double>(sw)));
                if (sx1 <= sx0) sx1 = std::min(sw, sx0 + 1);
                sx1 = std::min(sx1, sw);

                uint32_t sr = 0;
                uint32_t sg = 0;
                uint32_t sb = 0;
                uint32_t sa = 0;
                uint32_t count = 0;
                for (uint32_t sy = sy0; sy < sy1; ++sy) {
                    const size_t row_off = static_cast<size_t>(sy) * sw;
                    for (uint32_t sx = sx0; sx < sx1; ++sx) {
                        const auto& p = spx[row_off + sx];
                        sr += p.r;
                        sg += p.g;
                        sb += p.b;
                        sa += p.a;
                        ++count;
                    }
                }
                const uint32_t inv = count > 0 ? count : 1;
                dpx[static_cast<size_t>(dy) * dst_w + dx] = slint::Rgba8Pixel{
                    static_cast<uint8_t>(sr / inv),
                    static_cast<uint8_t>(sg / inv),
                    static_cast<uint8_t>(sb / inv),
                    static_cast<uint8_t>(sa / inv),
                };
            }
        }
    } else {
        // Bilinear upsample
        for (uint32_t dy = 0; dy < dst_h; ++dy) {
            const double fy = std::clamp(crop_y + (dy + 0.5) * scale_y - 0.5, 0.0, static_cast<double>(sh - 1));
            const uint32_t y0 = static_cast<uint32_t>(fy);
            const uint32_t y1 = std::min(y0 + 1, sh - 1);
            const double wy = fy - static_cast<double>(y0);

            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                const double fx = std::clamp(crop_x + (dx + 0.5) * scale_x - 0.5, 0.0, static_cast<double>(sw - 1));
                const uint32_t x0 = static_cast<uint32_t>(fx);
                const uint32_t x1 = std::min(x0 + 1, sw - 1);
                const double wx = fx - static_cast<double>(x0);

                const auto& p00 = spx[static_cast<size_t>(y0) * sw + x0];
                const auto& p10 = spx[static_cast<size_t>(y0) * sw + x1];
                const auto& p01 = spx[static_cast<size_t>(y1) * sw + x0];
                const auto& p11 = spx[static_cast<size_t>(y1) * sw + x1];

                auto lerp_ch = [&](uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11) -> uint8_t {
                    const double top = c00 * (1.0 - wx) + c10 * wx;
                    const double bot = c01 * (1.0 - wx) + c11 * wx;
                    const double v = top * (1.0 - wy) + bot * wy;
                    return static_cast<uint8_t>(std::clamp(v + 0.5, 0.0, 255.0));
                };

                dpx[static_cast<size_t>(dy) * dst_w + dx] = slint::Rgba8Pixel{
                    lerp_ch(p00.r, p10.r, p01.r, p11.r),
                    lerp_ch(p00.g, p10.g, p01.g, p11.g),
                    lerp_ch(p00.b, p10.b, p01.b, p11.b),
                    lerp_ch(p00.a, p10.a, p01.a, p11.a),
                };
            }
        }
    }

    return dst;
}

std::optional<slint::SharedPixelBuffer<slint::Rgba8Pixel>> decode_and_resize_cover_image(
    std::span<const uint8_t> encoded_bytes,
    std::string_view format_hint)
{
    if (encoded_bytes.empty() || encoded_bytes.size() > kMaxCoverPayloadBytes) {
        return std::nullopt;
    }
    std::string sniffed;
    if (format_hint.empty()) {
        sniffed = sniff_image_format(encoded_bytes);
        format_hint = sniffed;
    }
    auto img = slint::Image::load_from_data(encoded_bytes, format_hint);
    auto rgba = img.to_rgba8();
    if (!rgba.has_value() || rgba->width() == 0 || rgba->height() == 0) {
        return std::nullopt;
    }
    return resize_cover_rgba8(*rgba, kThumbnailWidth, kThumbnailHeight);
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
        hdr.height != kThumbnailHeight || hdr.format != 4) {
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
        const size_t hw = std::thread::hardware_concurrency();
        const size_t worker_count = std::clamp<size_t>(hw / 2, 2, 4);
        thumb_workers_.reserve(worker_count);
        for (size_t i = 0; i < worker_count; ++i) {
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

        // 3. Fast 3-tier extraction:
        //    Tier 1A: Zero-decoder EBML/ISOBMFF cover extraction + in-process image decode
        //    Tier 1B: Adjacent sidecar cover image (<stem>.{webp,jpg,png})
        //    Tier 2:  Single-pass libmpv 10% frame fallback (or ffmpeg fallback)
        if (!pixels.has_value()) {
            slint::SharedPixelBuffer<slint::Rgba8Pixel> gen_buf;
            uint32_t vw = 0;
            uint32_t vh = 0;
            double gen_dur = 0.0;
            bool ok = false;

            const ContainerProbeResult probe = probe_container_for_thumbnail(job->path);
            if (probe.duration_sec > 0.0) {
                gen_dur = probe.duration_sec;
                dur_sec = probe.duration_sec;
            }
            if (probe.width > 0 && probe.height > 0) {
                vw = probe.width;
                vh = probe.height;
            }

            if (probe.has_embedded_cover && !probe.cover_data.empty()) {
                if (auto cov = decode_and_resize_cover_image(probe.cover_data, probe.cover_format)) {
                    gen_buf = std::move(*cov);
                    ok = true;
                }
            }

            if (!ok && !stop.stop_requested() &&
                nav_epoch_.load(std::memory_order_relaxed) == job->epoch) {
                if (auto sidecar = try_load_sidecar_cover(job->path)) {
                    gen_buf = std::move(*sidecar);
                    ok = true;
                }
            }

            if (!ok && !stop.stop_requested() &&
                nav_epoch_.load(std::memory_order_relaxed) == job->epoch) {
                ok = extractor.extract(
                    job->path,
                    probe.parsed && !probe.has_embedded_cover,
                    dur_sec,
                    stop,
                    nav_epoch_,
                    job->epoch,
                    gen_dur,
                    vw,
                    vh,
                    gen_buf);
                if (gen_dur > 0.0) {
                    dur_sec = gen_dur;
                }
            }

            if (!ok && !stop.stop_requested() &&
                nav_epoch_.load(std::memory_order_relaxed) == job->epoch) {
                ok = extract_thumb_ffmpeg_fallback(
                    job->path, dur_sec, stop, nav_epoch_, job->epoch, gen_buf);
            }

            if (ok) {
                pixels = std::move(gen_buf);
                (void)write_mpxt_thumbnail_atomic(mpxt_path, *pixels);
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
