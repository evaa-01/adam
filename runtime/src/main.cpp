// Host for the recompiled game: registers the game with librecomp, opens an SDL window and starts the runtime.
// Launcher, settings menus and input come from RecompFrontend (recompui/recompinput, like Banjo: Recompiled):
// the launcher (Load ROM / Start Game, Controls, Settings, Exit) shows until the game is started; Esc opens
// the settings in game. Graphics: RT64 through RecompFrontend's renderer. Input: src\input.cpp. Settings:
// src\config.cpp. Audio: recompiled aspMain (rsp\aspMain.cpp; the live shell interprets it: src\rsp_interp.cpp) + SDL
// output (src\audio.cpp).
//
// Usage:
//   adam.exe                             launcher
//   adam.exe --game adam                 skip the launcher and start the game (needs a stored ROM)
//   adam.exe --rom <rom.z64>             store the ROM (as the launcher's Load ROM does), then the launcher
//   adam.exe --hash <rom.z64>            print the ROM's XXH3 hash and internal name
//   adam.exe --rsp-test <dump.bin>...    run the audio tasks in RAM dumps through the recompiled aspMain and the RSP
//                                        interpreter and compare (no window)
//   adam.exe --live-test <rom.z64>       live shell build only: recompile the game code from the ROM and report
//                                        (no window)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include "SDL.h"
#include "SDL_syswm.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <thread>
#pragma comment(lib, "dbghelp.lib")

#include "nfd.h"

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"
#include "xxHash/xxh3.h"

#include "recompui/recompui.h"
#include "recompui/program_config.h"
#include "recompui/config.h"
#include "recompui/renderer.h"
#include "recompinput/recompinput.h"
#include "recompinput/input_events.h"
#include "recompinput/input_state.h"
#include "recompinput/profiles.h"
#include "elements/ui_image.h"

#include "launcher_art.h"
#include "embedded_files.h"

#ifndef ADAM_LIVE
#include "recomp_overlays.inl"
#endif

namespace adam::audio {
    void init(uint32_t output_freq);
    ultramodern::audio_callbacks_t callbacks();
}

namespace adam::input {
    void init();
    ultramodern::input::callbacks_t callbacks();
    void apply_rumble(bool game_held);
}

namespace adam {
    void init_config();
    bool get_fast_forward_enabled();
    bool get_rewind_enabled();
}

// rewind.cpp: hold a button to run the game backwards.
// bezels.cpp: artwork in the black bars beside the 4:3 picture.
namespace adam::bezels {
    void update();
    void update_launcher(recompui::Element* container);
    void open_launcher_background_settings();
}

namespace adam::rewind {
    void init();
    void set_held(bool held);
    void set_enabled(bool enabled);
    bool available();
    bool in_gameplay();
}

// rsp_interp.cpp: RSP interpreter (live shell audio) and its comparison test against the recompiled aspMain.
RspExitReason adam_rsp_interpret(uint8_t* rdram, uint32_t ucode_addr);
int adam_rsp_test(int count, char** paths);

#ifdef ADAM_LIVE
// Live shell: the game code is recompiled from the ROM at game start (src\live.cpp).
namespace adam::live {
    bool register_tables();
    void on_init(uint8_t* rdram, recomp_context* ctx);
    void entrypoint(uint8_t* rdram, recomp_context* ctx);
    int test(const char* rom_path);
    bool find_function(const void* addr, uint32_t* vram, uint32_t* offset);
}
static gpr get_entrypoint_address() { return (gpr)(int32_t)0x80096400u; }
#else
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();
#endif

static const std::u8string game_id = u8"adam";
static const std::string mod_game_id = "adam";

// Filled in from `adam.exe --hash rom\game.z64`.
static constexpr uint64_t rom_hash = 0x43A8E44E8287EB13ULL;

// --- Sections -----------------------------------------------------------------------------------

// librecomp only learns where code lives when load_overlays is called.
// - The 7 overlays of the game's overlay table (.ovt0-.ovt6, .ovt2 = .text2) share 0x80025C00; a hook in
//   the game's loader func_800AB6C0 (scripts\extra_hooks.txt) unloads the slot and registers the new one.
// - librecomp's init registers every section whose ROM lies in the first 1MB at the boot copy address;
//   only .text really lives there, so drop the others (ovt0/ovt1/.text2 would land past .text's end).
// - .ovl1 is the only section at 0x8038F800 (checked: nothing else loads code there): register it once here.
static constexpr uint32_t BOOT_ROM = 0x1000;
static constexpr uint32_t BOOT_VRAM = 0x80096400;
static constexpr uint32_t OVT_SLOT_VRAM = 0x80025C00;

#ifndef ADAM_LIVE
static void register_sections(uint8_t* rdram, recomp_context* ctx) {
    for (const SectionTableEntry& section : section_table) {
        if (section.rom_addr == BOOT_ROM) {
            unload_overlays((int32_t)(BOOT_VRAM + section.size), 0x100000 - section.size);
        }
    }
    for (const SectionTableEntry& section : section_table) {
        if (section.rom_addr != BOOT_ROM && section.ram_addr != OVT_SLOT_VRAM) {
            load_overlays(section.rom_addr, (int32_t)section.ram_addr, section.size);
        }
    }
}

static void register_overlay_table() {
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays{
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}
#endif

// --- libultra functions N64Recomp skips but the runtime doesn't provide --------------------------

// osViGetCurrentMode returns the current OSViMode's type. The runtime keeps the mode private, but
// exports the VI registers it derives from it. The game (func_800B71B0) only checks type < 8
// (low-res, 320 wide) vs < 14 (hi-res, 640 wide), and only ever sets OS_VI_NTSC_LAN1 (2) or
// OS_VI_NTSC_HAF1 (11), so the width register tells them apart.
extern "C" void osViGetCurrentMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t OS_VI_NTSC_LAN1 = 2;
    constexpr uint32_t OS_VI_NTSC_HAF1 = 11;
    uint32_t width = ultramodern::renderer::get_vi_regs()->VI_WIDTH_REG;
    ctx->r2 = (width > 320) ? OS_VI_NTSC_HAF1 : OS_VI_NTSC_LAN1;
}

// --- RSP ----------------------------------------------------------------------------------------

// The game's audio microcode: recompiled by RSPRecomp (rsp\aspMain.toml -> rsp\aspMain.cpp) in the normal build; the
// live shell interprets it from RDRAM instead (src\rsp_interp.cpp, verified identical with --rsp-test).
// Graphics tasks don't come through here: the runtime sends them to RT64.
#ifndef ADAM_LIVE
extern RspUcodeFunc aspMain;
#endif

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
        case M_AUDTASK:
#ifdef ADAM_LIVE
            return adam_rsp_interpret;
#else
            return aspMain;
#endif
        default:
            fprintf(stderr, "Unknown RSP task type: %u\n", (unsigned)task->t.type);
            return nullptr;
    }
}

// --- Debug keys: P = pause, F9 = RAM dump; status in the window title ---------------------------

// P pauses the game: the game thread that reads the controller (osContStartReadData -> input.cpp's poll_input)
// waits in adam_debug_wait_while_paused, so the whole game stops while the window keeps showing the last frame.
// Audio goes silent meanwhile. The title shows game time (seconds since the game started, minus pauses), the
// frame number (controller reads: the game polls every VI, so ~60 per second) and the last RAM dump, so screenshots identify
// themselves.
//
// F9 writes the first 4 MB of RDRAM to dumps\inbox\adam_<n>.bin in the project (next free n; next to the exe if
// there's no ..\..\dumps folder), big-endian like a Project64 dump, so the two can be compared byte for byte (the
// runtime keeps each 32-bit word byte-swapped). Unless paused, the game keeps running while it's copied, so a few
// bytes may be mid-update.
//
// Both keys only act once the game has started and while no menu is open (they're read with GetAsyncKeyState,
// outside SDL, so the menus don't see them).
static uint8_t* dump_rdram = nullptr;
static HWND main_window = nullptr;
static std::atomic<bool> paused = false;
static std::atomic<uint32_t> frame_count = 0;

uint32_t adam_frame_count() {
    return frame_count.load();
}
static std::string last_dump_name;

// Also holds the game while a menu is open in game (Esc: settings, quit prompt...), the same way as P.
void adam_debug_wait_while_paused() {
    frame_count++;
    // (While a menu holds the game, RecompFrontend's renderer keeps re-presenting the last frame, so the menu keeps
    // working: runtime\patches\recompfrontend-menu-pause.patch.)
    while (paused.load() || recompui::is_context_capturing_input()) {
        Sleep(5);
    }
}

// Fast-forward: a framebuffer swap is applied at a VI only once the graphics tasks in flight when the game asked for it
// are done (runtime\patches\n64modernruntime-vi-swap-gate.patch). Without this, at 4x the battle's HUD flickered and
// frames showed black. The count is the game's pending counter 0x8016D060 (+1 when func_800B666C submits a task, -1
// when the task-done thread sees it finish). Waiting for it to reach 0 instead froze the game at 8x: by then the next
// frame's tasks were already queued, and the game only runs them after the swap.
static uint32_t gfx_tasks_in_flight() {
    int32_t pending = dump_rdram == nullptr ? 0 : *(const int32_t*)(dump_rdram + 0x16D060);
    return pending > 0 ? (uint32_t)pending : 0;
}

static std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    dump_rdram = rdram;
    ultramodern::set_swap_gate(gfx_tasks_in_flight);
    // SkipBuffering is RT64's default, which the 30 fps pacing (tag pacing-30fps) was measured with.
    return recompui::renderer::create_render_context(rdram, window_handle, ultramodern::renderer::PresentationMode::SkipBuffering, developer_mode);
}

static bool debug_keys_active() {
    return ultramodern::is_game_started() && !recompui::is_context_capturing_input() && GetForegroundWindow() == main_window;
}

// Game time in seconds: since the first call, not counting time spent paused.
static double game_seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    static clock::duration paused_total{};
    static clock::time_point pause_start{};
    static bool was_paused = false;
    clock::time_point now = clock::now();
    bool is_paused = paused.load();
    if (is_paused && !was_paused) {
        pause_start = now;
    }
    if (!is_paused && was_paused) {
        paused_total += now - pause_start;
    }
    was_paused = is_paused;
    return std::chrono::duration<double>((is_paused ? pause_start : now) - start - paused_total).count();
}

static void check_pause_key() {
    static bool was_down = false;
    bool down = (GetAsyncKeyState('P') & 0x8000) != 0 && debug_keys_active();
    if (down && !was_down) {
        paused = !paused.load();
        printf("[debug] %s at %.2f s, frame %u\n", paused.load() ? "paused" : "resumed", game_seconds(), frame_count.load());
    }
    was_down = down;
}

static void update_title() {
    static std::wstring shown;
    static ULONGLONG last_update = 0;
    if (main_window == nullptr || !ultramodern::is_game_started() || GetTickCount64() - last_update < 50) {
        return;
    }
    last_update = GetTickCount64();
    char text[256];
    snprintf(text, sizeof(text), "ADAM  |  %.2f s  |  frame %u  |  %s  |  last dump: %s", game_seconds(), frame_count.load(),
        paused.load() ? "PAUSED (P = resume)" : "P = pause, F9 = RAM dump", last_dump_name.empty() ? "-" : last_dump_name.c_str());
    std::wstring title{ text, text + strlen(text) };
    if (title != shown) {
        SetWindowTextW(main_window, title.c_str());
        shown = title;
    }
}

static void check_ram_dump_key() {
    static bool was_down = false;
    static int dump_count = 0;
    bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0 && debug_keys_active();
    if (down && !was_down && dump_rdram != nullptr) {
        constexpr size_t size = 4 * 1024 * 1024;
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; i++) {
            data[i] = dump_rdram[i ^ 3];
        }
        // First unused number, so dumps from earlier runs are never overwritten.
        char exe_path[MAX_PATH];
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path{ exe_path }.parent_path();
        // ADAM_DUMP_DIR=<folder> overrides the folder (test dumps that don't belong in the project's dumps\).
        if (const char* env_dir = std::getenv("ADAM_DUMP_DIR"); env_dir != nullptr && env_dir[0] != '\0') {
            dir = env_dir;
            std::filesystem::create_directories(dir);
        } else if (std::filesystem::is_directory(dir / ".." / ".." / "dumps")) {
            dir = dir / ".." / ".." / "dumps" / "inbox";
            std::filesystem::create_directories(dir);
        }
        std::filesystem::path path;
        do {
            path = (dir / ("adam_" + std::to_string(dump_count++) + ".bin")).lexically_normal();
        } while (std::filesystem::exists(path));
        std::ofstream{ path, std::ios::binary }.write((const char*)data.data(), data.size());
        last_dump_name = path.filename().string();
        printf("[ramdump] wrote %s at %.2f s, frame %u\n", path.string().c_str(), game_seconds(), frame_count.load());
    }
    was_down = down;
}

// --- Window -------------------------------------------------------------------------------------

// RecompFrontend's UI reads this global (as in Banjo: Recompiled).
SDL_Window* window = nullptr;

static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Xbox pads through XInput: a Bluetooth Xbox Wireless Controller (045e:0b22) delivered no input through
    // SDL's HIDAPI Xbox driver and only the triggers through RawInput (tested 2026-10-11). PlayStation pads are unaffected.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) < 0) {
        fprintf(stderr, "Failed to initialize SDL2: %s\n", SDL_GetError());
        std::exit(EXIT_FAILURE);
    }
    return nullptr;
}

static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // 4:3 like the game, large enough for the menus (they scale with the window height, designed for 1080).
    window = SDL_CreateWindow("ADAM", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960, SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        std::exit(EXIT_FAILURE);
    }

    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    SDL_GetWindowWMInfo(window, &wm_info);
    main_window = wm_info.info.win.window;

    return ultramodern::renderer::WindowHandle{ wm_info.info.win.window, GetCurrentThreadId() };
}

// Fast-forward: while the Fast-Forward button is held in game, the VI rate - and so the game - runs at 2x if it's switched
// on in the General tab (runtime\patches\n64modernruntime-fast-forward.patch makes the rate adjustable; audio.cpp mutes
// meanwhile). Not while paused or while a menu is open.
// Rewind (rewind.cpp): while the Rewind button is held in a mission, if it's switched on in the General tab. Wins over
// fast-forward.
// Both buttons are remappable in the Controls tab (runtime\patches\recompfrontend-feature-inputs.patch; defaults in
// config.cpp: Tab / right trigger, Backspace / left stick click; any gamepad).
static bool rewind_held = false;

static bool feature_input_held(recompinput::GameInput input) {
    for (int profile : { recompinput::profiles::get_sp_keyboard_profile_index(), recompinput::profiles::get_sp_controller_profile_index() }) {
        if (profile < 0) {
            continue;
        }
        for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
            if (recompinput::get_input_digital(0, recompinput::profiles::get_input_binding(profile, input, i))) {
                return true;
            }
        }
    }
    return false;
}

static void check_rewind() {
    bool held = false;
    bool enabled = adam::get_rewind_enabled();
    adam::rewind::set_enabled(enabled);
    if (enabled && adam::rewind::available() && ultramodern::is_game_started() && !paused.load() && !recompui::is_context_capturing_input()) {
        held = feature_input_held(recompinput::GameInput::REWIND);
    }
    rewind_held = held;
    adam::rewind::set_held(held);
}

static void check_fast_forward() {
    bool held = false;
    if (adam::get_fast_forward_enabled() && ultramodern::is_game_started() && !paused.load() && !recompui::is_context_capturing_input() &&
        !rewind_held) {
        held = feature_input_held(recompinput::GameInput::FAST_FORWARD);
    }
    uint32_t speed = held ? 2 : 1;
    if (speed != ultramodern::get_speed_multiplier()) {
        ultramodern::set_speed_multiplier(speed);
        printf("[fast-forward] %ux at %.2f s\n", speed, game_seconds());
    }
}

// Every VI (VI thread): the gamepad motor follows the game, but stops while the game is held (P or a menu) or rewinding.
static void update_rumble() {
    adam::input::apply_rumble(paused.load() || recompui::is_context_capturing_input());
    recompinput::update_rumble();
}

// Test runs (scripts\run_game.ps1 sets it): with ADAM_STATE_FILE=<path>, the current state is written there as one JSON line
// five times a second, so tests can check what the game is doing instead of guessing it from screenshots.
// game_mode = u16 0x800D5114 (rewind.cpp lists known values; 0x2D/0x30 = mission battles), logic_running = u8 0x80149FC4
// (0 while the game's own pause screen is up), menu = a RecompFrontend menu is open, paused = P, aspect = the aspect ratio
// the renderer uses (0 Original, 1 Expand, 2 Manual; see check_aspect_ratio).
static void write_state_file() {
    static const char* path = std::getenv("ADAM_STATE_FILE");
    static ULONGLONG last = 0;
    if (path == nullptr || path[0] == '\0' || GetTickCount64() - last < 200) {
        return;
    }
    last = GetTickCount64();
    bool started = ultramodern::is_game_started() && dump_rdram != nullptr;
    char line[384];
    snprintf(line, sizeof(line),
        "{\"time\": %.2f, \"frame\": %u, \"game_started\": %s, \"game_mode\": %d, \"logic_running\": %d, \"menu\": %s, "
        "\"paused\": %s, \"speed\": %u, \"rewinding\": %s, \"aspect\": %d}\n",
        game_seconds(), frame_count.load(), started ? "true" : "false",
        started ? *(const uint16_t*)(dump_rdram + (0x0D5114 ^ 2)) : -1, started ? dump_rdram[0x149FC4 ^ 3] : -1,
        recompui::is_context_capturing_input() ? "true" : "false", paused.load() ? "true" : "false",
        ultramodern::get_speed_multiplier(), rewind_held ? "true" : "false",
        (int)ultramodern::renderer::get_graphics_config().ar_option);
    // Written to a temporary file and renamed, so a reader never sees half a line.
    std::filesystem::path target{ path };
    std::filesystem::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out{ tmp, std::ios::trunc };
        out << line;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
}

// Widescreen only in gameplay (user, 2026-10-12): the Graphics tab's aspect ratio (e.g. Expand) is applied while a mission's
// battle code runs (rewind.cpp in_gameplay: the mode's overlay is 1); everywhere else - intro, menus, cutscenes, briefings,
// the launcher - the renderer gets Original, since those scenes are drawn for 4:3 (2D backgrounds and sprites don't fill a
// wider screen). The saved setting is never changed: only the copy the renderer uses (ultramodern's graphics config, which
// the Graphics tab rebuilds from the saved values whenever it applies a change - then this corrects it again).
static void check_aspect_ratio() {
    using ultramodern::renderer::AspectRatio;
    static bool ready = false;
    if (!ready) {
        // The Graphics tab exists once the menus are set up (it throws before that).
        try {
            recompui::config::get_graphics_config();
            ready = true;
        } catch (...) {
            return;
        }
    }
    auto chosen = static_cast<AspectRatio>(std::get<uint32_t>(
        recompui::config::get_graphics_config().get_option_value(recompui::config::graphics::options::ar_option)));
    bool gameplay = ultramodern::is_game_started() && adam::rewind::in_gameplay();
    AspectRatio wanted = gameplay ? chosen : AspectRatio::Original;
    ultramodern::renderer::GraphicsConfig config = ultramodern::renderer::get_graphics_config();
    if (config.ar_option != wanted) {
        config.ar_option = wanted;
        ultramodern::renderer::set_graphics_config(config);
        printf("[aspect] %s at %.2f s\n", wanted == AspectRatio::Original ? "Original" : wanted == AspectRatio::Expand ? "Expand" : "Manual",
            game_seconds());
    }
}

// Reset Game (config.cpp "Game" tab): the runtime can't restart the recompiled game in place, so quit cleanly (the save
// thread writes the save, as on Exit) and start adam.exe again with --game adam after recomp::start returns (main).
static std::atomic<bool> restart_requested = false;

namespace adam {
    void request_reset() {
        printf("[reset] restarting the game at %.2f s\n", game_seconds());
        restart_requested = true;
        ultramodern::quit();
    }
}

static void restart_self() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path exe_path{ exe };
    std::wstring command = L"\"" + exe_path.wstring() + L"\" --game adam";
    std::wstring dir = exe_path.parent_path().wstring();
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else {
        fprintf(stderr, "[reset] could not start %ls (error %lu)\n", exe, GetLastError());
    }
}

// Called by recomp::start on the main thread every millisecond.
static void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    recompinput::handle_events();
    check_rewind();
    check_fast_forward();
    check_pause_key();
    check_ram_dump_key();
    update_title();
    write_state_file();
    check_aspect_ratio();
    adam::bezels::update();
}

// --- Launcher -----------------------------------------------------------------------------------

// RecompFrontend's launcher reads this global (as in Banjo: Recompiled).
std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = rom_hash,
        // Only rom_hash validates the ROM; internal_name just picks a more specific error message.
        .internal_name = "ADAM",
        .display_name = "ADAM",
        .game_id = game_id,
        .mod_game_id = mod_game_id,
        .save_type = recomp::SaveType::AllowAll,
        .is_enabled = true,
        .entrypoint_address = get_entrypoint_address(),
#ifdef ADAM_LIVE
        .entrypoint = adam::live::entrypoint,
        .on_init_callback = adam::live::on_init,
#else
        .entrypoint = recomp_entrypoint,
        .on_init_callback = register_sections,
#endif
    },
};

// Background: a picture from the game, chosen in the "Background" entry / settings tab (src\bezels.cpp: the same picker and
// adjustments as the bezels; data\launcher_background.json; default D02END20). The pictures are decoded from the stored ROM
// (src\launcher_art.cpp); without a stored ROM the launcher keeps RecompFrontend's default look until Load ROM stores one.
static void on_launcher_init(recompui::LauncherMenu* menu) {
    adam::bezels::update_launcher(menu->get_background_container());
    recompui::GameOptionsMenu* options = menu->init_game_options_menu(game_id, mod_game_id, "ADAM", {});
    recompui::update_game_mod_id(mod_game_id);
    options->add_start_game_or_load_rom_option();
    options->add_setup_controls_option();
    options->add_settings_option();
    options->add_option("Background", []() { adam::bezels::open_launcher_background_settings(); });
    options->add_exit_option();
}

// Every frame while the launcher shows: applies background changes (and shows it right after the first Load ROM).
static void on_launcher_update(recompui::LauncherMenu* menu) {
    adam::bezels::update_launcher(menu->get_background_container());
}

// --- Command line -------------------------------------------------------------------------------

static std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file{ path, std::ios::binary };
    return std::vector<uint8_t>{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

static int print_hash(const char* path) {
    std::vector<uint8_t> rom = read_file(path);
    if (rom.size() < 0x40) {
        fprintf(stderr, "Could not read %s\n", path);
        return EXIT_FAILURE;
    }
    printf("hash: 0x%016llXULL\n", (unsigned long long)XXH3_64bits(rom.data(), rom.size()));
    printf("name: \"%.20s\"\n", (const char*)rom.data() + 0x20);
    return EXIT_SUCCESS;
}

static const char* rom_error_name(recomp::RomValidationError err) {
    switch (err) {
        case recomp::RomValidationError::Good: return "Good";
        case recomp::RomValidationError::FailedToOpen: return "FailedToOpen";
        case recomp::RomValidationError::NotARom: return "NotARom";
        case recomp::RomValidationError::IncorrectRom: return "IncorrectRom";
        case recomp::RomValidationError::NotYet: return "NotYet";
        case recomp::RomValidationError::IncorrectVersion: return "IncorrectVersion";
        default: return "OtherError";
    }
}

// On a crash: print where it happened and write crash.dmp (all threads' stacks; open with
// `lldb adam.exe -c crash.dmp` -> `thread backtrace all`) into the working directory (data\).
static LONG WINAPI on_crash(EXCEPTION_POINTERS* info) {
    void* addr = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "(no module: JIT code?)";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &module)) {
        GetModuleFileNameA(module, name, sizeof(name));
    }
    fprintf(stderr, "[crash] exception 0x%08lX at %p (%s +0x%llX), thread %lu\n", info->ExceptionRecord->ExceptionCode, addr,
        name, module ? (unsigned long long)((uintptr_t)addr - (uintptr_t)module) : 0ull, GetCurrentThreadId());
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        fprintf(stderr, "[crash] %s address 0x%llX\n", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
    }
#ifdef ADAM_LIVE
    uint32_t vram, offset;
    if (module == nullptr && adam::live::find_function(addr, &vram, &offset)) {
        fprintf(stderr, "[crash] in compiled game function 0x%08X + 0x%X; code bytes from -0x40:\n[crash]", vram, offset);
        for (int i = -0x40; i < 0x20; i++) {
            fprintf(stderr, " %02X", ((const uint8_t*)addr)[i]);
        }
        fprintf(stderr, "\n");
    }
#endif
    HANDLE file = CreateFileA("crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{ GetCurrentThreadId(), info, FALSE };
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithThreadInfo, &exception, nullptr, nullptr);
        CloseHandle(file);
        fprintf(stderr, "[crash] wrote crash.dmp\n");
    }
    // Game memory too (8 MB, big-endian like F9's dumps): the minidump doesn't contain it.
    if (dump_rdram != nullptr) {
        FILE* f = fopen("crash_rdram.bin", "wb");
        if (f != nullptr) {
            for (uint32_t i = 0; i < 8 * 1024 * 1024; i++) {
                fputc(dump_rdram[i ^ 3], f);
            }
            fclose(f);
            fprintf(stderr, "[crash] wrote crash_rdram.bin\n");
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#ifdef ADAM_LIVE
// Debug (ADAM_WATCHDOG=1, live build): when the frame callback hasn't started for 5 s while the game runs (no pause, no
// menu), print each thread's game functions: return addresses on its native stack that are inside compiled game code
// (nearest function start; JIT frames have no unwind info, so this is a scan, not an exact backtrace).
extern "C" std::atomic<uint64_t> adam_frame_hook_calls; // rewind.cpp
static void print_game_stacks() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        SuspendThread(thread);
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        std::vector<uint32_t> funcs;
        std::vector<uintptr_t> natives; // offsets into adam.exe (resolve with llvm-symbolizer --obj=adam.exe)
        static const uintptr_t exe_base = (uintptr_t)GetModuleHandleA(nullptr);
        static const uintptr_t exe_end = exe_base + ((IMAGE_NT_HEADERS*)(exe_base + ((IMAGE_DOS_HEADER*)exe_base)->e_lfanew))->OptionalHeader.SizeOfImage;
        if (GetThreadContext(thread, &ctx)) {
            if (ctx.Rip >= exe_base && ctx.Rip < exe_end) {
                natives.push_back(ctx.Rip - exe_base);
            }
            {
                MEMORY_BASIC_INFORMATION m{};
                VirtualQuery((void*)ctx.Rsp, &m, sizeof(m));
                uintptr_t e = (uintptr_t)m.BaseAddress + m.RegionSize;
                for (uintptr_t p = ctx.Rsp; p + 8 <= e && natives.size() < 16; p += 8) {
                    uintptr_t v = *(uintptr_t*)p;
                    if (v >= exe_base && v < exe_end) {
                        natives.push_back(v - exe_base);
                    }
                }
            }
            uint32_t vram, offset;
            if (adam::live::find_function((void*)ctx.Rip, &vram, &offset)) {
                funcs.push_back(vram);
            }
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery((void*)ctx.Rsp, &mbi, sizeof(mbi));
            uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            for (uintptr_t p = ctx.Rsp; p + 8 <= end && funcs.size() < 24; p += 8) {
                if (adam::live::find_function(*(void**)p, &vram, &offset) && offset != 0 && (funcs.empty() || funcs.back() != vram)) {
                    funcs.push_back(vram);
                }
            }
        }
        ResumeThread(thread);
        CloseHandle(thread);
        if (!funcs.empty()) {
            printf("[watchdog] thread %lu:", te.th32ThreadID);
            for (uint32_t f : funcs) {
                printf(" %08X", f);
            }
            printf("\n[watchdog]   native:");
            for (uintptr_t n : natives) {
                printf(" 0x%llX", (unsigned long long)n);
            }
            printf("\n");
        }
    }
    CloseHandle(snap);
}

static void start_watchdog() {
    const char* env = std::getenv("ADAM_WATCHDOG");
    if (env == nullptr || env[0] != '1') {
        return;
    }
    std::thread([] {
        uint64_t last = 0;
        int quiet = 0;
        while (true) {
            Sleep(1000);
            uint64_t calls = adam_frame_hook_calls.load();
            bool running = ultramodern::is_game_started() && !paused.load() && !recompui::is_context_capturing_input();
            quiet = (calls == last && running && calls != 0) ? quiet + 1 : 0;
            last = calls;
            if (quiet == 5 || quiet == 20) {
                printf("[watchdog] no frame callback for %d s (game time %.2f s)\n", quiet, game_seconds());
                print_game_stacks();
            }
        }
    }).detach();
}
#endif

int main(int argc, char** argv) {
    // Unbuffered output, so logs survive when the process is killed while redirected to a file.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(on_crash);
#ifdef ADAM_LIVE
    start_watchdog();
#endif

    // ROM paths are made absolute before switching to the exe's folder below.
    std::string rom_path_abs;
    const char* rom_path = nullptr;
    std::string live_test_rom;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hash") == 0 && i + 1 < argc) {
            return print_hash(argv[i + 1]);
        }
#ifndef ADAM_LIVE
        if (strcmp(argv[i], "--rsp-test") == 0 && i + 1 < argc) {
            return adam_rsp_test(argc - i - 1, argv + i + 1);
        }
#endif
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path_abs = std::filesystem::absolute(argv[++i]).string();
            rom_path = rom_path_abs.c_str();
        }
        if (strcmp(argv[i], "--live-test") == 0 && i + 1 < argc) {
            live_test_rom = std::filesystem::absolute(argv[++i]).string();
        }
    }

    // The menus' assets\ and the live shell's live\ are built into the exe: unpack them into the "data" folder next to the
    // exe (which also holds the stored ROM copy, saves and settings) and work from there, since RecompFrontend loads
    // assets\ relative to the working directory.
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::filesystem::path exe_dir = std::filesystem::path{ exe_path }.parent_path();
    std::filesystem::path data_path = exe_dir / "data";
    std::filesystem::create_directories(data_path);
    if (!adam::unpack_embedded_files(data_path)) {
        MessageBoxA(nullptr, "Could not write ADAM's files into the data folder next to adam.exe.\n"
            "Move adam.exe to a folder you can write to (not Program Files).", "ADAM", MB_ICONERROR);
        return EXIT_FAILURE;
    }
    std::filesystem::current_path(data_path);

#ifdef ADAM_LIVE
    // `adam.exe --live-test <rom.z64>`: recompile all game code from the ROM and report, without opening a window.
    if (!live_test_rom.empty()) {
        return adam::live::test(live_test_rom.c_str());
    }
#endif

    // 1ms timer resolution, as Zelda64Recomp does.
    timeBeginPeriod(1);
    NFD_Init();

    recompui::programconfig::set_program_name("ADAM");
    recompui::programconfig::set_program_id(game_id);

    adam::audio::init(48000);
    adam::rewind::init();

    const adam::EmbeddedFile* controller_db = adam::find_embedded_file("recompcontrollerdb.txt");
    if (SDL_GameControllerAddMappingsFromRW(SDL_RWFromConstMem(controller_db->data, (int)controller_db->size), 1) < 0) {
        fprintf(stderr, "Failed to load controller mappings: %s\n", SDL_GetError());
    }

    recompui::register_primary_font("InterVariable.ttf", "Inter Variable");

    recomp::register_config_path(data_path);

    for (const recomp::GameEntry& game : supported_games) {
        recomp::register_game(game);
    }

    if (rom_path != nullptr) {
        recomp::RomValidationError err = recomp::select_rom(rom_path, game_id);
        if (err != recomp::RomValidationError::Good) {
            fprintf(stderr, "ROM rejected: %s (check rom_hash in main.cpp with --hash)\n", rom_error_name(err));
            return EXIT_FAILURE;
        }
        printf("ROM stored in %s\n", data_path.string().c_str());
    }

#ifdef ADAM_LIVE
    if (!adam::live::register_tables()) {
        recompui::message_box("data\\live\\symbols.toml is broken: delete the data\\live folder and start ADAM again.");
        return EXIT_FAILURE;
    }
#else
    register_overlay_table();
#endif

    adam::input::init();
    adam::init_config();
    recompui::register_launcher_init_callback(on_launcher_init);
    recompui::register_launcher_update_callback(on_launcher_update);

    recomp::Configuration cfg{};
    // librecomp's CLI parser handles "--game adam" (start right away, skipping the launcher).
    cfg.argc = argc;
    cfg.argv = argv;
    // From runtime\package\VERSION (CMakeLists.txt).
    cfg.project_version = recomp::Version{ ADAM_VERSION_MAJOR, ADAM_VERSION_MINOR, ADAM_VERSION_PATCH };
    cfg.rsp_callbacks = recomp::rsp::callbacks_t{ .get_rsp_microcode = get_rsp_microcode };
    cfg.audio_callbacks = adam::audio::callbacks();
    cfg.input_callbacks = adam::input::callbacks();
    cfg.renderer_callbacks = ultramodern::renderer::callbacks_t{ .create_render_context = create_render_context };
    cfg.gfx_callbacks = ultramodern::gfx_callbacks_t{ .create_gfx = create_gfx, .create_window = create_window, .update_gfx = update_gfx };
    cfg.events_callbacks = ultramodern::events::callbacks_t{ .vi_callback = update_rumble };
    cfg.error_handling_callbacks = ultramodern::error_handling::callbacks_t{ .message_box = recompui::message_box };

    recomp::start(cfg);

    NFD_Quit();
    timeEndPeriod(1);
    if (restart_requested.load()) {
        restart_self();
    }
    return EXIT_SUCCESS;
}
