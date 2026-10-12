// Controller input: RecompFrontend's recompinput (SDL keyboard + game controllers, remappable in the Controls
// tab; defaults in config.cpp), single player into N64 controller port 1. The runtime's osCont* replacements
// call these callbacks; the game's own osContInit/osContStartReadData/osContGetReadData are named, so they go
// through the runtime.

#include "ultramodern/input.hpp"
#include "recompinput/recompinput.h"
#include "recompinput/input_state.h"
#include "recompinput/profiles.h"
#include "recompinput/players.h"

void adam_debug_wait_while_paused(); // main.cpp

namespace {
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
}

namespace adam::input {
    void init() {
        recompinput::players::set_single_player_mode(true);
    }

    ultramodern::input::callbacks_t callbacks() {
        return ultramodern::input::callbacks_t{
            .poll_input = poll_input,
            .get_input = recompinput::profiles::get_n64_input,
            .set_rumble = recompinput::set_rumble,
            .get_connected_device_info = get_connected_device_info,
        };
    }
}
