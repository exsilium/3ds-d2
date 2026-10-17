// Minimal PNG writer for the test lanes: 8-bit RGB, zlib "stored" blocks (no
// compression), so it needs no library. Screenshots only.
#ifndef D2_TESTS_PNG_H
#define D2_TESTS_PNG_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace png {

inline uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline void put32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

inline void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
    put32(out, (uint32_t)data.size());
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()));
}

// rgb: width*height*3 bytes, top row first.
inline bool write_rgb(const std::string &path, const uint8_t *rgb, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(width * 3 + 1) * height);
    for (uint32_t y = 0; y < height; y++) {
        raw.push_back(0);   // filter: none
        raw.insert(raw.end(), rgb + (size_t)y * width * 3, rgb + (size_t)(y + 1) * width * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t x : raw) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
        const size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (long)off, raw.begin() + (long)(off + n));
        if (!n)
            break;
    }
    put32(z, (b << 16) | a);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});   // 8-bit, RGB
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace png

#endif
