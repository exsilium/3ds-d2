// MPQ lookup (see mpq.h). The archive's hash table maps a name, through
// three hashes of it, to an entry of its block table: position, sizes and
// flags. Both tables are encrypted with keys derived from fixed names.
#include "game/mpq.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace game {
namespace {

uint32_t g_crypt[0x500];
bool g_crypt_ready = false;

void init_crypt()
{
    uint32_t seed = 0x00100001;
    for (int i = 0; i < 0x100; i++)
        for (int j = i; j < 0x500; j += 0x100) {
            seed = (seed * 125 + 3) % 0x2AAAAB;
            const uint32_t hi = (seed & 0xFFFF) << 16;
            seed = (seed * 125 + 3) % 0x2AAAAB;
            g_crypt[j] = hi | (seed & 0xFFFF);
        }
    g_crypt_ready = true;
}

// kind 0: hash table index, 1 and 2: the two check values, 3: key.
uint32_t hash(const std::string &s, int kind)
{
    uint32_t seed1 = 0x7FED7FED, seed2 = 0xEEEEEEEE;
    for (unsigned char ch : s) {
        ch = ch == '/' ? '\\' : (unsigned char)std::toupper(ch);
        seed1 = g_crypt[(kind << 8) + ch] ^ (seed1 + seed2);
        seed2 = ch + seed1 + seed2 + (seed2 << 5) + 3;
    }
    return seed1;
}

void decrypt(uint32_t *words, size_t n, uint32_t key)
{
    uint32_t seed = 0xEEEEEEEE;
    for (size_t i = 0; i < n; i++) {
        seed += g_crypt[0x400 + (key & 0xFF)];
        const uint32_t v = words[i] ^ (key + seed);
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        seed = v + seed + (seed << 5) + 3;
        words[i] = v;
    }
}

bool read_table(FILE *f, uint64_t pos, uint32_t entries, const char *key_name, std::vector<uint32_t> &out)
{
    out.resize((size_t)entries * 4);
    if (std::fseek(f, (long)pos, SEEK_SET) != 0 || std::fread(out.data(), 16, entries, f) != entries)
        return false;
    decrypt(out.data(), out.size(), hash(key_name, 3));
    return true;
}

}  // namespace

bool mpq_find_stored(const std::string &archive, const std::string &name, uint64_t &offset, uint32_t &size)
{
    if (!g_crypt_ready)
        init_crypt();
    FILE *f = std::fopen(archive.c_str(), "rb");
    if (!f)
        return false;
    bool found = false;
    uint32_t h[8];   // magic, header size, archive size, version | sector shift, hash pos, block pos, counts
    if (std::fread(h, 4, 8, f) == 8 && h[0] == 0x1A51504D /* "MPQ\x1A" */ && h[6] && (h[6] & (h[6] - 1)) == 0) {
        std::vector<uint32_t> hashes, blocks;
        if (read_table(f, h[4], h[6], "(hash table)", hashes) && read_table(f, h[5], h[7], "(block table)", blocks)) {
            const uint32_t n = h[6], a = hash(name, 1), b = hash(name, 2);
            for (uint32_t i = hash(name, 0) & (n - 1), tries = 0; tries < n; i = (i + 1) & (n - 1), tries++) {
                const uint32_t *e = &hashes[(size_t)i * 4];   // check a, check b, locale | platform, block
                if (e[3] == 0xFFFFFFFF)
                    break;   // end of the chain
                if (e[0] != a || e[1] != b || e[3] >= h[7])
                    continue;
                const uint32_t *blk = &blocks[(size_t)e[3] * 4];   // position, packed size, size, flags
                const uint32_t flags = blk[3];
                // Exists, and neither imploded, compressed nor encrypted.
                if ((flags & 0x80000000u) && !(flags & 0x00010300u) && blk[1] == blk[2]) {
                    offset = blk[0];
                    size = blk[2];
                    found = true;
                }
                break;
            }
        }
    }
    std::fclose(f);
    return found;
}

}  // namespace game
