// Stand-in for the game-side header that RecompFrontend's ui_api_events.cpp includes. Upstream it is reached
// as "../../../../../patches/ui_funcs.h" (RecompFrontend inside a game repo's lib\ folder, with the game's mod
// patch code in patches\); runtime\patches\recompfrontend-ui-funcs.patch makes it a plain "ui_funcs.h" found
// here. adam has no patch code: this only declares the function RecompFrontend itself defines and pulls in the
// event structs it uses.
#pragma once

#include "recomp.h"
#include "recompui/event_structs.h"

extern "C" void recomp_run_ui_callbacks(uint8_t* rdram, recomp_context* ctx);
