# Controls (3DS game app)

The mapping lives in `src/game/controls.cpp`. It follows the ideas D2Vita proved on the Vita, adapted to the 3DS's buttons and two screens:
- an analog stick walks the character;
- touch is a touchpad for the cursor;
- the face and shoulder buttons are the mouse buttons and D2's hotkeys.

## Screens

| Screen | Shows |
| --- | --- |
| Top | The whole 800×600 frame stretched over all 400×240 (2:1 across, 2.5:1 down), each pixel averaging a 2×2 block. **Select** switches to a 1:1 400×240 window that follows the cursor, for reading text. |
| Bottom | Diagnostics: game time, fps, worst frame gap, draw time per frame, guest heap and VirtualAlloc use against their limits (with peaks), host heap, linear and application memory free, files and bytes read, and the latest runtime messages. It is also the touchpad. |

## Buttons

| Input | Action |
| --- | --- |
| Circle pad | In menus and with a panel open: move the cursor. In the game world: walk. The cursor orbits the character in the stick's direction, the left button is held while the pad is pushed, and a fuller push reaches further. |
| C-stick (New 3DS) | Move the cursor |
| Touch (bottom screen) | A touchpad: drag moves the cursor (one drag across the pad crosses the frame); a short tap clicks where the cursor is |
| A | Left mouse button (held) |
| B | Right mouse button (held): cast the right skill |
| X | Inventory (I) |
| Y | Show items on the ground (Alt, held) |
| L | Stand still (Shift, held) |
| D-pad | Belt potions 1–4 (up, right, down, left) |
| ZL / ZR (New 3DS) | Weapon swap (W) / automap (Tab) |
| Start | Escape: game menu, closes panels |
| Select | Top screen: whole frame ↔ 1:1 zoom |
| Select + Start | On-screen keyboard (it also opens by itself when the game focuses a text field) |

**R is a layer key.** While R is held:

| R + | Action |
| --- | --- |
| A | Character screen (C) |
| B | Skill tree (T) |
| X | Automap (Tab) |
| Y | Quests (Q) |
| D-pad | Skill hotkeys F1–F4 |
| Start | Enter: confirm, e.g. OK after typing a character name |
| Select | Close all panels (Space) |

## Cinematics

A, B, Start (Escape), R + Start (Enter) or a tap on the touchpad skips the video playing, as a key or click does on the PC. The press is held back until it is released, so it does not also act on the next screen. [cinematics.md](cinematics.md) explains the player.

## How input reaches the game

Each poll (every ≥ 16 ms, from the game's own message pump) becomes the window messages Windows would send:
- `WM_MOUSEMOVE`, with consecutive moves merged;
- `WM_LBUTTONDOWN`/`UP` and `WM_RBUTTONDOWN`/`UP`, with lParam = `(y << 16) | x` in game pixels;
- `WM_KEYDOWN`/`UP` with set-1 scan codes. Alt is sent as `WM_SYSKEY*`, and Enter, Escape and Backspace also produce `WM_CHAR`;
- `WM_CHAR` for typed text.

`GetKeyState` and `GetCursorPos` change when the event is queued. `DispatchMessageA` and `SendMessageA` tail-call the window procedure (`rt::tail_call`), so it runs as ordinary guest code that may wait on the game's other threads. When it ran as a nested call, menu clicks were silently lost.

Two timing rules the game needs:
- **Taps:** a tap holds the button for 120 ms. Down and up inside one 40 ms game frame do not count as a click.
- **Walking:** the cursor moves first and the button goes down on the next poll. The game resolves a click against what the cursor hovered in the previous frame.

On the Linux lane, `M4_SCRIPT` drives the same `game::Controls` with scripted pad events (`tests/m4/game_main.cpp`). The scripted run below goes from the main menu, through a new Barbarian, into the Rogue Encampment:

`20000 tap 400 307; 26000 tap 400 290; 32000 text Tester; 34000 press R; 34100 press START; 34300 release START; 34400 release R`

## On-screen keyboard

When the game focuses a text field, such as the hero name box, a keyboard replaces the message log on the bottom screen. Select+Start opens it at any time. `src/game/osk.cpp` holds the layout and behaviour.

- **Typing:** tap the keys, or click them with the mouse in Azahar. Characters go to the game as `WM_CHAR`.
- **Keys:** `<-` deletes, `Shift` capitalises the next letter, `OK` sends Enter and closes, `Close` just closes.
- **Buttons while it is open:** B deletes, Y is shift, Start is OK, Select closes. The button that closed the keyboard is held back from the game until it is released.
- **Field detection:** D2Win's focused-control pointer at `0x7D55C8`; type 1 at the control is an edit box. The field's text is never read: on Battle.net it can be a password.

## Telling menus from the game world

`src/game/d2_state.cpp` reads two locations in Game.exe 1.14d's memory on every input poll (addresses from D2Vita's reverse engineering, used as facts):
- **In game:** the player unit pointer at `0x7A6A70` is non-null.
- **Panel open:** one of the panel entries in the UI table at `0x7A27C0` is non-zero (inventory, character sheet, skill tree, NPC dialogue, game menu, shop, waypoint, stash, and others).

Before trusting either, it checks that a known function entry at `0x45B440` starts with `push ebp`. On any other executable the state stays "menus", so the circle pad only moves the cursor. Transitions are logged, for example `game state: in game, panel open`.

## Azahar's default keyboard mapping

| 3DS | Key | | 3DS | Key |
| --- | --- | --- | --- | --- |
| A | A | | Circle pad | arrow keys |
| B | S | | C-stick | I J K L |
| X | Z | | D-pad | T F G H |
| Y | X | | Start | M |
| L | Q | | Select | N |
| R | W | | ZL / ZR | 1 / 2 |
| Touch | mouse on the bottom screen | | | |

