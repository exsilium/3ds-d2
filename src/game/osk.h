// On-screen keyboard for the game's text fields (character names, chat):
// a grid of keys in a 40x17 character area of the bottom screen, tapped with
// the touch screen. Typed keys go to the game as WM_CHAR (rt::input_char);
// Backspace and Enter as real key presses. While it is open the bottom
// screen's touch and the buttons below belong to it:
//   B       backspace          Y       shift
//   Start   OK (Enter) + close Select  close
// Platform-neutral: the platform draws rows() and passes taps to tap().
#ifndef D2_GAME_OSK_H
#define D2_GAME_OSK_H

#include <cstdint>
#include <string>
#include <vector>

namespace game {

class Osk {
public:
    static constexpr int kCols = 40, kRows = 17;   // the area it draws in (characters)

    void open();
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    // A tap at character cell (col, row) of the area. Returns true if a key was hit.
    bool tap(int col, int row);
    // The clock, once per poll: ends the flash of a pressed key.
    void tick(uint64_t now_ms);
    // Buttons, as Button bits newly pressed this poll (controls.h).
    void buttons(uint32_t pressed);

    // What to draw: kRows strings of kCols characters, and for each cell
    // whether it is part of a key (drawn highlighted).
    struct Row {
        std::string text;
        std::vector<bool> key;
    };
    std::vector<Row> rows() const;
    bool dirty() const { return dirty_; }    // rows() changed since the last clear_dirty()
    void clear_dirty() { dirty_ = false; }

private:
    void press(int code);

    bool open_ = false, shift_ = false, dirty_ = true;
    int flash_ = -1;                   // index of the key shown pressed, -1 = none
    uint64_t now_ms_ = 0, flash_until_ms_ = 0;
};

}  // namespace game

#endif
