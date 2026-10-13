// Controller input: RecompFrontend's recompinput (SDL keyboard + game controllers, remappable in the Controls
// tab; defaults in config.cpp), single player into N64 controller port 1. The runtime's osCont* replacements
// call these callbacks; the game's own osContInit/osContStartReadData/osContGetReadData are named, so they go
// through the runtime.

#include "ultramodern/input.hpp"
#include "recompinput/recompinput.h"
#include "recompinput/input_state.h"
#include "recompinput/profiles.h"
#include "recompinput/players.h"

#include <array>
#include <atomic>

void adam_debug_wait_while_paused(); // main.cpp
namespace adam::rewind { bool active(); } // rewind.cpp

namespace {
    // What the game last asked the Rumble Pak motor to do, per controller (see adam::input::apply_rumble).
    std::array<std::atomic<bool>, 4> game_rumble{};

    void set_rumble(int controller_num, bool on) {
        if (controller_num >= 0 && controller_num < (int)game_rumble.size()) {
            game_rumble[controller_num] = on;
        }
    }

    void poll_input() {
        // P in main.cpp pauses the game here (this runs on the game thread that reads the controller).
        adam_debug_wait_while_paused();
        recompinput::poll_inputs();
    }

    ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
        if (controller_num == 0) {
            return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
        }
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }

    // While rewinding, the rewound frames run with no buttons pressed (a held Start would open menus).
    bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
        if (adam::rewind::active()) {
            *buttons = 0;
            *x = 0.0f;
            *y = 0.0f;
            return true;
        }
        return recompinput::profiles::get_n64_input(controller_num, buttons, x, y);
    }
}

namespace adam::input {
    void init() {
        recompinput::players::set_single_player_mode(true);
    }

    // Called every VI before recompinput::update_rumble (main.cpp). While the game is held (P, a menu) or rewinding, the
    // motor is off: the game can't send "stop" then, so a rumble that was running when the menu opened went on and on.
    // Afterwards the game's last request applies again.
    void apply_rumble(bool game_held) {
        for (size_t i = 0; i < game_rumble.size(); i++) {
            recompinput::set_rumble((int)i, game_rumble[i].load() && !game_held && !adam::rewind::active());
        }
    }

    ultramodern::input::callbacks_t callbacks() {
        return ultramodern::input::callbacks_t{
            .poll_input = poll_input,
            .get_input = get_input,
            .set_rumble = set_rumble,
            .get_connected_device_info = get_connected_device_info,
        };
    }
}
