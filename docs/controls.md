# Controls (3DS game app)

The mapping lives in `src/game/controls.cpp`. It follows the ideas D2Vita proved on the Vita, adapted to the 3DS's buttons and two screens:
- an analog stick walks the character;
- touch is a touchpad for the cursor;
- the face and shoulder buttons are the mouse buttons and D2's hotkeys.

## Screens

| Screen | Shows |
| --- | --- |
| Top | The whole 800×600 frame stretched over all 400×240 (2:1 across, 2.5:1 down), each pixel averaging a 2×2 block. **Select** switches to a 1:1 400×240 window that follows the cursor, for reading text. |
| Bottom | A row of tabs (MAP, KEYS, ZOOM, INFO, and PANEL while a panel is open) over a 320×222 area. See [Bottom screen](#bottom-screen) below. Until the game shows its first frame, it shows the startup messages. |

## Buttons

| Input | Action |
| --- | --- |
| Circle pad | In menus and with a panel open: move the cursor. In the game world: walk. The cursor orbits the character in the stick's direction, the left button is held while the pad is pushed, and a fuller push reaches further. |
| C-stick (New 3DS) | Move the cursor |
| Touch (bottom screen) | Depends on the tab (see [Bottom screen](#bottom-screen)). On MAP and INFO, a touchpad: drag moves the cursor (one drag across the pad crosses the frame); a short tap clicks where the cursor is |
| A | Left mouse button (held) |
| B | Right mouse button (held): cast the right skill |
| X | Inventory (I) |
| Y | Show items on the ground (Alt, held) |
| L | Stand still (Shift, held) |
| D-pad | Belt potions 1–4 (up, right, down, left) |
| ZL / ZR (New 3DS) | Weapon swap (W) / automap (Tab) |
| Start | Escape: game menu, closes panels |
| Select | Top screen: whole frame ↔ 1:1 zoom |

**R is a layer key.** While R is held:

| R + | Action |
| --- | --- |
| A | Character screen (C) |
| B | Skill tree (T) |
| X | Automap (Tab) |
| Y | Quests (Q) |
| D-pad | Skill hotkeys F1–F4 |
| Start | Enter: confirm, e.g. OK after typing a character name |
| Select | On-screen keyboard (it also opens by itself when the game focuses a text field) |
| L + Select | Screenshot into the Camera album (see below) |

## Bottom screen

`src/game/second_screen.cpp` draws the bottom screen and handles its touches. The tab bar is the top 18 pixels. Tap a tab to show it. Tap the tab that is already shown to use its second function. The chosen tab and the map size are saved in `sdmc:/3ds/3ds-d2/settings.txt`.

| Tab | Shows | Touch in the area |
| --- | --- | --- |
| MAP | The automap, centred on the player, at 1:1 or half size. Tap MAP again to switch the size. The game's own map on the top screen (Tab) is separate and stays off unless you turn it on. | Touchpad |
| KEYS | 20 buttons: Skill 1–8 (F1–F8), Character, Inventory, Skills, Quests, Party, Merc, Map (Tab), Run/Walk, Swap (W), Items (Alt, latched: tap on, tap off), Chat (Enter), Close all (Space) | Presses the button: the key is held while you touch it |
| ZOOM | The frame at 1:1 around the cursor: readable text and item details | Points at the game (below) |
| INFO | The diagnostics: game time, fps, worst frame gap, draw time per frame, guest heap and VirtualAlloc use against their limits (with peaks), host heap, linear and application memory free, files and bytes read, then the latest runtime messages | Touchpad |
| PANEL | The open panel's half of the frame at 1:1. At 640×480 D2's panels are 320 pixels wide, the width of the screen. It scrolls to follow the cursor, and a bar on the right shows where you are. With panels on both sides (stash and inventory, shop and inventory), it shows the side the cursor is on. Tap PANEL again to move the cursor to the same spot on the other side. | Points at the game (below) |

PANEL appears when a panel opens and is shown by itself. Choosing another tab hides it until another panel opens. When the panels close, the bottom screen goes back to your tab.

**Pointing** (ZOOM and PANEL): the cursor goes to the touched spot and follows the finger. A short tap is a left click there; it picks up or drops an item, buys, or walks. Holding still for half a second is a right click; it drinks or uses an item, or casts. Dragging near the edge of the area scrolls it.

The panels are found from the UI table at `0x7A27C0` (see [Telling menus from the game world](#telling-menus-from-the-game-world)): inventory and skill tree on the right; character sheet, shop, socketing, quest log, waypoints, party, trade, stash, cube and mercenary on the left; NPC dialogue in the centre. The game menu (Start) is not offered: it is readable on the top screen as it is.

**The automap** (`src/game/automap.cpp`): Game.exe's UI pass draws the map with `0x45AD60` when `ui[0x0A]` (Tab) is set. That call site (`0x456F93`) now calls a small stub of guest code, placed in the zero slack after `.text` (`0x6CB5B0`). While MAP is shown, every third UI pass also draws the map for the bottom screen: the frame is copied aside and cleared to black, the game draws the map into it, and every pixel it drew is kept as map. The frame then gets its copy back, so the top screen is unaffected. (Over black rather than over the world: the map is drawn blended with what is under it.) At 1:1 the bottom screen keeps the map's checkerboard of ground dots on the same screen squares, moving the map in steps of two pixels where needed, so the ground does not shimmer as the player walks. The stub calls the game's functions as ordinary guest code because the map's first draw in a level waits for the game's other threads, which a native `guest_call` cannot do.

On the Linux lane, `M4_BOTTOM=map|keys|zoom|info` picks the starting tab. The script event `<ms> touch <x> <y> [ms]` touches the bottom screen, and each `shot` also saves the bottom screen as `b<ms>.png`.

## Screenshots

**L + R + Select** saves what the game is showing into the console's Camera album, the way L + R does in Animal Crossing: New Leaf. Hold L and R, then press Select. `src/app/ctr_screenshot.cpp` writes the file.

- **Where:** `sdmc:/DCIM/<NNN>NIN03/HNI_<NNNN>.JPG`, the folder and names the camera uses. The number is one past the highest in the newest folder. A folder holding 100 pictures moves on to the next one.
- **What:** the game's own frame, not the 400×240 screen. In game that is 640×480, the camera's own photo size. Larger frames, such as the 800×600 menus, are scaled to fit 640×480. Cinematics keep their aspect ratio. The zoom setting makes no difference.
- **File:** a baseline JPEG with an EXIF block, holding the date and a 160×120 thumbnail.
- **While saving:** the top screen holds the frame for a moment. The bottom screen then shows the path, or why saving failed.

Rosalina's screenshot (Luma3DS menu) still works too. It saves both screens as BMPs in `/luma/screenshots/`.

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

When the game focuses a text field, such as the hero name box, a keyboard takes over the bottom screen, under the diagnostics. R + Select opens it at any time. `src/game/osk.cpp` holds the layout and behaviour.

- **Typing:** tap the keys, or click them with the mouse in Azahar. Characters go to the game as `WM_CHAR`.
- **Keys:** `<-` deletes, `Shift` capitalises the next letter, `OK` sends Enter and closes, `Close` just closes.
- **Buttons while it is open:** B deletes, Y is shift, Start is OK, Select closes. The button that closed the keyboard is held back from the game until it is released.
- **Field detection:** D2Win's focused-control pointer at `0x7D55C8`; type 1 at the control is an edit box. The field's text is never read: on Battle.net it can be a password.

## Telling menus from the game world

`src/game/d2_state.cpp` reads two locations in Game.exe 1.14d's memory on every input poll (addresses from D2Vita's reverse engineering, used as facts):
- **In game:** the player unit pointer at `0x7A6A70` is non-null.
- **Panel open:** one of the panel entries in the UI table at `0x7A27C0` is non-zero (inventory, character sheet, skill tree, NPC dialogue, game menu, shop, quest log, waypoint, party, stash, and others). Every non-zero entry below `0x28` is logged with each change, for example `game state: in game, panel open (ui 800208003)`.

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

