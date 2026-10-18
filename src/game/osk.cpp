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
    int row, unit, units;   // key row 0-4; position and width in key units (10 per row)
    const char *label;
};

// Five rows of keys under a one-line title. A key unit is a 32-pixel cell
// holding a 29-pixel key; rows are 25 pixels apart with 22-pixel keys.
constexpr int kUnit = 32, kKeyW = kUnit - 3, kMargin = 2;
constexpr int kRowPitch = 25, kKeyH = kRowPitch - 3, kKeysTop = 14;
const Key kKeys[] = {
    {'1', 0, 0, 1, "1"}, {'2', 0, 1, 1, "2"}, {'3', 0, 2, 1, "3"}, {'4', 0, 3, 1, "4"}, {'5', 0, 4, 1, "5"},
    {'6', 0, 5, 1, "6"}, {'7', 0, 6, 1, "7"}, {'8', 0, 7, 1, "8"}, {'9', 0, 8, 1, "9"}, {'0', 0, 9, 1, "0"},
    {'q', 1, 0, 1, "q"}, {'w', 1, 1, 1, "w"}, {'e', 1, 2, 1, "e"}, {'r', 1, 3, 1, "r"}, {'t', 1, 4, 1, "t"},
    {'y', 1, 5, 1, "y"}, {'u', 1, 6, 1, "u"}, {'i', 1, 7, 1, "i"}, {'o', 1, 8, 1, "o"}, {'p', 1, 9, 1, "p"},
    {'a', 2, 0, 1, "a"}, {'s', 2, 1, 1, "s"}, {'d', 2, 2, 1, "d"}, {'f', 2, 3, 1, "f"}, {'g', 2, 4, 1, "g"},
    {'h', 2, 5, 1, "h"}, {'j', 2, 6, 1, "j"}, {'k', 2, 7, 1, "k"}, {'l', 2, 8, 1, "l"}, {kBack, 2, 9, 1, "<-"},
    {'z', 3, 0, 1, "z"}, {'x', 3, 1, 1, "x"}, {'c', 3, 2, 1, "c"}, {'v', 3, 3, 1, "v"}, {'b', 3, 4, 1, "b"},
    {'n', 3, 5, 1, "n"}, {'m', 3, 6, 1, "m"}, {'-', 3, 7, 1, "-"}, {'_', 3, 8, 1, "_"}, {'.', 3, 9, 1, "."},
    {kShift, 4, 0, 2, "Shift"}, {' ', 4, 2, 4, "Space"}, {kEnter, 4, 6, 2, "OK"}, {kClose, 4, 8, 2, "Close"},
};
constexpr int kKeyCount = sizeof kKeys / sizeof kKeys[0];

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

bool Osk::tap(int x, int y)
{
    // Every pixel from the first key row down belongs to a key: a key's hit
    // area is its whole cell, including the gaps to its right and below, and
    // the edges extend to the area's border, so a tap between keys still
    // types the nearer one.
    if (y < kKeysTop - 2)
        return false;
    int row = (y - kKeysTop) / kRowPitch, unit = (x - kMargin) / kUnit;
    row = row < 0 ? 0 : row > 4 ? 4 : row;
    unit = x < kMargin ? 0 : unit > 9 ? 9 : unit;
    for (int i = 0; i < kKeyCount; i++) {
        const Key &k = kKeys[i];
        if (k.row == row && unit >= k.unit && unit < k.unit + k.units) {
            flash_ = i;
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

std::string Osk::title() const
{
    // 40 characters fill the 320-pixel line.
    return shift_ ? "SHIFT  B:del Y:shift Start:OK Sel:close" : "Keys   B:del Y:shift Start:OK Sel:close";
}

std::vector<Osk::KeyView> Osk::keys() const
{
    std::vector<KeyView> out;
    out.reserve(kKeyCount);
    for (int i = 0; i < kKeyCount; i++) {
        const Key &k = kKeys[i];
        std::string label = k.label;
        if (shift_ && label.size() == 1 && std::isalpha((unsigned char)label[0]))
            label[0] = (char)std::toupper((unsigned char)label[0]);
        out.push_back({kMargin + k.unit * kUnit, kKeysTop + k.row * kRowPitch, k.units * kUnit - (kUnit - kKeyW), kKeyH,
                       label, i == flash_});
    }
    return out;
}

}  // namespace game
