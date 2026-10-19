// PKWARE Data Compression Library "explode": the decompressor of the MPQs'
// imploded files. A port of StormLib's src/pklib/explode.c (Ladislav Zezula,
// MIT licence), with the input and output in memory instead of callbacks.
#ifndef D2_GAME_PKLIB_EXPLODE_H
#define D2_GAME_PKLIB_EXPLODE_H

#include <cstdint>

namespace game {

// Decompresses `in` (in_len bytes) into `out`, at most out_max bytes, and
// returns the bytes written. As Storm's wrapper does, input past in_len reads
// as missing and output past out_max is dropped; a bad stream ends with what
// was written so far.
uint32_t pk_explode(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_max);

}  // namespace game

#endif
