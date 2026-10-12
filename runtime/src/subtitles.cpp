// Sound hook for subtitles: the game plays every sound effect and voice clip through func_8009B830(number, volume,
// pan, restart, priority) -> func_800ADCFC -> MusStartEffect2 (0x800AE540). Numbers below the count at 0x80149F10
// play from effect bank *0x800FF3B8, the rest (minus that count) from bank *0x800FF3BC.
// Called at the start of func_8009B830 (static build: scripts\extra_hooks.txt; live build: a syscall patch in live.cpp).
// For now it only logs, to map sound numbers to the lines in the translation file.
#include <cstdint>
#include <cstdio>

#include "recomp.h"

uint32_t adam_frame_count();

extern "C" void adam_on_sound(uint8_t* rdram, recomp_context* ctx) {
    uint32_t number = (uint32_t)ctx->r4;
    uint32_t bank_a_count = (uint32_t)MEM_W(0, (gpr)(int32_t)0x80149F10);
    printf("[sound] frame %u: number %u (bank %c %u) volume %d pan %d\n", adam_frame_count(), number,
        number < bank_a_count ? 'A' : 'B', number < bank_a_count ? number : number - bank_a_count,
        (int)(int32_t)ctx->r5, (int)(int32_t)ctx->r6);
    fflush(stdout);
}
