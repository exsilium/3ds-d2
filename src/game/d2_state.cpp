// See d2_state.h.
#include "game/d2_state.h"

#include "runtime/rt_host.h"

#include <cstdint>

namespace game {
namespace {

constexpr uint32_t kCheckVa = 0x0045B440;     // push ebp
constexpr uint32_t kPlayerUnitVa = 0x007A6A70;
constexpr uint32_t kUiTableVa = 0x007A27C0;
constexpr uint32_t kFocusVa = 0x007D55C8;
constexpr uint32_t kEditBoxType = 1;
// UI table entries that are panels over the world: inventory, character
// sheet, skill tree, NPC dialogue, game menu, shop, add-socket, waypoint,
// quest log, stash, mercenary inventory, trade/cube.
constexpr uint8_t kPanels[] = {0x01, 0x02, 0x04, 0x08, 0x09, 0x0C, 0x0E, 0x14, 0x19, 0x1A, 0x21, 0x24};

}  // namespace

GameState read_game_state()
{
    GameState s;
    uint8_t check = 0;
    if (!rt::guest_read(kCheckVa, &check, 1) || check != 0x55)
        return s;   // not the 1.14d layout: report nothing, the controls stay in cursor mode
    s.known = true;
    uint32_t player = 0;
    rt::guest_read(kPlayerUnitVa, &player, 4);
    s.in_game = player != 0;
    if (s.in_game) {
        uint32_t ui[0x25] = {};
        if (rt::guest_read(kUiTableVa, ui, sizeof ui))
            for (uint8_t i : kPanels)
                if (ui[i]) {
                    s.panel_open = true;
                    break;
                }
    }
    uint32_t focus = 0, type = 0;
    if (rt::guest_read(kFocusVa, &focus, 4) && focus && rt::guest_read(focus, &type, 4) && type == kEditBoxType)
        s.edit_focus = focus;
    // Transitions go to the runtime log (rare, and useful to follow a session).
    static bool was_in_game = false, was_panel = false, was_edit = false;
    if (s.in_game != was_in_game || s.panel_open != was_panel || (s.edit_focus != 0) != was_edit) {
        rt::logf("game state: %s%s%s", s.in_game ? "in game" : "menus", s.panel_open ? ", panel open" : "",
                 s.edit_focus ? ", text field" : "");
        was_in_game = s.in_game;
        was_panel = s.panel_open;
        was_edit = s.edit_focus != 0;
    }
    return s;
}

}  // namespace game
