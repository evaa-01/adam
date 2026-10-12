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
#include "recompui/renderer.h"
#include "recompinput/recompinput.h"
#include "recompinput/input_events.h"
#include "recompinput/input_state.h"
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
}

namespace adam {
    void init_config();
    uint32_t get_fast_forward_speed();
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
    while (paused.load() || recompui::is_any_context_shown()) {
        Sleep(5);
    }
}

static std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    dump_rdram = rdram;
    // SkipBuffering is RT64's default, which the 30 fps pacing (tag pacing-30fps) was measured with.
    return recompui::renderer::create_render_context(rdram, window_handle, ultramodern::renderer::PresentationMode::SkipBuffering, developer_mode);
}

static bool debug_keys_active() {
    return ultramodern::is_game_started() && !recompui::is_any_context_shown() && GetForegroundWindow() == main_window;
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
        if (std::filesystem::is_directory(dir / ".." / ".." / "dumps")) {
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

// Fast-forward: while the right trigger (any gamepad) or Tab is held in game, the VI rate - and so the game - runs at
// the General tab's speed (runtime\patches\n64modernruntime-fast-forward.patch makes the rate adjustable; audio.cpp
// mutes meanwhile). Not while paused or while a menu is open.
static void check_fast_forward() {
    bool held = false;
    if (ultramodern::is_game_started() && !paused.load() && !recompui::is_any_context_shown()) {
        held = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0 && GetForegroundWindow() == main_window;
        for (int i = 0; i < SDL_NumJoysticks() && !held; i++) {
            SDL_GameController* controller = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
            if (controller != nullptr && SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) {
                held = true;
            }
        }
    }
    uint32_t speed = held ? adam::get_fast_forward_speed() : 1;
    if (speed != ultramodern::get_speed_multiplier()) {
        ultramodern::set_speed_multiplier(speed);
        printf("[fast-forward] %ux at %.2f s\n", speed, game_seconds());
    }
}

// Called by recomp::start on the main thread every millisecond.
static void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    recompinput::handle_events();
    check_fast_forward();
    check_pause_key();
    check_ram_dump_key();
    update_title();
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

// Background: a picture from the game, decoded from the stored ROM (src\launcher_art.cpp). Without a stored ROM the
// launcher keeps RecompFrontend's default look until Load ROM stores one (on_launcher_update adds it then).
static bool launcher_background_tried = false;

static void add_launcher_background(recompui::LauncherMenu* menu) {
    // librecomp only rechecks stored ROMs at startup (is_rom_valid stays false after Load ROM): wait for the file.
    std::error_code ec;
    if (!std::filesystem::exists(recomp::get_config_path() / "adam.z64", ec)) {
        return;
    }
    launcher_background_tried = true;
    std::vector<char> rgba;
    uint32_t width, height;
    if (!adam::launcher_art::load_background(rgba, width, height)) {
        return;
    }
    recompui::queue_image_from_bytes_rgba32("adam_launcher_background", rgba, width, height);
    recompui::ContextId context = recompui::get_current_context();
    // Scaled to the window's width and centered vertically (like set_launcher_background_svg): fills the window and
    // crops top and bottom in a wide window.
    recompui::Element* image = context.create_element<recompui::Image>(menu->get_background_container(), "adam_launcher_background");
    image->set_position(recompui::Position::Absolute);
    image->set_top(50.0f, recompui::Unit::Percent);
    image->set_left(0);
    image->set_width(100.0f, recompui::Unit::Percent);
    image->set_height_auto();
    image->set_translate_2D(0.0f, -50.0f, recompui::Unit::Percent);
}

static void on_launcher_init(recompui::LauncherMenu* menu) {
    add_launcher_background(menu);
    recompui::GameOptionsMenu* options = menu->init_game_options_menu(game_id, mod_game_id, "ADAM", {});
    recompui::update_game_mod_id(mod_game_id);
    options->add_start_game_or_load_rom_option();
    options->add_setup_controls_option();
    options->add_settings_option();
    options->add_exit_option();
}

// Every frame while the launcher shows: after the first Load ROM, add the background right away.
static void on_launcher_update(recompui::LauncherMenu* menu) {
    if (!launcher_background_tried) {
        add_launcher_background(menu);
    }
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

int main(int argc, char** argv) {
    // Unbuffered output, so logs survive when the process is killed while redirected to a file.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

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
    cfg.project_version = recomp::Version{ 0, 1, 0 };
    cfg.rsp_callbacks = recomp::rsp::callbacks_t{ .get_rsp_microcode = get_rsp_microcode };
    cfg.audio_callbacks = adam::audio::callbacks();
    cfg.input_callbacks = adam::input::callbacks();
    cfg.renderer_callbacks = ultramodern::renderer::callbacks_t{ .create_render_context = create_render_context };
    cfg.gfx_callbacks = ultramodern::gfx_callbacks_t{ .create_gfx = create_gfx, .create_window = create_window, .update_gfx = update_gfx };
    cfg.events_callbacks = ultramodern::events::callbacks_t{ .vi_callback = recompinput::update_rumble };
    cfg.error_handling_callbacks = ultramodern::error_handling::callbacks_t{ .message_box = recompui::message_box };

    recomp::start(cfg);

    NFD_Quit();
    timeEndPeriod(1);
    return EXIT_SUCCESS;
}
