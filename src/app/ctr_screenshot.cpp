// Screenshots into the console's Camera album (L+R+Select), as Animal
// Crossing: New Leaf takes them with L+R. The album lists JPEGs stored under
// sdmc:/DCIM/<NNN>NIN03/HNI_<NNNN>.JPG, so a picture written there with the
// usual name shows up next to the camera's own photos; no system service is
// involved (a retail game writes through a write-only SD archive, homebrew
// writes the file).
//
// The picture is the game's own frame, not the 400x240 screen: in game that
// is 640x480, the camera's own photo size; larger frames (menus, 800x600) are
// scaled down to fit 640x480, and cinematics keep their aspect ratio. Every
// output pixel averages a grid of point samples over its footprint (one at
// 1:1, so in-game shots are exact).
//
// The JPEG is baseline, with an EXIF block in front (like the camera's): the
// date the album sorts by, and a 160x120 thumbnail for its grid.
//
// Numbering: the next number after the highest HNI_ file in the highest
// NIN03 folder; a full folder (100 pictures, as the camera keeps them) starts
// the next folder at HNI_0001. The file is written under a temporary name and
// renamed, so the album never sees half a picture.
//
// Each screenshot is a pair (issue #61): with the frame, the bottom screen as
// shown, 320x240, into the game's own folder (kShotDir, not the album), and
// both in the game's list of pairs (game/report.h), from which a problem
// report can attach them. The list keeps the newest 16; the bottom pictures
// of older pairs are removed with them (the album's are the player's).
//
// The report's side: any of these pictures, or Luma3DS's BMP screenshots,
// read back as RGB (scaled down to fit, for its preview) and made a JPEG.
#include "game/report.h"

#include <3ds.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <setjmp.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

#include <jpeglib.h>

namespace shot {

struct Source {
    const uint8_t *pixels;      // indices (palette), or a cinematic's Y, U, V planes one after the other
    uint32_t width, height;
    const uint32_t *palette;    // 0x00RRGGBB; indexed frames only
    bool yuv;
    uint32_t display_height;    // cinematics: the height the picture is meant to have
    const uint16_t *fb565 = nullptr;   // instead: the bottom screen's framebuffer (RGB565, 240x320 rotated)
    const uint8_t *rgb = nullptr;      // instead: RGB triplets, rows top first
};

// A picture read back for a report: RGB triplets, rows top first.
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgb;
};

bool save(const Source &src, const uint16_t *bottom, std::string &msg);
bool load(const std::string &path, uint32_t max_w, uint32_t max_h, Image &out, std::string &msg);
bool to_jpeg(const Image &img, std::string &out, std::string &msg);

namespace {

constexpr int kMaxW = 640, kMaxH = 480;          // the camera's photo size
constexpr int kThumbW = 160, kThumbH = 120;      // the EXIF thumbnail (DCF)
constexpr int kQuality = 92, kThumbQuality = 80;
constexpr int kPerFolder = 100;
const char kDcim[] = "sdmc:/DCIM";
const char kShotDir[] = "sdmc:/3ds/3ds-d2/screenshots";
const char kShotList[] = "sdmc:/3ds/3ds-d2/screenshots/pairs.txt";

// ---- sampling ---------------------------------------------------------------------

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// The RGB of source pixel (x, y).
void rgb_at(const Source &s, uint32_t x, uint32_t y, int &r, int &g, int &b)
{
    if (s.fb565) {   // column x, bottom to top
        const uint16_t p = s.fb565[(size_t)x * 240 + (239 - y)];
        r = ((p >> 11) & 0x1F) * 255 / 31;
        g = ((p >> 5) & 0x3F) * 255 / 63;
        b = (p & 0x1F) * 255 / 31;
        return;
    }
    if (s.rgb) {
        const uint8_t *p = s.rgb + ((size_t)y * s.width + x) * 3;
        r = p[0];
        g = p[1];
        b = p[2];
        return;
    }
    if (!s.yuv) {
        const uint32_t p = s.palette[s.pixels[(size_t)y * s.width + x]];
        r = (p >> 16) & 0xFF;
        g = (p >> 8) & 0xFF;
        b = p & 0xFF;
        return;
    }
    // BT.601, limited range (as draw_yuv shows it).
    const uint32_t cw = (s.width + 1) / 2, ch = (s.height + 1) / 2;
    const uint8_t *yp = s.pixels, *up = yp + (size_t)s.width * s.height, *vp = up + (size_t)cw * ch;
    const size_t c = (size_t)(y >> 1) * cw + (x >> 1);
    const int yy = 298 * (yp[(size_t)y * s.width + x] - 16) + 128, u = up[c] - 128, v = vp[c] - 128;
    r = clamp8((yy + 409 * v) >> 8);
    g = clamp8((yy - 100 * u - 208 * v) >> 8);
    b = clamp8((yy + 516 * u) >> 8);
}

// Row `oy` of the picture scaled to ow x oh, as RGB triplets.
void scaled_row(const Source &s, int ow, int oh, int oy, uint8_t *out)
{
    // n x n samples per output pixel, n = the scale step rounded up.
    const int nx = ((int)s.width + ow - 1) / ow, ny = ((int)s.height + oh - 1) / oh;
    for (int ox = 0; ox < ow; ox++) {
        int r = 0, g = 0, b = 0;
        for (int j = 0; j < ny; j++) {
            // Sample centres spread evenly over the pixel's footprint.
            const uint32_t sy = (uint32_t)(((2 * oy * ny + 2 * j + 1) * (int64_t)s.height) / (2 * oh * ny));
            for (int i = 0; i < nx; i++) {
                const uint32_t sx = (uint32_t)(((2 * ox * nx + 2 * i + 1) * (int64_t)s.width) / (2 * ow * nx));
                int pr, pg, pb;
                rgb_at(s, sx, sy, pr, pg, pb);
                r += pr;
                g += pg;
                b += pb;
            }
        }
        const int n = nx * ny;
        out[3 * ox] = (uint8_t)((r + n / 2) / n);
        out[3 * ox + 1] = (uint8_t)((g + n / 2) / n);
        out[3 * ox + 2] = (uint8_t)((b + n / 2) / n);
    }
}

// The largest size of the picture's aspect ratio inside max_w x max_h (never larger than the source).
void fit(const Source &s, int max_w, int max_h, int &w, int &h)
{
    const int dw = (int)s.width, dh = (int)(s.display_height ? s.display_height : s.height);
    w = dw;
    h = dh;
    if (w > max_w) {
        h = (h * max_w + w / 2) / w;
        w = max_w;
    }
    if (h > max_h) {
        w = (w * max_h + h / 2) / h;
        h = max_h;
    }
    w = w < 1 ? 1 : w;
    h = h < 1 ? 1 : h;
}

// ---- JPEG -------------------------------------------------------------------------

struct JpegError {
    jpeg_error_mgr mgr;
    jmp_buf jump;
    char text[JMSG_LENGTH_MAX];
};

void jpeg_fail(j_common_ptr c)
{
    JpegError *e = reinterpret_cast<JpegError *>(c->err);
    e->mgr.format_message(c, e->text);
    longjmp(e->jump, 1);
}

// Compresses the picture at ow x oh into `out` (thumbnail: no markers) or
// into `file` (the photo: `app1` written first, as an EXIF file has it).
bool encode(const Source &s, int ow, int oh, int quality, FILE *file, const std::vector<uint8_t> *app1,
            std::vector<uint8_t> *out, std::string &msg)
{
    jpeg_compress_struct c;
    JpegError err;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    std::vector<uint8_t> row((size_t)ow * 3);
    unsigned char *mem = nullptr;
    unsigned long mem_size = 0;
    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&c);
        std::free(mem);
        msg = err.text;
        return false;
    }
    jpeg_create_compress(&c);
    if (file)
        jpeg_stdio_dest(&c, file);
    else
        jpeg_mem_dest(&c, &mem, &mem_size);
    c.image_width = (JDIMENSION)ow;
    c.image_height = (JDIMENSION)oh;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    c.write_JFIF_header = file && !app1;   // a file without EXIF in its place is a plain JFIF
    jpeg_start_compress(&c, TRUE);
    if (app1)
        jpeg_write_marker(&c, JPEG_APP0 + 1, app1->data(), (unsigned)app1->size());
    while (c.next_scanline < c.image_height) {
        scaled_row(s, ow, oh, (int)c.next_scanline, row.data());
        JSAMPROW rows[1] = {row.data()};
        jpeg_write_scanlines(&c, rows, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    if (out)
        out->assign(mem, mem + mem_size);
    std::free(mem);
    return true;
}

// ---- EXIF -------------------------------------------------------------------------
//
// A little-endian TIFF structure: IFD0 (camera, date, pointer to the Exif
// IFD), the Exif IFD (version, original date, pixel size), IFD1 (the
// thumbnail's offset and length), then the thumbnail.

enum : uint16_t { kAscii = 2, kShort = 3, kLong = 4, kRational = 5, kUndefined = 7 };

struct Entry {
    uint16_t tag, type;
    uint32_t count;
    std::vector<uint8_t> data;   // little-endian value(s)
};

struct Ifd {
    std::vector<Entry> entries;
    size_t size() const
    {
        size_t n = 2 + 12 * entries.size() + 4;
        for (const Entry &e : entries)
            if (e.data.size() > 4)
                n += (e.data.size() + 1) & ~(size_t)1;
        return n;
    }
};

void put16(std::vector<uint8_t> &v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void put32(std::vector<uint8_t> &v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }

Entry ascii(uint16_t tag, const char *s)
{
    Entry e{tag, kAscii, (uint32_t)std::strlen(s) + 1, {}};
    e.data.assign(s, s + e.count);
    return e;
}
Entry short_(uint16_t tag, uint16_t x) { Entry e{tag, kShort, 1, {}}; put16(e.data, x); return e; }
Entry long_(uint16_t tag, uint32_t x) { Entry e{tag, kLong, 1, {}}; put32(e.data, x); return e; }
Entry rational(uint16_t tag, uint32_t n, uint32_t d) { Entry e{tag, kRational, 1, {}}; put32(e.data, n); put32(e.data, d); return e; }
Entry undefined4(uint16_t tag, const char *s) { Entry e{tag, kUndefined, 4, {}}; e.data.assign(s, s + 4); return e; }

// Writes `ifd` at the end of `tiff`; `next` is the next IFD's offset. Tags in
// ascending order, as TIFF requires.
void write_ifd(std::vector<uint8_t> &tiff, const Ifd &ifd, uint32_t next)
{
    const size_t at = tiff.size();
    size_t extra = at + 2 + 12 * ifd.entries.size() + 4;   // where values over 4 bytes go
    std::vector<uint8_t> overflow;
    put16(tiff, (uint32_t)ifd.entries.size());
    for (const Entry &e : ifd.entries) {
        put16(tiff, e.tag);
        put16(tiff, e.type);
        put32(tiff, e.count);
        if (e.data.size() <= 4) {
            for (size_t i = 0; i < 4; i++)
                tiff.push_back(i < e.data.size() ? e.data[i] : 0);
        } else {
            put32(tiff, (uint32_t)(extra + overflow.size()));
            overflow.insert(overflow.end(), e.data.begin(), e.data.end());
            if (overflow.size() & 1)
                overflow.push_back(0);
        }
    }
    put32(tiff, next);
    tiff.insert(tiff.end(), overflow.begin(), overflow.end());
}

std::vector<uint8_t> exif(int w, int h, const std::vector<uint8_t> &thumb)
{
    // The console's clock is local time with no time zone: break it down as is.
    char date[20];
    const time_t now = std::time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    std::strftime(date, sizeof date, "%Y:%m:%d %H:%M:%S", &t);

    Ifd ifd0, sub, ifd1;
    ifd0.entries = {ascii(0x010F, "Nintendo"), ascii(0x0110, "Nintendo 3DS"), short_(0x0112, 1),
                    rational(0x011A, 72, 1), rational(0x011B, 72, 1), short_(0x0128, 2),
                    ascii(0x0131, "3ds-d2"), ascii(0x0132, date), short_(0x0213, 2), long_(0x8769, 0)};
    sub.entries = {undefined4(0x9000, "0220"), ascii(0x9003, date), ascii(0x9004, date),
                   undefined4(0x9101, "\x01\x02\x03\x00"),
                   undefined4(0xA000, "0100"), short_(0xA001, 1), long_(0xA002, (uint32_t)w),
                   long_(0xA003, (uint32_t)h)};
    ifd1.entries = {short_(0x0103, 6), rational(0x011A, 72, 1), rational(0x011B, 72, 1), short_(0x0128, 2),
                    long_(0x0201, 0), long_(0x0202, (uint32_t)thumb.size())};
    const uint32_t off0 = 8, off_sub = off0 + (uint32_t)ifd0.size(), off1 = off_sub + (uint32_t)sub.size(),
                   off_thumb = off1 + (uint32_t)ifd1.size();
    ifd0.entries.back() = long_(0x8769, off_sub);
    ifd1.entries[4] = long_(0x0201, off_thumb);

    std::vector<uint8_t> app1 = {'E', 'x', 'i', 'f', 0, 0};
    std::vector<uint8_t> tiff = {'I', 'I', 0x2A, 0};
    put32(tiff, off0);
    write_ifd(tiff, ifd0, off1);
    write_ifd(tiff, sub, 0);
    write_ifd(tiff, ifd1, 0);
    tiff.insert(tiff.end(), thumb.begin(), thumb.end());
    app1.insert(app1.end(), tiff.begin(), tiff.end());
    return app1;
}

// ---- where it goes ----------------------------------------------------------------

// "NNN" + suffix with NNN in 100..999 (case-insensitive suffix): NNN, else -1.
int folder_number(const char *name)
{
    if (std::strlen(name) != 8 || strncasecmp(name + 3, "NIN03", 5) != 0)
        return -1;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (name[i] < '0' || name[i] > '9')
            return -1;
        n = n * 10 + (name[i] - '0');
    }
    return n >= 100 ? n : -1;
}

// "HNI_NNNN.JPG" or ".MPO" (the camera's 3D photos): NNNN, else -1.
int file_number(const char *name)
{
    if (std::strlen(name) != 12 || strncasecmp(name, "HNI_", 4) != 0 ||
        (strcasecmp(name + 8, ".JPG") != 0 && strcasecmp(name + 8, ".MPO") != 0))
        return -1;
    int n = 0;
    for (int i = 4; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9')
            return -1;
        n = n * 10 + (name[i] - '0');
    }
    return n;
}

bool next_path(std::string &dir, std::string &path)
{
    mkdir(kDcim, 0777);
    int folder = 100;
    if (DIR *d = opendir(kDcim)) {
        while (dirent *e = readdir(d)) {
            const int n = folder_number(e->d_name);
            if (n > folder)
                folder = n;
        }
        closedir(d);
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s/%dNIN03", kDcim, folder);
    int last = 0, count = 0;
    if (DIR *d = opendir(buf)) {
        while (dirent *e = readdir(d)) {
            const int n = file_number(e->d_name);
            if (n >= 0) {
                count++;
                if (n > last)
                    last = n;
            }
        }
        closedir(d);
    }
    if (count >= kPerFolder || last >= 9999) {
        if (folder >= 999)
            return false;
        std::snprintf(buf, sizeof buf, "%s/%dNIN03", kDcim, ++folder);
        last = 0;
    }
    dir = buf;
    mkdir(dir.c_str(), 0777);
    std::snprintf(buf, sizeof buf, "/HNI_%04d.JPG", last + 1);
    path = dir + buf;
    return true;
}

// Writes the picture at w x h to `path` as a JPEG (EXIF `app1` first, if
// given), under a temporary name renamed at the end.
bool write_jpeg(const Source &src, int w, int h, const std::vector<uint8_t> *app1, const std::string &path,
                const std::string &tmp, std::string &msg)
{
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        msg = "cannot write " + tmp;
        return false;
    }
    const bool ok = encode(src, w, h, kQuality, f, app1, nullptr, msg);
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        if (ok)
            msg = "cannot write " + path;
        return false;
    }
    return true;
}

bool read_file(const std::string &path, std::string &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[4096];
    out.clear();
    while (size_t n = std::fread(buf, 1, sizeof buf, f))
        out.append(buf, n);
    std::fclose(f);
    return true;
}

// The bottom screen as shown (if `fb`), beside the frame: its own JPEG, and
// the pair in the game's list. False (the frame is saved all the same) if
// either could not be written.
bool save_bottom(const uint16_t *fb, const std::string &top, std::string &msg)
{
    // Named by the time, as Luma3DS names its own: 2026-11-02_14-02-31.123_bot.jpg.
    const uint64_t now_ms = osGetTime();   // ms since 1900, local time
    game::report::ShotPair p;
    p.time = now_ms / 1000 - 2208988800ull;
    p.ms = (uint32_t)(now_ms % 1000);
    std::string name = game::report::shot_time_text(p.time);
    name[10] = '_';
    name[13] = name[16] = '-';
    char ms[8];
    std::snprintf(ms, sizeof ms, ".%03u", (unsigned)p.ms);
    mkdir(kShotDir, 0777);
    p.top = top;
    bool ok = true;
    if (fb) {
        p.bot = std::string(kShotDir) + "/" + name + ms + "_bot.jpg";
        Source src{};
        src.width = 320;
        src.height = 240;
        src.fb565 = fb;
        if (!write_jpeg(src, 320, 240, nullptr, p.bot, std::string(kShotDir) + "/bot.tmp", msg)) {
            p.bot.clear();
            ok = false;
        }
    }
    std::string text;
    read_file(kShotList, text);
    std::vector<game::report::ShotPair> list = game::report::parse_shot_list(text);
    for (const game::report::ShotPair &old : game::report::add_shot(list, p))
        if (!old.bot.empty())
            std::remove(old.bot.c_str());
    text = game::report::shot_list_text(list);
    FILE *f = std::fopen(kShotList, "wb");
    if (!f || std::fwrite(text.data(), 1, text.size(), f) != text.size()) {
        if (f)
            std::fclose(f);
        msg = "cannot write the list of screenshots";
        return false;
    }
    std::fclose(f);
    return ok;
}

// The decompressor's errors, as the compressor's.
bool load_jpeg(const std::string &data, uint32_t max_w, uint32_t max_h, Image &out, std::string &msg)
{
    jpeg_decompress_struct d;
    JpegError err;
    d.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&d);
        msg = err.text;
        return false;
    }
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, (const unsigned char *)data.data(), (unsigned long)data.size());
    jpeg_read_header(&d, TRUE);
    d.out_color_space = JCS_RGB;
    // libjpeg's own scaling (1/2, 1/4, 1/8) to the largest that fits: the
    // game's 640x480 frame is the bottom screen's 320x240 at 1/2.
    d.scale_num = 1;
    d.scale_denom = 1;
    while (max_w && max_h && d.scale_denom < 8 &&
           (d.image_width / d.scale_denom > max_w || d.image_height / d.scale_denom > max_h))
        d.scale_denom *= 2;
    jpeg_start_decompress(&d);
    out.width = d.output_width;
    out.height = d.output_height;
    out.rgb.resize((size_t)out.width * out.height * 3);
    while (d.output_scanline < d.output_height) {
        JSAMPROW row = out.rgb.data() + (size_t)d.output_scanline * out.width * 3;
        jpeg_read_scanlines(&d, &row, 1);
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    return true;
}

}  // namespace

bool load(const std::string &path, uint32_t max_w, uint32_t max_h, Image &out, std::string &msg)
{
    std::string data;
    if (!read_file(path, data)) {
        msg = "cannot read " + path;
        return false;
    }
    if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M') {
        if (!game::report::decode_bmp(data, out.width, out.height, out.rgb)) {
            msg = "not a 24-bit BMP: " + path;
            return false;
        }
    } else if (!load_jpeg(data, max_w, max_h, out, msg)) {
        return false;
    }
    if (!max_w || !max_h || (out.width <= max_w && out.height <= max_h))
        return true;
    // Still too large (a BMP, or past 1/8): averaged down to fit.
    Source src{};
    src.width = out.width;
    src.height = out.height;
    src.rgb = out.rgb.data();
    int w, h;
    fit(src, (int)max_w, (int)max_h, w, h);
    Image small;
    small.width = (uint32_t)w;
    small.height = (uint32_t)h;
    small.rgb.resize((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        scaled_row(src, w, h, y, small.rgb.data() + (size_t)y * w * 3);
    out = std::move(small);
    return true;
}

bool to_jpeg(const Image &img, std::string &out, std::string &msg)
{
    Source src{};
    src.width = img.width;
    src.height = img.height;
    src.rgb = img.rgb.data();
    std::vector<uint8_t> bytes;
    if (!encode(src, (int)img.width, (int)img.height, kQuality, nullptr, nullptr, &bytes, msg))
        return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool save(const Source &src, const uint16_t *bottom, std::string &msg)
{
    if (!src.pixels || !src.width || !src.height || (!src.yuv && !src.palette)) {
        msg = "no frame";
        return false;
    }
    int w, h, tw, th;
    fit(src, kMaxW, kMaxH, w, h);
    fit(src, kThumbW, kThumbH, tw, th);
    std::vector<uint8_t> thumb;
    if (!encode(src, tw, th, kThumbQuality, nullptr, nullptr, &thumb, msg))
        return false;
    const std::vector<uint8_t> app1 = exif(w, h, thumb);

    std::string dir, path;
    if (!next_path(dir, path)) {
        msg = "DCIM is full";
        return false;
    }
    if (!write_jpeg(src, w, h, &app1, path, dir + "/HNI_TEMP.TMP", msg))
        return false;
    char info[32];
    std::snprintf(info, sizeof info, " (%dx%d)", w, h);
    std::string why;
    if (!save_bottom(bottom, path, why))
        msg = path + info + ", but " + why;
    else
        msg = path + info + (bottom ? " and the bottom screen" : ", no bottom screen");
    return true;
}

}  // namespace shot
