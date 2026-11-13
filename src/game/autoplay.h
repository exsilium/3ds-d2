// Scripted pad input, for unattended runs (the Linux game lane's M4_SCRIPT,
// the 3DS app's sdmc:/3ds/3ds-d2/autoplay.txt). Events are separated by ';'
// or newlines; each starts with a guest time in milliseconds:
//   <ms> tap <x> <y>       put the cursor on game pixel x,y and click it (A, the
//                          left button, for 100 ms) 300 ms later; every tap waits
//                          so: a slow frame rate still sees the spot hovered
//   <ms> hover <x> <y>     put the cursor on game pixel x,y, without a click
//   <ms> touch <x> <y> [ms] touch the bottom screen at x,y (its pixels) for ms (100)
//   <ms> press <button>    hold a button: A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT
//   <ms> release <button>
//   <ms> circle <x> <y>    circle pad position (-156..156, y up)
//   <ms> text <string>     type characters (WM_CHAR), as the on-screen keyboard does
//   <ms> shot              ask the host for a screenshot (if it can take one)
//   <ms> wait <state>      from <ms> on, hold the script until the game shows
//                          textfield | ingame | menus; the later events' times
//                          then count from that moment, so one script works at
//                          any speed (Linux lane, Azahar, the console)
//   <ms> tapuntil <state> <x> <y>  tap x,y, then again every 5 s, until <state>
//                          (a click that is ignored while a screen still loads)
//   <ms> tapunit <npc> [state]  tap the NPC (d2_units.h: a name such as warriv,
//                          or its monstats class) where it is, again every
//                          1.5 s where it has moved to, until <state> (npcmenu:
//                          its menu is open); off the screen, the spot at the
//                          edge towards it (the player walks there)
//   <ms> tapmenu <line>    tap line <line> (1: the first under the name) of the
//                          menu of the NPC the last tapunit opened, where the
//                          NPC is now, again every 1.5 s until the menu closes
//   <ms> tapobject <class> [state]  as tapunit, for an object by its class
//                          (a waypoint: 119 in the Rogue Encampment), until
//                          <state> (ui20: the waypoint's panel)
//   <ms> goto <class>      walk towards the level's preset object of that class (the
//                          circle pad held its way) until it is loaded and within 8
//                          subtiles, then let go; later events count from there
//   <ms> seek <name>       look for an object by its objects.txt name (Waypoint): walk to
//                          the nearest room not loaded yet until one is loaded, then to
//                          it, until within 8 subtiles; "tapobject last" then taps it
//   <ms> units [all]       log the units and objects near the player (class, id, screen spot);
//                          all: every one loaded, and the level's preset objects
//   <ms> skipvideos        from then on, press A (a click) as each cinematic plays: skips it
//   <ms> closepanels [off] from then on, press START (Escape) while a panel is
//                          open in the game: a walk that clicks into an NPC's
//                          menu goes on walking ("off": no more)
//   <ms> profile <names>   start a profiler run with these options (profiler.h's
//                          names, comma-separated: frames,guest,...); "off" stops it
//   states: textfield, ingame, menus, npcmenu, menuclosed, act1 to act5 (in a game, in that act),
//           ui<n> (in a game, the game's UI entry n open: 8 an NPC's menu, 20 the waypoints)
//   # ...                  a comment line
#ifndef D2_GAME_AUTOPLAY_H
#define D2_GAME_AUTOPLAY_H

#include "game/controls.h"
#include "game/d2_state.h"

#include <cstdint>
#include <string>
#include <vector>

namespace game {

class Autoplay {
public:
    void load(const std::string &script);
    bool active() const { return next_ < events_.size() || !waiting_for_.empty() || pad_.held || pad_.touching || click_at_; }
    // Applies the events due at now_ms to the scripted pad and returns it.
    // shot is set when a "shot" event fired.
    const Pad &step(uint64_t now_ms, Controls &controls, bool &shot, const GameState &state);
    // A "profile" event since the last call: its argument ("off", or the
    // options' names). False if none.
    bool take_profile(std::string &arg) { return !profile_.empty() && (arg.swap(profile_), profile_.clear(), true); }

private:
    std::vector<std::string> events_;
    size_t next_ = 0;
    Pad pad_;
    uint64_t touch_until_ = 0;
    int64_t shift_ms_ = 0;           // added to event times: the time spent waiting
    uint64_t wait_at_ = 0;           // script time of the wait in progress
    std::string waiting_for_;        // a wait in progress
    bool retry_tap_ = false;         // tapuntil: tap again while waiting
    int retry_x_ = 0, retry_y_ = 0;
    uint32_t retry_unit_ = 0;        // tapunit, tapobject: the class tapped, where it is
    uint32_t retry_type_ = 1;        // its unit type: 1 monster or NPC, 2 object
    uint32_t goto_cls_ = 0;          // goto: the object class walked to
    bool goto_near() const;          // goto: the object loaded and within 8 subtiles
    // seek: the object named, the rooms given up on, the one steered to.
    std::string seek_name_;
    std::vector<uint32_t> seek_skip_;
    uint32_t seek_room_ = 0, seek_cls_ = 0;
    double seek_best_ = 1e9;
    uint64_t seek_since_ = 0;
    bool seek_near() const;          // seek: the object loaded and within 8 subtiles
    void seek_step(uint64_t now_ms, Controls &controls);
    uint32_t menu_unit_ = 0;         // the last tapunit's class: whose menu tapmenu taps
    int retry_line_ = 0;             // tapmenu: the line
    uint64_t click_at_ = 0;          // a tap's click, after the cursor is in place
    uint64_t click_until_ = 0;       // a tap's click (A) is held until then
    bool skip_videos_ = false;       // skipvideos: A while a cinematic plays
    uint64_t skip_release_ms_ = 0, skip_next_ms_ = 0;
    bool close_panels_ = false;      // closepanels: START while a panel is open
    uint64_t close_release_ms_ = 0, close_next_ms_ = 0;
    uint64_t next_retry_ms_ = 0;
    std::string profile_;            // a "profile" event's argument, until taken
};

}  // namespace game

#endif
