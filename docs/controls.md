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
| Select | Top screen: whole frame ↔ 1:1 zoom (on release, when no D-pad button was pressed with it) |

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

**Select is a layer key for the bottom screen.** While Select is held (without R), the D-pad does not use the belt:

| Select + | Action |
| --- | --- |
| D-pad right / left | Next / previous tab along the bar (PANEL included while a panel is open), wrapping around |
| D-pad up | MAP (the map, also from HELP) |
| D-pad down | Switches the map's size (x1 ↔ x1/2), showing MAP |

## Bottom screen

`src/game/second_screen.cpp` draws the bottom screen and handles its touches. The tab bar is the top 18 pixels. Tap a tab to show it. Tap the tab that is already shown to use its second function. The chosen tab, the map size, the map's marker refresh and whether MAP shows HELP are saved in `sdmc:/3ds/3ds-d2/settings.txt`.

| Tab | Shows | Touch in the area |
| --- | --- | --- |
| MAP | The automap, centred on the player and scrolling with them, at 1:1 or half size. Three buttons under the map: how often the crosses and names of others are refreshed while you move (mark .25s, .5s, 1s, 2s, 5s), the size (x1, x1/2; tapping MAP again also switches it), and **?** in the corner. The game's own map on the top screen (Tab) is separate and stays off unless you turn it on. | Touchpad, except the buttons |
| HELP | Shown in the MAP tab by **?**: the controls, one line per button or combination. The tab reads HELP and the corner button MAP; tapping either goes back to the map. **?** works before the game too, under "The map shows here in the game." On the first start (no `settings.txt` yet) the tab opens on HELP. | Touchpad, except the MAP button |
| KEYS | 20 buttons: Skill 1–8 (F1–F8), Character, Inventory, Skills, Quests, Party, Merc, Map (Tab), Run/Walk, Swap (W), Items (Alt, latched: tap on, tap off), Chat (Enter), Close all (Space) | Presses the button: the key is held while you touch it |
| ZOOM | The frame at 1:1 around the cursor: readable text and item details | Points at the game (below) |
| INFO | The diagnostics: game time, fps, worst frame gap, draw time per frame, guest heap and VirtualAlloc use against their limits (with peaks), host heap, linear and application memory free, files and bytes read, the last 10 s's hitches (frames over 100 and 250 ms, the longest; frames not drawn and sound starved; what held up the drawing thread: waiting, ready while another game thread ran, host work on its core, and the last heartbeat's length), then the latest runtime messages. **report a problem** at the bottom asks for a [problem report](../README.md#problem-reports) of this session when the game ends; tap it again to take that back. **profiler run** opens a checklist of what to measure: sprite trace, guest code profile (about 12% slower while on), file-read trace, SD card benchmark (a second's pause). Tap the options, then **start the run**; the button stays lit (and the first line shows PROF) until **stop the run**. The ticks are remembered; they cannot change during a run. A session with a run is offered as a problem report when the game ends | Touchpad, except the buttons (and the checklist while it is open) |
| PANEL | The open panel's half of the frame at 1:1. At 640×480 D2's panels are 320 pixels wide, the width of the screen. It scrolls to follow the cursor, and a bar on the right shows where you are. With panels on both sides (stash and inventory, shop and inventory), it shows the side the cursor is on. Tap PANEL again to move the cursor to the same spot on the other side. | Points at the game (below) |

PANEL appears when a panel opens and is shown by itself. Choosing another tab hides it until another panel opens. When the panels close, the bottom screen goes back to your tab.

**Pointing** (ZOOM and PANEL): the cursor goes to the touched spot and follows the finger. A short tap is a left click there; it picks up or drops an item, buys, or walks. Holding still for half a second is a right click; it drinks or uses an item, or casts. Dragging near the edge of the area scrolls it.

The panels are found from the UI table at `0x7A27C0` (see [Telling menus from the game world](#telling-menus-from-the-game-world)): inventory and skill tree on the right; character sheet, shop, socketing, quest log, waypoints, party, trade, stash, cube and mercenary on the left; NPC dialogue in the centre. The game menu (Start) is not offered: it is readable on the top screen as it is.

**The automap** (`src/game/automap.cpp`): the bottom map is drawn in two parts.

- **Tiles** (floors, walls, objects, extras): drawn by 3ds-d2 itself from the game's map data, every time the player moves a pixel, at the player's exact position, so the map scrolls smoothly. The level's map layer (`0x7A5164`) holds a binary tree of cells per kind, ordered by y; each cell names a frame of the map's DC6 cell file (`0x7A5168`) and a position in map pixels (world pixels / 10). The frames are decoded once and kept as runs of pixels. Drawn the way the game draws them (bottom-left corner at the cell's position plus the frame's offsets, opaque), they matched every pixel of a capture of the game's own map in the lane. The game draws the ground as single dots with black between; at 1:1 that turned each pixel on and off as the map moved, so the black around each lone dot takes its colour and the ground shows solid, as at half size. Drawing takes about 2 ms in Azahar, where the game's own map took about 16 ms, 15 of them for the tiles.
- **Units** (the crosses and names of NPCs and other players, waypoint labels): captured from the game. Game.exe's UI pass draws the map with `0x45AD60` when `ui[0x0A]` (Tab) is set. That call site (`0x456F93`) now calls a small stub of guest code in the zero slack after `.text` (`0x6CB5B0`). When a capture is due, the stub runs only the game's map setup (`0x457520`) and units pass (`0x45AC90`, about 1 ms), with the map's offsets and unit bounds set so it draws around the player into a region at the frame's top-left corner, the size of the bottom map plus a margin. That part of the frame is copied aside and cleared to black first; every pixel drawn is kept, and the frame gets its copy back, so the top screen is unaffected. The player's own cross is left out (a filter in front of the per-unit function `0x45A860` skips the player while capturing) and drawn by 3ds-d2 at the centre, in the game's colour for it (`0x7A51B0`). Between captures the unit pixels move with the map. Captures come at the marker refresh set under the map while the player moves, every two seconds standing still (attacking in place included), and at once when MAP is shown, its size changes or the player nears the edge of the last capture. The stub calls the game's functions as ordinary guest code because the map's first draw in a level waits for the game's other threads, which a native `guest_call` cannot do.

On the Linux lane, `M4_BOTTOM=map|help|keys|zoom|info` picks the starting tab. The script event `<ms> touch <x> <y> [ms]` touches the bottom screen, and each `shot` also saves the bottom screen as `b<ms>.png`.

## Screenshots

**L + R + Select** saves what the game is showing into the console's Camera album, the way L + R does in Animal Crossing: New Leaf. Hold L and R, then press Select. `src/app/ctr_screenshot.cpp` writes the file.

- **Where:** `sdmc:/DCIM/<NNN>NIN03/HNI_<NNNN>.JPG`, the folder and names the camera uses. The number is one past the highest in the newest folder. A folder holding 100 pictures moves on to the next one.
- **What:** the game's own frame, not the 400×240 screen. In game that is 640×480, the camera's own photo size. Larger frames, such as the 800×600 menus, are scaled to fit 640×480. Cinematics keep their aspect ratio. The zoom setting makes no difference.
- **File:** a baseline JPEG with an EXIF block, holding the date and a 160×120 thumbnail.
- **While saving:** the top screen holds the frame for a moment. The bottom screen then shows the path, or why saving failed.

Rosalina's screenshot (Luma3DS menu) still works too. It saves both screens as BMPs in `/luma/screenshots/`.

## Problem reports

The top screen shows [problem reports](../README.md#problem-reports) before the game starts (when reports wait to be sent, or with Select held while 3ds-d2 starts) and after it ends (when a report was made for that session).

| Screen | Input | Action |
| --- | --- | --- |
| List | Up / Down, L / R | Choose a report; a page up or down |
| List | A | Open the report |
| List | X | Send every report waiting, oldest first; stops at the first that fails (no Wi-Fi), the rest wait |
| List | Start | Go on (to the game, or out of the app after a session) |
| Report | A | Send it; then its Sentry ID, and A to go on |
| Report | Y | Write or change the description (the system keyboard; the game's own if that does not start) |
| Report | X | Mark it as not to be sent |
| Report | Select, twice | Delete the report and its files |
| Report | B | Back to the list |

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

