// Finds a file stored in an MPQ archive without compression or encryption,
// as Diablo II stores its cinematics, so it can be read in place: the
// result is the file's byte range within the archive.
#ifndef D2_GAME_MPQ_H
#define D2_GAME_MPQ_H

#include <cstdint>
#include <string>

namespace game {

// `name` as the game spells it ("data\\local\\video\\..."); case and slash
// direction do not matter. False if the archive has no such file, or holds
// it compressed or encrypted.
bool mpq_find_stored(const std::string &archive, const std::string &name, uint64_t &offset, uint32_t &size);

}  // namespace game

#endif
