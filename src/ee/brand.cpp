// brand: "Enhanced Edition" branding on the Enhanced Edition program R1.28.8 (sha 5291a092).
//   1. Menu version line: "(R1.28.8)" -> "(R1.28.8 + Enhancement Pack)".
//   2. The 4K start-up splash (splash4k.cpp).
// The logo lines are data only (taller resource/interface/logo_small.tga and logo.tga, made by tools/brand/).
//
// Version line (READ): the menu frame builder makes the line at 0x535f9b..0x535fac:
//   0x535f9b  e8 20 bb 23 00        call 0x771ac0      ; version(): sprintf(0x8ebb00, "%s%s", "R1.28.8", "") -> 0x8ebb00
//   0x535fa0  89 45 98              mov [ebp-0x68],eax ; %2%
//   0x535fa3  e8 08 bb 23 00        call 0x771ab0      ; text "product_name" -> %1%
//   0x535fa8  68 a0 ac 83 00        push 0x83aca0      ; "<s(8)c(120 120 120)f(arial_hq)>%1% (%2%)"
// Only the first call becomes "mov eax, <our text>" (b8 imm32, same 5 bytes), applied through ee_patch with all
// 18 bytes as the gate. version() and its other callers (saves, network, logs) are untouched.
// uiscale.ini [EE]: VersionLine=1 (0 = their own "(R1.28.8)"); VersionText = the text inside the brackets
// (default "R1.28.8 + Enhancement Pack 1.2"). The game's format adds the brackets; '<', '>' and '%'
// are dropped from the ini text (markup and format characters), at most 63 characters.
#include <windows.h>
#include <string.h>
#include "ee.h"

extern "C" void ee_splash4k_init(void);

namespace {
const uint32_t kSite = 0x535f9b;
const unsigned char kOrig[18] = {0xe8, 0x20, 0xbb, 0x23, 0x00, 0x89, 0x45, 0x98, 0xe8,
                                 0x08, 0xbb, 0x23, 0x00, 0x68, 0xa0, 0xac, 0x83, 0x00};
const char kDefText[] = "R1.28.8 + Enhancement Pack 1.2";
char g_text[64];   // must live as long as the program (the patched code points at it)
}

extern "C" void ee_brand_init(void) {
    if (ee_ini_int("VersionLine", 1)) {
        char raw[64];   // text from [EE] VersionText
        ee_ini_str("VersionText", kDefText, raw, sizeof raw);
        size_t k = 0;
        for (const char *c = raw; *c && k < sizeof g_text - 1; c++)
            if (*c != '<' && *c != '>' && *c != '%') g_text[k++] = *c;
        g_text[k] = 0;
        if (!k) strcpy(g_text, kDefText);
        ee_log("brand: version text \"(%s)\"", g_text);
        unsigned char put[18];
        memcpy(put, kOrig, sizeof put);
        DWORD a = (DWORD)(uintptr_t)g_text;
        put[0] = 0xb8;
        memcpy(put + 1, &a, 4);
        ee_patch(kSite, kOrig, put, sizeof put, "brand: menu version line");
    } else {
        ee_log("brand: VersionLine=0, their own version line kept");
    }
    ee_splash4k_init();
}
