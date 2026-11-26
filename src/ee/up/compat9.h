// compat9.h: what the interface upscaler (up9.cpp) needs from its surroundings, provided by the
// d3d9 proxy (src/ee/d3dhook.cpp).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <string>
#include "../ee.h"

#define log_text ee_log
std::string fmtname(D3DFORMAT f);
extern volatile LONG g_frames;   // Present count

// The old scale layer's state, as far as the upscaler reads it. On the Enhanced Edition the game
// scales its interface itself (projection), so: active = interface factor > 1, gw/gh = layout size,
// bb = the real back buffer, rt_is_bb = the current render target is the back buffer.
struct ScaleState {
    bool active = false;
    UINT gw = 0, gh = 0;
    void *bb = nullptr;
    bool rt_is_bb = true;
};
extern ScaleState g_sc;
extern CRITICAL_SECTION g_ee_cs;   // the proxy hook lock, = up9 g_uplk
