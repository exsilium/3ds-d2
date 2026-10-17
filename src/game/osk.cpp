// See osk.h.
#include "game/osk.h"

#include "game/controls.h"
#include "runtime/rt_host.h"

#include <cctype>

namespace game {
namespace {

enum Code : int { kBack = 1, kEnter = 2, kShift = 3, kClose = 4 };   // otherwise: the character

struct Key {
    int code;
    int row, col, width;   // in character cells; every key is 2 rows tall
    const char *label;
};

// Five rows of keys, 2 cells tall with a blank row between them, under a
// one-line title. Keys are 3 cells wide with a 1-cell gap: 10 per row.
constexpr int kTop = 2;
const Key kKeys[] = {
    {'1', 0, 0, 3, "1"}, {'2', 0, 4, 3, "2"}, {'3', 0, 8, 3, "3"}, {'4', 0, 12, 3, "4"}, {'5', 0, 16, 3, "5"},
    {'6', 0, 20, 3, "6"}, {'7', 0, 24, 3, "7"}, {'8', 0, 28, 3, "8"}, {'9', 0, 32, 3, "9"}, {'0', 0, 36, 3, "0"},
    {'q', 1, 0, 3, "q"}, {'w', 1, 4, 3, "w"}, {'e', 1, 8, 3, "e"}, {'r', 1, 12, 3, "r"}, {'t', 1, 16, 3, "t"},
    {'y', 1, 20, 3, "y"}, {'u', 1, 24, 3, "u"}, {'i', 1, 28, 3, "i"}, {'o', 1, 32, 3, "o"}, {'p', 1, 36, 3, "p"},
    {'a', 2, 0, 3, "a"}, {'s', 2, 4, 3, "s"}, {'d', 2, 8, 3, "d"}, {'f', 2, 12, 3, "f"}, {'g', 2, 16, 3, "g"},
    {'h', 2, 20, 3, "h"}, {'j', 2, 24, 3, "j"}, {'k', 2, 28, 3, "k"}, {'l', 2, 32, 3, "l"}, {kBack, 2, 36, 3, "<-"},
    {'z', 3, 0, 3, "z"}, {'x', 3, 4, 3, "x"}, {'c', 3, 8, 3, "c"}, {'v', 3, 12, 3, "v"}, {'b', 3, 16, 3, "b"},
    {'n', 3, 20, 3, "n"}, {'m', 3, 24, 3, "m"}, {'-', 3, 28, 3, "-"}, {'_', 3, 32, 3, "_"}, {'.', 3, 36, 3, "."},
    {kShift, 4, 0, 7, "Shift"}, {' ', 4, 8, 15, "Space"}, {kEnter, 4, 24, 7, "OK"}, {kClose, 4, 32, 7, "Close"},
};

int key_row(const Key &k) { return kTop + k.row * 3; }

}  // namespace

void Osk::open()
{
    flash_ = -1;
    open_ = true;
    shift_ = false;
    dirty_ = true;
}

void Osk::press(int code)
{
    if (code >= 0x20)
        rt::logf("keyboard: '%c'", shift_ ? std::toupper(code) : code);
    else
        rt::logf("keyboard: %s", code == kBack ? "backspace" : code == kEnter ? "OK" : code == kShift ? "shift" : "close");
    switch (code) {
    case kBack:
        rt::input_key(0x08, true);   // VK_BACK: its WM_CHAR 0x08 deletes in the edit box
        rt::input_key(0x08, false);
        break;
    case kEnter:
        rt::input_key(0x0D, true);   // VK_RETURN (with WM_CHAR 0x0D): confirms the field
        rt::input_key(0x0D, false);
        open_ = false;
        break;
    case kShift:
        shift_ = !shift_;
        break;
    case kClose:
        open_ = false;
        break;
    default:
        rt::input_char((uint32_t)(shift_ ? std::toupper(code) : code));
        if (shift_ && std::isalpha(code))
            shift_ = false;   // one capital, like a phone keyboard
        break;
    }
    dirty_ = true;
}

bool Osk::tap(int col, int row)
{
    // Every cell of a key row belongs to a key: a key's hit area also covers
    // the gap column to its right and the blank row under it, so a tap that
    // lands between keys still types the nearer one.
    for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++) {
        const Key &k = kKeys[i];
        const int r = key_row(k);
        if (row >= r && row < r + 3 && col >= k.col && col <= k.col + k.width) {
            flash_ = (int)i;
            flash_until_ms_ = now_ms_ + 150;
            press(k.code);
            return true;
        }
    }
    return false;
}

void Osk::tick(uint64_t now_ms)
{
    now_ms_ = now_ms;
    if (flash_ >= 0 && now_ms >= flash_until_ms_) {
        flash_ = -1;
        dirty_ = true;
    }
}

void Osk::buttons(uint32_t pressed)
{
    if (pressed & kB)
        press(kBack);
    if (pressed & kY)
        press(kShift);
    if (pressed & kStart)
        press(kEnter);
    if (pressed & kSelect)
        press(kClose);
}

std::vector<Osk::Row> Osk::rows() const
{
    std::vector<Row> out(kRows, Row{std::string(kCols, ' '), std::vector<bool>(kCols, false)});
    const std::string title = shift_ ? "Keyboard  [SHIFT]  B:del Y:shift Start:OK" : "Keyboard  B:del Y:shift Start:OK Sel:close";
    out[0].text.replace(0, title.size() < (size_t)kCols ? title.size() : kCols, title, 0, kCols);
    for (const Key &k : kKeys) {
        const int r = key_row(k);
        std::string label = k.label;
        if (shift_ && label.size() == 1 && std::isalpha((unsigned char)label[0]))
            label[0] = (char)std::toupper((unsigned char)label[0]);
        const bool flashed = &k - kKeys == flash_;   // just pressed: drawn inverted for a moment
        for (int dr = 0; dr < 2; dr++)
            for (int c = 0; c < k.width; c++)
                out[r + dr].key[k.col + c] = !flashed;
        const int pad = (k.width - (int)label.size()) / 2;
        out[r].text.replace(k.col + (pad > 0 ? pad : 0), label.size(), label);
    }
    return out;
}

}  // namespace game
