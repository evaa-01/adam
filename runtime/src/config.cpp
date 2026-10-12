// Settings menu (RecompFrontend): General, Graphics, Controls and Sound tabs. No Mods tab: nothing is written
// for this game. The settings are saved as <tab id>.json in the runtime's config path (runtime\build\data,
// see main.cpp), next to the stored ROM and the saves.
//
// Default controls are the ones adam had before the menus (input.cpp):
//   Keyboard:  stick = arrows, A = X, B = C, Z = Z, Start = Enter, L = A, R = S,
//              C buttons = I/J/K/L, D-pad = T/F/G/H   (P = pause and F9 = RAM dump are debug keys, see main.cpp)
//   Gamepad:   stick = left stick, A = A (south), B = X (west), Z = left trigger, Start, L = LB, R = RB,
//              C buttons = right stick (+ Y/B face buttons, RecompFrontend's default), D-pad = D-pad
// Esc (keyboard) or Back (gamepad) opens the menu in game.

#include <filesystem>

#include "librecomp/game.hpp"
#include "ultramodern/config.hpp"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "recompinput/input_mapping.h"

namespace {
    void set_control_defaults() {
        using namespace recompinput;
        auto key = [](SDL_Scancode k) { return InputField::keyboard(k); };

        set_default_mapping_for_keyboard(GameInput::A,          { key(SDL_SCANCODE_X) });
        set_default_mapping_for_keyboard(GameInput::B,          { key(SDL_SCANCODE_C) });
        set_default_mapping_for_keyboard(GameInput::Z,          { key(SDL_SCANCODE_Z) });
        set_default_mapping_for_keyboard(GameInput::START,      { key(SDL_SCANCODE_RETURN) });
        set_default_mapping_for_keyboard(GameInput::L,          { key(SDL_SCANCODE_A) });
        set_default_mapping_for_keyboard(GameInput::R,          { key(SDL_SCANCODE_S) });
        set_default_mapping_for_keyboard(GameInput::C_UP,       { key(SDL_SCANCODE_I) });
        set_default_mapping_for_keyboard(GameInput::C_DOWN,     { key(SDL_SCANCODE_K) });
        set_default_mapping_for_keyboard(GameInput::C_LEFT,     { key(SDL_SCANCODE_J) });
        set_default_mapping_for_keyboard(GameInput::C_RIGHT,    { key(SDL_SCANCODE_L) });
        set_default_mapping_for_keyboard(GameInput::DPAD_UP,    { key(SDL_SCANCODE_T) });
        set_default_mapping_for_keyboard(GameInput::DPAD_DOWN,  { key(SDL_SCANCODE_G) });
        set_default_mapping_for_keyboard(GameInput::DPAD_LEFT,  { key(SDL_SCANCODE_F) });
        set_default_mapping_for_keyboard(GameInput::DPAD_RIGHT, { key(SDL_SCANCODE_H) });
        set_default_mapping_for_keyboard(GameInput::X_AXIS_NEG, { key(SDL_SCANCODE_LEFT) });
        set_default_mapping_for_keyboard(GameInput::X_AXIS_POS, { key(SDL_SCANCODE_RIGHT) });
        set_default_mapping_for_keyboard(GameInput::Y_AXIS_POS, { key(SDL_SCANCODE_UP) });
        set_default_mapping_for_keyboard(GameInput::Y_AXIS_NEG, { key(SDL_SCANCODE_DOWN) });

        // RecompFrontend's gamepad defaults put R on the right trigger and C-down also on RB; adam used RB for R.
        set_default_mapping_for_controller(GameInput::R, { InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) });
        set_default_mapping_for_controller(GameInput::C_DOWN, { InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, true) });
    }
}

namespace adam {
    const std::string fast_forward_speed_option = "fast_forward_speed";

    // Fast-forward speed from the General tab (main.cpp applies it while the right trigger or Tab is held).
    uint32_t get_fast_forward_speed() {
        return std::get<uint32_t>(recompui::config::get_general_config().get_option_value(fast_forward_speed_option));
    }

    void init_config() {
        recompui::config::GeneralTabOptions general_options{};
        general_options.has_rumble_strength = true;
        recomp::config::Config& general_config = recompui::config::create_general_tab(general_options);
        static const std::vector<recomp::config::ConfigOptionEnumOption> fast_forward_speeds = {
            { 2u, "2x", "2x" }, { 4u, "4x", "4x" }, { 8u, "8x", "8x" },
        };
        general_config.add_enum_option(fast_forward_speed_option, "Fast-Forward Speed",
            "How fast the game runs while you hold the right trigger (keyboard: Tab). Sound is muted meanwhile.",
            fast_forward_speeds, 4u);

        recompui::config::create_graphics_tab();

        set_control_defaults();
        recompui::config::create_controls_tab();

        recompui::config::create_sound_tab();

        // Graphics defaults for a new install (no graphics.json yet): the original N64 picture, except
        // the resolution: Original 2x. RecompFrontend's own defaults are enhancements (Auto resolution, Expand aspect
        // ratio, 16:9 HUD, display refresh rate, MSAA 2x). At Auto (4x in a 1280x960 window) RT64 also draws thin lines
        // across scaled S2DEX2 backgrounds (BG_1CYC slices sample past their TMEM rows; dumps\compare_06 notes).
        // Everything can still be changed in the Graphics tab.
        namespace graphics = recompui::config::graphics;
        namespace renderer = ultramodern::renderer;
        bool first_run = !std::filesystem::exists(recomp::get_config_path() / (graphics::id + ".json"));

        recompui::config::finalize();

        if (first_run) {
            recomp::config::Config& graphics_config = recompui::config::get_graphics_config();
            graphics_config.set_option_value(graphics::options::res_option, uint32_t(renderer::Resolution::Original2x));
            graphics_config.set_option_value(graphics::options::ar_option, uint32_t(renderer::AspectRatio::Original));
            graphics_config.set_option_value(graphics::options::hr_option, uint32_t(renderer::HUDRatioMode::Original));
            graphics_config.set_option_value(graphics::options::rr_option, uint32_t(renderer::RefreshRate::Original));
            graphics_config.set_option_value(graphics::options::msaa_option, uint32_t(renderer::Antialiasing::None));
            graphics_config.set_option_value(graphics::options::hpfb_option, uint32_t(renderer::HighPrecisionFramebuffer::Off));
            graphics_config.save_config();
            graphics_config.load_config(); // applies the saved settings to the renderer config, as at startup
        }
    }
}
