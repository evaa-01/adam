// Rewind: while Backspace or the left stick click is held in a mission, the game runs backwards (up to 10 s), and
// continues from that point when it's released (main.cpp reads the button; set_held). Only in missions (the battle
// overlay, see MISSION_OVERLAY) and only when the General tab's "Rewind" option is on; elsewhere nothing is recorded.
//
// How: at the start of every frame callback (func_800A8B34, the game's per-frame logic; hook in extra_hooks.txt / live.cpp)
// the game's state is in RDRAM. A shadow copy of RDRAM holds the state at the last frame start; each frame, the 1 KB
// pages that differ from it are recorded (their OLD contents = an undo record) and the shadow is updated. While
// rewinding, each frame start first puts back what the last frame changed (from the shadow), then applies the newest
// undo record. The frame then runs from that older state with no buttons pressed and shows its picture; sound is muted.
//
// Not rewound (kept live, because host-side state goes with them or the game rebuilds them each frame):
// - OS objects the game creates (threads, message queues + their buffers; reported by
//   runtime\patches\n64modernruntime-rewind.patch) and the threads' stacks (the blocked threads' host code is
//   mid-call and expects its stack frames as they are)
// - low memory + the depth buffer (0x80000000-0x80025C00) and the two framebuffers (0x802D4000, 0x8036A000; 0x25800
//   bytes each; the memory between them is game data and IS rewound)
// - variables only the system code uses (scheduler, audio library, libultra; rewind_ranges.inc from
//   scripts\rewind_ranges.py): e.g. rolling back the graphics-pending flag 0x8016D060 made the game wait forever
// An overlay load (new scene code) clears the history: the runtime's code mapping can't be rewound.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/os_objects.hpp"
#include "recomp.h"

extern "C" void yield_self(uint8_t* rdram);

namespace {
    constexpr uint32_t RAM_SIZE = 8 * 1024 * 1024;
    constexpr uint32_t PAGE_SIZE = 1024;
    constexpr uint32_t PAGE_COUNT = RAM_SIZE / PAGE_SIZE;
    constexpr std::chrono::seconds MAX_AGE{ 10 };        // how far back rewinding can go
    constexpr size_t MAX_BYTES = 512u * 1024 * 1024;
    // Guess for the size of a thread's stack below the sp the game passes to osCreateThread.
    constexpr uint32_t STACK_SIZE = 0x2000;
    // Graphics-pending flag (cleared when the RDP is done with the frame's tasks; see extra_hooks.txt).
    constexpr uint32_t GFX_PENDING = 0x8016D060;
    // The game's current mode (u16, 0x00-0x55; the state machine func_800AB8D4 switches on it, and the table at
    // 0x800D5068 gives each mode's overlay).
    constexpr uint32_t GAME_MODE = 0x800D5114;
    constexpr uint32_t LOADED_MODE = 0x800D5116;    // the mode the overlay slot was last loaded for
    constexpr uint32_t MODE_OVERLAYS = 0x800D5068;  // s16 per mode: overlay index
    constexpr uint32_t MODE_COUNT = 0x56;
    // Rewind only runs in missions: the modes whose code is overlay 1 (the battle code; seen: 0x2D mission 01, 0x30
    // mission 02). Intro, menus, cutscenes, briefings and CONTINUE? use other overlays; rewinding there crashed.
    constexpr int16_t MISSION_OVERLAY = 1;
    // u8: 1 while the game logic runs (func_800A8B34 only calls the state machine when it's set), 0 on frozen frames,
    // e.g. the pause screen (Start in a mission). Rewinding skips frozen frames: they'd only replay the pause screen.
    constexpr uint32_t LOGIC_RUNNING = 0x80149FC4;

    struct Range { uint32_t lo, hi; };   // [lo, hi): RDRAM offsets (or KSEG0 addresses before exclude())
    struct Record {
        std::chrono::steady_clock::time_point time;
        std::vector<uint32_t> pages;
        std::vector<uint8_t> data;      // PAGE_SIZE bytes per page: the contents before this frame
        bool frozen = false;            // this frame ran with the game logic stopped (pause screen)
    };

    std::mutex excluded_mutex;
    std::vector<Range> excluded;        // guarded by excluded_mutex
    std::atomic<bool> excluded_changed = true;

    std::atomic<bool> held = false;
    std::atomic<bool> enabled = true;   // General tab option
    std::atomic<bool> in_mission = false;
    std::atomic<bool> rewinding = false;
    std::atomic<bool> reset_pending = true;

    // Only touched on the game's frame callback thread.
    std::vector<uint8_t> shadow;
    std::vector<uint8_t> mask;          // 1 = excluded byte
    enum : uint8_t { PAGE_NORMAL, PAGE_PARTIAL, PAGE_EXCLUDED };
    std::vector<uint8_t> page_kind;
    std::deque<Record> history;
    size_t history_bytes = 0;

    // Statistics, printed every 10 s.
    uint64_t stat_frames = 0, stat_bytes = 0, stat_us = 0;
    std::chrono::steady_clock::time_point stat_start = std::chrono::steady_clock::now();

    void exclude(uint32_t addr, uint32_t size) {
        uint32_t lo = (addr & 0x1FFFFFFF) & ~3u;
        uint32_t hi = ((addr & 0x1FFFFFFF) + size + 3) & ~3u;
        if (size == 0 || lo >= RAM_SIZE) {
            return;
        }
        std::lock_guard lock{ excluded_mutex };
        excluded.push_back({ lo, std::min(hi, RAM_SIZE) });
        excluded_changed = true;
    }

    void on_os_object(ultramodern::OsObjectKind kind, uint32_t addr, uint32_t size, uint32_t stack_top) {
        exclude(addr, size);
        if (kind == ultramodern::OsObjectKind::Thread) {
            exclude(stack_top - STACK_SIZE, STACK_SIZE);
            printf("[rewind] thread 0x%08X, stack top 0x%08X\n", addr, stack_top);
        }
    }

    void rebuild_mask() {
        std::lock_guard lock{ excluded_mutex };
        std::fill(mask.begin(), mask.end(), 0);
        for (const Range& r : excluded) {
            std::fill(mask.begin() + r.lo, mask.begin() + r.hi, 1);
        }
        for (uint32_t p = 0; p < PAGE_COUNT; p++) {
            const uint8_t* m = mask.data() + p * PAGE_SIZE;
            size_t count = 0;
            for (uint32_t i = 0; i < PAGE_SIZE; i++) {
                count += m[i];
            }
            page_kind[p] = count == 0 ? PAGE_NORMAL : count == PAGE_SIZE ? PAGE_EXCLUDED : PAGE_PARTIAL;
        }
    }

    // Copies a page into RDRAM, keeping its excluded bytes.
    void restore_page(uint8_t* rdram, const uint8_t* src, uint32_t p) {
        uint8_t* dst = rdram + p * PAGE_SIZE;
        if (page_kind[p] == PAGE_NORMAL) {
            memcpy(dst, src, PAGE_SIZE);
            return;
        }
        const uint8_t* m = mask.data() + p * PAGE_SIZE;
        for (uint32_t i = 0; i < PAGE_SIZE; i++) {
            if (!m[i]) {
                dst[i] = src[i];
            }
        }
    }

    bool record_time_too_old(std::chrono::steady_clock::time_point time) {
        return std::chrono::steady_clock::now() - time > MAX_AGE;
    }

    void record_frame(uint8_t* rdram, bool frozen) {
        Record record;
        record.time = std::chrono::steady_clock::now();
        record.frozen = frozen;
        for (uint32_t p = 0; p < PAGE_COUNT; p++) {
            if (page_kind[p] == PAGE_EXCLUDED) {
                continue;
            }
            uint8_t* cur = rdram + p * PAGE_SIZE;
            uint8_t* old = shadow.data() + p * PAGE_SIZE;
            if (memcmp(cur, old, PAGE_SIZE) != 0) {
                record.pages.push_back(p);
                record.data.insert(record.data.end(), old, old + PAGE_SIZE);
                memcpy(old, cur, PAGE_SIZE);
            }
        }
        stat_bytes += record.data.size();
        history_bytes += record.data.size();
        history.push_back(std::move(record));
        while (record_time_too_old(history.front().time) || history_bytes > MAX_BYTES) {
            history_bytes -= history.front().data.size();
            history.pop_front();
        }
    }

    void step_back(uint8_t* rdram) {
        // The shadow holds the state at the start of the last (rewound) frame; go further back from there (adam_rewind_frame doesn't step back with an empty history): one frame, plus any frozen
        // (pause screen) frames before it. Always an odd number of frames: the game alternates between two framebuffers
        // frame by frame, and going back an even number would make it draw into the one on screen and wait forever for
        // it to be swapped out.
        size_t count = 1;
        while (count < history.size() && history[history.size() - count].frozen) {
            count++;
        }
        if (count % 2 == 0) {
            count += count < history.size() ? 1 : -1;
        }
        for (size_t n = 0; n < count; n++) {
            Record& record = history.back();
            for (size_t i = 0; i < record.pages.size(); i++) {
                uint32_t p = record.pages[i];
                memcpy(shadow.data() + p * PAGE_SIZE, record.data.data() + i * PAGE_SIZE, PAGE_SIZE);
            }
            history_bytes -= record.data.size();
            history.pop_back();
        }
        // RAM = shadow (this also undoes what the last rewound frame changed).
        for (uint32_t p = 0; p < PAGE_COUNT; p++) {
            if (page_kind[p] != PAGE_EXCLUDED && memcmp(rdram + p * PAGE_SIZE, shadow.data() + p * PAGE_SIZE, PAGE_SIZE) != 0) {
                restore_page(rdram, shadow.data() + p * PAGE_SIZE, p);
            }
        }
    }
}

namespace adam::rewind {
    void init() {
        shadow.resize(RAM_SIZE);
        mask.resize(RAM_SIZE);
        page_kind.resize(PAGE_COUNT);
        exclude(0x80000000, 0x25C00);              // low memory + depth buffer
        // The two framebuffers (320x240 RGBA16 each). The 450 KB between them (0x802F9800-0x8036A000) is game data (e.g.
        // the battle's model objects) and must be rewound: kept live, the objects no longer matched the rewound
        // pointers to them and the game read a null pointer (func_8009FA60).
        exclude(0x802D4000, 320 * 240 * 2);
        exclude(0x8036A000, 320 * 240 * 2);
        // The audio library's heap (voices, sample buffer lists): alHeapInit's ALHeap at 0x80102BB0 holds base
        // 0x80270000, size 0x64000 (Project64 dumps). Rolled back while the library's list heads stayed live, the
        // sample buffer list became circular and the audio thread looped forever.
        exclude(0x80270000, 0x64000);
        // Variables only the system code uses (scheduler, audio library, libultra; scripts\rewind_ranges.py).
        static constexpr Range system_ranges[] = {
#include "rewind_ranges.inc"
        };
        for (const Range& r : system_ranges) {
            exclude(r.lo, r.hi - r.lo);
        }
        ultramodern::set_os_object_callback(on_os_object);
    }

    void set_held(bool h) {
        held = h;
    }

    void set_enabled(bool e) {
        enabled = e;
    }

    // In a mission and switched on: the button may start a rewind.
    bool available() {
        return enabled.load() && in_mission.load();
    }

    bool active() {
        return rewinding.load();
    }

    // In a mission's gameplay (battle code loaded), whether or not rewind is switched on. main.cpp allows widescreen only here.
    bool in_gameplay() {
        return in_mission.load();
    }
}

extern "C" std::atomic<uint64_t> adam_frame_hook_calls{ 0 }; // for main.cpp's watchdog (debug)

// Frame callback entry (func_800A8B34), on the game's callback thread.
extern "C" void adam_rewind_frame(uint8_t* rdram) {
    adam_frame_hook_calls++;
    auto start = std::chrono::steady_clock::now();
    static uint32_t last_mode = 0xFFFFFFFF;
    uint32_t mode = MEM_HU(0, (gpr)(int32_t)GAME_MODE);
    if (mode != last_mode) {
        static const auto first = start;
        printf("[rewind] game mode 0x%02X at %.1f s\n", mode, std::chrono::duration<double>(start - first).count());
        last_mode = mode;
    }
    bool mission = mode < MODE_COUNT && mode == MEM_HU(0, (gpr)(int32_t)LOADED_MODE) &&
        MEM_H(0, (gpr)(int32_t)(MODE_OVERLAYS + mode * 2)) == MISSION_OVERLAY;
    in_mission = mission;
    static const bool disabled = std::getenv("ADAM_REWIND") != nullptr && std::getenv("ADAM_REWIND")[0] == '0';
    if (disabled) {
        return;
    }
    if (!mission || !enabled.load()) {
        // Nothing recorded here; the next mission frame starts a fresh history.
        reset_pending = true;
        rewinding = false;
        return;
    }
    if (excluded_changed.exchange(false)) {
        rebuild_mask();
    }
    // Whether the frame starting now runs with the game logic stopped (stored with the record made at the next frame).
    static bool frame_frozen = false;
    if (reset_pending.exchange(false)) {
        memcpy(shadow.data(), rdram, RAM_SIZE);
        history.clear();
        history_bytes = 0;
        rewinding = false;
        frame_frozen = MEM_BU(0, (gpr)(int32_t)LOGIC_RUNNING) == 0;
        return;
    }
    bool rewind = held.load();
    if (rewind != rewinding.load()) {
        printf("[rewind] %s (%zu frames of history)\n", rewind ? "start" : "stop", history.size());
    }
    if (rewind && history.empty()) {
        // At the oldest frame (10 s back, or right after a scene started): hold the game until the button is released.
        // Replaying the same frame again isn't possible: it would draw into the framebuffer on screen and wait forever
        // for it to be swapped out. Like the P pause, this blocks the game thread (and with it the game).
        printf("[rewind] at the oldest frame: holding\n");
        while (held.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        rewind = false;
        printf("[rewind] stop (0 frames of history)\n");
    }
    rewinding = rewind;
    if (rewind) {
        // The RDP may still be reading the last frame's display lists from RDRAM.
        while (MEM_W(0, (gpr)(int32_t)GFX_PENDING) != 0) {
            yield_self(rdram);
        }
        step_back(rdram);
    } else {
        record_frame(rdram, frame_frozen);
    }
    frame_frozen = MEM_BU(0, (gpr)(int32_t)LOGIC_RUNNING) == 0;

    stat_frames++;
    stat_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    if (std::chrono::steady_clock::now() - stat_start > std::chrono::seconds(10)) {
        printf("[rewind] %llu frames: %.2f ms and %llu KB per frame; history %zu frames, %zu MB\n",
            (unsigned long long)stat_frames, stat_us / 1000.0 / stat_frames, (unsigned long long)(stat_bytes / 1024 / stat_frames),
            history.size(), history_bytes / (1024 * 1024));
        stat_frames = stat_bytes = stat_us = 0;
        stat_start = std::chrono::steady_clock::now();
    }
}

// The overlay loader put new code into the shared slot: rewinding past it isn't possible.
extern "C" void adam_rewind_reset(void) {
    reset_pending = true;
}
