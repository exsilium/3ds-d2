// Constant tables of the Bink 1 format (bink_tables.cpp).
#ifndef D2_GAME_BINK_BINK_TABLES_H
#define D2_GAME_BINK_BINK_TABLES_H

#include <cstdint>

namespace bink {

extern const uint8_t kScan[64];               // coefficient order of DCT and residue blocks
extern const uint8_t kTreeCodes[16][16];      // the 16 Huffman code sets (LSB-first codes)
extern const uint8_t kTreeLengths[16][16];    // ... and their code lengths (at most 7)
extern const uint8_t kRunPatterns[16][64];    // pixel orders of run blocks
extern const uint32_t kIntraQuant[16][64];    // dequantisation, by level and scan position
extern const uint32_t kInterQuant[16][64];

}  // namespace bink

#endif
