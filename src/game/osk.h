// On-screen keyboard for the game's text fields (character names, chat):
// a grid of keys drawn in a 320x140-pixel area of the bottom screen and
// tapped with the touch screen. Typed keys go to the game as WM_CHAR
// (rt::input_char); Backspace and Enter as real key presses. While it is
// open the bottom screen's touch and the buttons below belong to it:
//   B       backspace          Y       shift
//   Start   OK (Enter) + close Select  close
// Platform-neutral: the platform draws title() and keys() in pixels and
// passes taps, in the same coordinates, to tap().
//
// open_text() instead types into a string of the platform's, outside the
// game (a report's description, before the game starts or after it ended):
// nothing goes to the game and nothing is logged.
#ifndef D2_GAME_OSK_H
#define D2_GAME_OSK_H

#include <cstdint>
#include <string>
#include <vector>

namespace game {

class Osk {
public:
    // The area it draws in, in pixels; (0, 0) is its top-left corner.
    static constexpr int kWidth = 320, kHeight = 140;

    void open();
    void open_text(std::string *text);
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    // A tap at pixel (x, y) of the area. Returns true if a key was hit.
    bool tap(int x, int y);
    // The clock, once per poll: ends the flash of a pressed key.
    void tick(uint64_t now_ms);
    // Buttons, as Button bits newly pressed this poll (controls.h).
    void buttons(uint32_t pressed);

    // What to draw: the title line (8-pixel text at the top of the area)
    // and each key's rectangle and label. The label is to be centred in
    // the rectangle; a pressed key is drawn inverted for a moment.
    struct KeyView {
        int x, y, w, h;
        std::string label;
        bool pressed;
    };
    std::string title() const;
    std::vector<KeyView> keys() const;
    bool dirty() const { return dirty_; }    // what to draw changed since the last clear_dirty()
    void clear_dirty() { dirty_ = false; }

private:
    void press(int code);

    std::string *text_ = nullptr;      // open_text's string, or null: the game's text field
    bool open_ = false, shift_ = false, dirty_ = true;
    int flash_ = -1;                   // index of the key shown pressed, -1 = none
    uint64_t now_ms_ = 0, flash_until_ms_ = 0;
};

}  // namespace game

#endif
