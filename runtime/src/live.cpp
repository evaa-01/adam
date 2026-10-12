// Live shell (CMake option ADAM_LIVE): adam.exe without the recompiled game code. The game's MIPS code is recompiled
// from the player's ROM at game start with N64Recomp's live recompiler (LiveRecomp/sljit, the same one librecomp
// uses for mods). Inputs built into the exe and unpacked to data\live\: symbols.toml (function addresses, sizes and names; no
// code) and config.toml (stubs).
//
// How it differs from the static build:
// - Every call goes through the runtime's function lookup (use_lookup_for_all_function_calls): live code can't
//   call functions by name. Library functions the runtime provides (live_natives.inc) are entries in the same
//   function tables, as in the static build's recomp_overlays.inl.
// - librecomp builds its function map from the tables before our init callback runs (load_stored_rom -> init ->
//   on_init_callback -> entrypoint), but the code only exists once the ROM is loaded. So the tables are registered
//   at startup with placeholder pointers (structure from symbols.toml, compiled against a blank ROM), and on_init
//   compiles the real code, fills the pointers in and reloads the sections.
// - scripts\extra_hooks.txt inserts C code into the generated C; live code has no C. Each hook's instruction is
//   replaced by a `syscall` and done in syscall_handler below (the replaced instruction is redone there); the
//   audio pacing hook becomes a replacement for osAiGetStatus (its only caller is the audio thread).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "recomp.h"
#include "librecomp/sections.h"
#include "librecomp/overlays.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/ultra64.h"
#include "recompiler/context.h"
#include "recompiler/live_recompiler.h"

#include "live_natives.inc"

extern "C" void yield_self(uint8_t* rdram);
extern "C" void cop0_status_write(recomp_context* ctx, gpr value);
extern "C" gpr cop0_status_read(recomp_context* ctx);
extern "C" void switch_error(const char* func, uint32_t vram, uint32_t jtbl);
extern "C" uint32_t adam_ai_status(void); // audio.cpp
extern "C" void adam_on_sound(uint8_t* rdram, recomp_context* ctx); // subtitles.cpp
uint32_t adam_frame_count(); // main.cpp

namespace {
    constexpr uint32_t ENTRY_VRAM = 0x80096400;
    constexpr uint32_t BOOT_ROM = 0x1000;
    constexpr uint32_t OVT_SLOT_VRAM = 0x80025C00;
    constexpr size_t ROM_SIZE = 32 * 1024 * 1024;
    const char* SYMBOLS_PATH = "live/symbols.toml";
    const char* CONFIG_PATH = "live/config.toml";

    // Function tables handed to librecomp (they must not move after registration).
    std::vector<std::vector<FuncEntry>> section_funcs;
    std::vector<std::vector<size_t>> section_func_indices; // context function index per FuncEntry (-1 = native)
    std::vector<SectionTableEntry> section_table;
    int overlays_by_index[] = { -1 };
    size_t total_sections = 0;

    N64Recomp::LiveGeneratorOutput live_output;
    recomp_func_t* entry_func = nullptr;

    void not_compiled(uint8_t*, recomp_context*) {
        fprintf(stderr, "[live] called a function before the game code was compiled\n");
        std::abort();
    }

    // --- Hooks (see scripts\extra_hooks.txt for the static build's versions) ---------------------------------

    struct HookPatch { uint32_t vram; uint32_t original; };
    constexpr HookPatch hook_patches[] = {
        { 0x800AD8A8, 0x3C028017 }, // idle loop head: lui $v0, 0x8017
        { 0x800B6880, 0x3C028017 }, // gfx-pending wait loop head: lui $v0, 0x8017
        { 0x800A8B04, 0x1040FFFF }, // main thread end: beqz $v0, .
        { 0x800AB7A0, 0x8FBF002C }, // overlay loader epilogue: lw $ra, 0x2C($sp)
        { 0x8009B830, 0x3C028017 }, // sound/voice playback func_8009B830 entry: lui $v0, 0x8017
    };

    void syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
        switch ((uint32_t)instruction_vram) {
            case 0x800AD8A8: // idle thread loop never yields by itself
                yield_self(rdram);
                ctx->r2 = (gpr)(int32_t)0x80170000;
                break;
            case 0x800B6880: // "while (gfx_task_pending) {}": yield while the flag at 0x8016D060 is set
                if (MEM_W(0, (gpr)(int32_t)0x8016D060) != 0) {
                    yield_self(rdram);
                }
                ctx->r2 = (gpr)(int32_t)0x80170000;
                break;
            case 0x800A8B04: // "for (;;);" when the flag is 0 (always, as on hardware): park the main thread
                if (ctx->r2 == 0) {
                    pause_self(rdram);
                }
                break;
            case 0x800AB7A0: // overlay loaded into the shared slot: $s6 = ROM, $s4 = VRAM, $s2 = size
                unload_overlays((int32_t)OVT_SLOT_VRAM, 0x5F1D0);
                load_overlays((uint32_t)ctx->r22, (int32_t)ctx->r20, (uint32_t)ctx->r18);
                printf("[overlay] loaded ROM 0x%06X size 0x%05X at 0x%08X\n", (unsigned)ctx->r22, (unsigned)ctx->r18, (unsigned)ctx->r20);
                ctx->r31 = MEM_W(0x2C, ctx->r29);
                break;
            case 0x8009B830: // a sound or voice clip starts (subtitles.cpp)
                adam_on_sound(rdram, ctx);
                ctx->r2 = (gpr)(int32_t)0x80170000;
                break;
            default:
                fprintf(stderr, "[live] unexpected syscall at 0x%08X\n", (unsigned)instruction_vram);
                break;
        }
    }

    // Call trace for research (live build only): ADAM_TRACE=<lo>-<hi> (hex vram, e.g. 80000000-80400000) logs the
    // first 20 calls of every function in that range with the frame number, e.g. to see which code runs when a voice
    // line starts. Every jal/jalr of the compiled code goes through get_function, so nothing is missed (calls from
    // the runtime itself, like thread starts, aren't seen).
    uint32_t trace_lo = 0, trace_hi = 0;
    std::unique_ptr<std::atomic<uint8_t>[]> trace_counts;

    recomp_func_t* traced_get_function(int32_t vram) {
        uint32_t addr = (uint32_t)vram;
        if (addr >= trace_lo && addr < trace_hi) {
            std::atomic<uint8_t>& count = trace_counts[(addr - trace_lo) / 4];
            if (count.load(std::memory_order_relaxed) < 20 && count.fetch_add(1) < 20) {
                printf("[trace] frame %u: 0x%08X\n", adam_frame_count(), addr);
            }
        }
        return get_function(vram);
    }

    void init_trace() {
        const char* range = std::getenv("ADAM_TRACE");
        if (range == nullptr || std::sscanf(range, "%x-%x", &trace_lo, &trace_hi) != 2 || trace_hi <= trace_lo
            || trace_hi - trace_lo > 0x1000000) {
            trace_lo = trace_hi = 0;
            return;
        }
        trace_counts = std::make_unique<std::atomic<uint8_t>[]>((trace_hi - trace_lo) / 4);
        printf("[trace] logging calls to 0x%08X-0x%08X\n", trace_lo, trace_hi);
    }

    // Audio pacing (static build: hook at 0x800B26C8 in func_800B2658, the only caller of osAiGetStatus): report the
    // FIFO as full while the host queue is full and wait at a low priority (see CLAUDE.md "Audio").
    void adam_osAiGetStatus(uint8_t* rdram, recomp_context* ctx) {
        uint32_t status = adam_ai_status();
        if (status != 0) {
            OSPri pri = osGetThreadPri(rdram, NULLPTR);
            osSetThreadPri(rdram, NULLPTR, 11);
            yield_self(rdram);
            osSetThreadPri(rdram, NULLPTR, pri);
        }
        ctx->r2 = (gpr)(int32_t)status;
    }

    // --- Context ------------------------------------------------------------------------------------------------

    std::vector<std::string> read_stubs() {
        std::ifstream file{ CONFIG_PATH };
        std::stringstream text;
        text << file.rdbuf();
        std::string s = text.str();
        std::vector<std::string> stubs;
        size_t start = s.find("stubs = [");
        if (start == std::string::npos) {
            return stubs;
        }
        size_t end = s.find(']', start);
        std::regex name_re("\"([^\"]+)\"");
        std::string list = s.substr(start, end - start);
        for (auto it = std::sregex_iterator(list.begin(), list.end(), name_re); it != std::sregex_iterator(); ++it) {
            stubs.push_back((*it)[1]);
        }
        return stubs;
    }

    // Loads symbols.toml against `rom` and applies what N64Recomp's main.cpp does in symbol-file mode.
    bool load_context(std::vector<uint8_t>&& rom, N64Recomp::Context& context) {
        if (!N64Recomp::Context::from_symbol_file(SYMBOLS_PATH, std::move(rom), context, true)) {
            fprintf(stderr, "[live] could not load %s\n", SYMBOLS_PATH);
            return false;
        }
        auto rename = [&context](size_t index, const std::string& name) {
            N64Recomp::Function& func = context.functions[index];
            context.functions_by_name.erase(func.name);
            func.name = name;
            context.functions_by_name[func.name] = index;
        };
        for (size_t i = 0; i < context.functions.size(); i++) {
            N64Recomp::Function& func = context.functions[i];
            if (N64Recomp::reimplemented_funcs.contains(func.name)) {
                rename(i, func.name + "_recomp");
                func.reimplemented = true;
                func.ignored = true;
            }
            else if (N64Recomp::ignored_funcs.contains(func.name)) {
                rename(i, func.name + "_recomp");
                func.ignored = true;
            }
            else if (N64Recomp::renamed_funcs.contains(func.name)) {
                rename(i, func.name + "_recomp");
            }
        }
        for (uint32_t index : context.functions_by_vram[ENTRY_VRAM]) {
            if (context.functions[index].rom == BOOT_ROM) {
                rename(index, "recomp_entrypoint");
            }
        }
        for (const std::string& stub : read_stubs()) {
            auto it = context.functions_by_name.find(stub);
            if (it != context.functions_by_name.end()) {
                context.functions[it->second].stubbed = true;
            }
        }
        context.use_lookup_for_all_function_calls = true;
        return true;
    }

    // Replaces each hook instruction with a syscall (in every function that contains it: overlapping functions
    // translate shared code twice). Returns false if an instruction isn't the expected one (different ROM?).
    bool patch_hooks(N64Recomp::Context& context) {
        for (const HookPatch& patch : hook_patches) {
            int found = 0;
            for (N64Recomp::Function& func : context.functions) {
                if (func.rom < BOOT_ROM || patch.vram < func.vram || patch.vram >= func.vram + func.words.size() * 4) {
                    continue;
                }
                if (context.sections[func.section_index].rom_addr != BOOT_ROM) {
                    continue;
                }
                uint32_t& word = func.words[(patch.vram - func.vram) / 4];
                if (byteswap(word) != patch.original) {
                    fprintf(stderr, "[live] hook at 0x%08X: found 0x%08X, expected 0x%08X\n", patch.vram, byteswap(word), patch.original);
                    return false;
                }
                word = byteswap(0x0000000Cu); // syscall
                found++;
            }
            if (found == 0) {
                fprintf(stderr, "[live] hook at 0x%08X: no function contains it\n", patch.vram);
                return false;
            }
        }
        return true;
    }

    std::string strip_recomp(const std::string& name) {
        constexpr std::string_view suffix = "_recomp";
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            return name.substr(0, name.size() - suffix.size());
        }
        return name;
    }

    void stub_func(uint8_t* rdram, recomp_context* ctx) {}

    recomp_func_t* native_for(const std::string& name) {
        static const std::unordered_map<std::string, recomp_func_t*> natives = [] {
            std::unordered_map<std::string, recomp_func_t*> m;
            for (const auto& [n, f] : live_natives) {
                m.emplace(n, f);
            }
            m["osAiGetStatus"] = adam_osAiGetStatus;
            return m;
        }();
        auto it = natives.find(strip_recomp(name));
        return it == natives.end() ? nullptr : it->second;
    }

    // Builds the tables (like N64Recomp's recomp_overlays.inl). Compiled functions get placeholders for now.
    void build_tables(const N64Recomp::Context& context) {
        total_sections = context.sections.size();
        section_funcs.assign(total_sections, {});
        section_func_indices.assign(total_sections, {});
        for (size_t s = 0; s < total_sections; s++) {
            const N64Recomp::Section& section = context.sections[s];
            for (size_t index : context.section_functions[s]) {
                const N64Recomp::Function& func = context.functions[index];
                // Stubs return at once. They aren't given to LiveRecomp: for a stubbed function it emits only the
                // entry and no return, so a call ran on into whatever code came next.
                recomp_func_t* native = func.stubbed ? stub_func : native_for(func.name);
                if (native != nullptr) {
                    section_funcs[s].push_back({ .func = native, .offset = func.rom - section.rom_addr, .rom_size = 0 });
                    section_func_indices[s].push_back((size_t)-1);
                }
                else if (!func.name.empty() && !func.ignored && !func.words.empty()) {
                    section_funcs[s].push_back({ .func = not_compiled, .offset = func.rom - section.rom_addr, .rom_size = (uint32_t)(func.words.size() * 4) });
                    section_func_indices[s].push_back(index);
                }
            }
        }
        section_table.clear();
        for (size_t s = 0; s < total_sections; s++) {
            const N64Recomp::Section& section = context.sections[s];
            if (section_funcs[s].empty()) {
                continue;
            }
            section_table.push_back({ .rom_addr = section.rom_addr, .ram_addr = section.ram_addr, .size = section.size,
                .funcs = section_funcs[s].data(), .num_funcs = section_funcs[s].size(), .relocs = nullptr, .num_relocs = 0, .index = s });
        }
    }

    // Compiles every game function from the real ROM and puts the code into the tables.
    bool compile(std::span<const uint8_t> rom_span) {
        auto t0 = std::chrono::steady_clock::now();
        N64Recomp::Context context;
        if (!load_context(std::vector<uint8_t>(rom_span.begin(), rom_span.end()), context) || !patch_hooks(context)) {
            return false;
        }
        std::vector<int32_t> section_addresses(context.sections.size());
        for (size_t s = 0; s < context.sections.size(); s++) {
            section_addresses[s] = (int32_t)context.sections[s].ram_addr;
        }
        init_trace();
        N64Recomp::live_recompiler_init();
        N64Recomp::LiveGeneratorInputs inputs{
            .base_event_index = 0,
            .cop0_status_write = cop0_status_write,
            .cop0_status_read = cop0_status_read,
            .switch_error = switch_error,
            .do_break = do_break,
            .get_function = trace_hi != 0 ? traced_get_function : get_function,
            .syscall_handler = syscall_handler,
            .pause_self = pause_self,
            .trigger_event = nullptr,
            .reference_section_addresses = nullptr,
            .local_section_addresses = section_addresses.data(),
        };
        N64Recomp::LiveGenerator generator{ context.functions.size(), inputs };
        std::vector<std::vector<uint32_t>> static_funcs(context.sections.size());
        size_t compiled = 0;
        for (size_t s = 0; s < section_func_indices.size(); s++) {
            for (size_t index : section_func_indices[s]) {
                if (index == (size_t)-1) {
                    continue;
                }
                std::ostringstream dummy{};
                if (!N64Recomp::recompile_function_live(generator, context, index, dummy, static_funcs, true)) {
                    fprintf(stderr, "[live] failed to recompile %s (0x%08X)\n", context.functions[index].name.c_str(), context.functions[index].vram);
                    return false;
                }
                compiled++;
            }
        }
        live_output = generator.finish();
        if (!live_output.good) {
            fprintf(stderr, "[live] code generation failed\n");
            return false;
        }
        for (size_t s = 0; s < section_func_indices.size(); s++) {
            for (size_t e = 0; e < section_func_indices[s].size(); e++) {
                size_t index = section_func_indices[s][e];
                if (index != (size_t)-1) {
                    section_funcs[s][e].func = live_output.functions[index];
                }
            }
        }
        for (uint32_t index : context.functions_by_vram[ENTRY_VRAM]) {
            if (context.functions[index].rom == BOOT_ROM) {
                entry_func = live_output.functions[index];
            }
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("[live] recompiled %zu functions in %.0f ms (%zu KB of code)\n", compiled, ms, live_output.code_size / 1024);
        return entry_func != nullptr;
    }
}

namespace adam::live {
    // At startup: tables from symbols.toml (blank ROM, so the structure is right but nothing is compiled yet).
    bool register_tables() {
        N64Recomp::Context context;
        if (!load_context(std::vector<uint8_t>(ROM_SIZE, 0), context)) {
            return false;
        }
        build_tables(context);
        recomp::overlays::overlay_section_table_data_t sections{
            .code_sections = section_table.data(),
            .num_code_sections = section_table.size(),
            .total_num_sections = total_sections,
        };
        recomp::overlays::overlays_by_index_t overlays{ .table = overlays_by_index, .len = 1 };
        recomp::overlays::register_overlays(sections, overlays);
        return true;
    }

    // on_init_callback: compile from the loaded ROM, then rebuild librecomp's function map: init() already loaded
    // the first 1MB with placeholders. .text is reloaded; the other sections as in the static build's
    // register_sections (main.cpp): everything except the shared overlay slot, which the loader hook fills.
    void on_init(uint8_t* rdram, recomp_context* ctx) {
        if (!compile(recomp::get_rom())) {
            fprintf(stderr, "[live] could not recompile the game code from this ROM\n");
            std::abort();
        }
        unload_overlays((int32_t)ENTRY_VRAM, 0x100000);
        for (const SectionTableEntry& section : section_table) {
            if (section.ram_addr != OVT_SLOT_VRAM) {
                load_overlays(section.rom_addr, (int32_t)section.ram_addr, section.size);
            }
        }
    }

    void entrypoint(uint8_t* rdram, recomp_context* ctx) {
        entry_func(rdram, ctx);
    }

    // `adam.exe --live-test <rom.z64>`: load and recompile everything, print the result, no window.
    int test(const char* rom_path) {
        std::ifstream file{ rom_path, std::ios::binary };
        std::vector<uint8_t> rom{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        if (rom.size() < 0x1000) {
            fprintf(stderr, "[live] could not read %s\n", rom_path);
            return EXIT_FAILURE;
        }
        if (!register_tables()) {
            return EXIT_FAILURE;
        }
        size_t placeholders = 0;
        for (const auto& funcs : section_funcs) {
            placeholders += funcs.size();
        }
        printf("[live] tables: %zu sections with code, %zu entries\n", section_table.size(), placeholders);
        if (!compile(rom)) {
            return EXIT_FAILURE;
        }
        size_t missing = 0;
        for (const auto& funcs : section_funcs) {
            for (const FuncEntry& entry : funcs) {
                missing += (entry.func == nullptr || entry.func == not_compiled) ? 1 : 0;
            }
        }
        printf("[live] entries without code: %zu\n", missing);
        return missing == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
}
