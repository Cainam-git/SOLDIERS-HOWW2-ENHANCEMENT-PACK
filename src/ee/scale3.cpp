// Tested in the game: 17 of 17 places patch; menus, Options, campaign lists, map clip, briefing,
//   mission-failed box and text at x3; clicks on menus land.
//   OPEN POINT: in one test a left click on a unit in the 3D view moved the camera far away instead of
//   selecting the unit; whether the program's own x2 does the same has not been checked.
//   Not yet run in the game: the pointer x factor (DXVK's SetCursor import hooked, below), and MouseFix
//   (remainder kept for the movement that triggers the hover update; 0x4e64ff, 0x4e662d) with its
//   MouseLog=1 diagnostic.
//
// src/ee/scale3.cpp: interface factor 3 (or any whole factor) for the Enhanced Edition program.
//
// What the program does by itself (READ, Soldiers.exe 5291a092): when the chosen screen mode is wider
// than 2560 it sets bit 1 (value 2) of the screen object's first byte 0x8c1680 (0x4e8f6b..0x4e8f96),
// lays the interface out at screen/2 and draws it x2 through the interface camera. Every place that
// depends on that factor (all readers of the flag, found by reading every reference to 0x8c1680 and
// to the screen size 0x8c1684/0x8c1688, and every reader of the 0.5 constant 0x82aea4 inside them):
//   0x4e8f6b  flag decision  cmp [ebp-0x100],0xa00  -> call stub_flag (chooses the factor, sets flags)
//   0x4e8f8c  camera branch  cmp [ebp-0x100],0xa00  -> cmp dword [g_factor],1
//   0x4e8fcb  camera size    (w-1)*0.5, (h-1)*0.5   -> operand points at g_inv (1/factor)
//   0x5b2a4d, 0x5b2c16  root window size: screen / ((flag>>1 & 1) + 1)  -> call stub: divisor = factor
//   0x4e6351, 0x4e6364  edge test (0x4e6330): screen * 0.5        -> g_inv
//   0x4e647f, 0x4e65fb  mouse deltas (0x4e63e0): xmm1 = 0.5       -> g_inv
//   0x4e6596, 0x4e65b0, 0x4e6695, 0x4e66ac  cursor * 0.5 (0x4e63e0) -> g_inv
//   0x4e677e, 0x4e6794  cursor * 0.5 (0x4e6740, the mouse position everyone reads) -> g_inv
//   0x4e686a  edge scroll (0x4e6820): xmm1 = 0.5                  -> g_inv
//   0x771c2d  video window rectangle x2 (IVideoWindow::SetWindowPosition) -> jmp stub_video (x factor)
// The constant 0.5 at 0x82aea4 itself is NOT changed (374 readers, almost all unrelated).
//
// uiscale.ini [EE]:
//   UiFactor=3      whole factor for the interface; 0 = module off (the program's own x2 stays);
//                   2 = same as the program's own behaviour.
//   UiMinWidth=2561 the factor applies only to screen modes at least this wide (the program's own
//                   rule is "wider than 2560"); narrower modes keep the interface x1.
// The factor is lowered when the laid-out interface would be smaller than 800x600 (the smallest
// mode the game itself offers): 3840x2160 -> 3 (1280x720), 3440x1440 -> 2, 3840x1600 -> 2.
//
//   UiPointer=1     the mouse pointer is enlarged by the same factor (the game's pointer is the Direct3D
//                   cursor, 32x32; DXVK turns it into a Win32 cursor and shows it with SetCursor). 0 = 32 px.
//
// ee_scale3_factor() gives the factor in use right now (1 = not scaled) for the other modules.
// ee_scale3_set_cursor_scaler(fn) lets another module enlarge pointer pictures (e.g. xBRZ); default nearest.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <math.h>
#include <string.h>
#include <stdio.h>   // sprintf for the byte detail in the log
#include <stdlib.h>
#include "ee.h"

extern "C" {
int ee3_factor = 1;           // factor in use (1 when the flag is off)
int ee3_fm1 = 0;              // factor - 1 (divisor stubs)
float ee3_inv = 1.0f;         // 1 / factor (float operand sites)
uint32_t ee3_video_back = 0;  // 0x771c45 in the running program
int ee3_cfg_factor = 3;       // ini UiFactor
int ee3_cfg_minwidth = 2561;  // ini UiMinWidth
void ee3_stub_flag(void);
void ee3_stub_div_esi(void);
void ee3_stub_div_ecx(void);
void ee3_stub_video(void);
void __cdecl ee3_choose(const int *mode);
int ee_scale3_factor(void);
void ee3_pointer_hook(void);
void ee3_stub_delta(void);
void ee3_stub_moveev(void);
void __cdecl ee3_delta(int *out, int dx, int dy);
void __cdecl ee3_moveev(void);
uint32_t ee3_moveev_target = 0;   // 0x4e3830 in the running program
}

static bool g_patched = false;
static uintptr_t g_base = 0x400000;
static int g_mousefix = 1, g_mouselog = 0;
static DWORD WINAPI mouse_watch(LPVOID);

// Stubs. Called from the program's code; they keep every register the program relies on.
asm(R"(
.intel_syntax noprefix
.text
.p2align 4
.globl _ee3_stub_flag
_ee3_stub_flag:
    pushad
    mov eax, dword ptr [ebp-0x108]
    mov ecx, dword ptr [ebp-0x114]
    lea eax, [eax+ecx*4]
    push eax
    call _ee3_choose
    add esp, 4
    popad
    cmp dword ptr [_ee3_factor], 1
    ret
.p2align 4
.globl _ee3_stub_div_esi
_ee3_stub_div_esi:
    and esi, 2
    jz 1f
    mov esi, dword ptr [_ee3_fm1]
1:  ret
.p2align 4
.globl _ee3_stub_div_ecx
_ee3_stub_div_ecx:
    and ecx, 2
    jz 2f
    mov ecx, dword ptr [_ee3_fm1]
2:  ret
.p2align 4
.globl _ee3_stub_video
_ee3_stub_video:
    imul eax, dword ptr [_ee3_factor]
    push eax
    mov eax, edi
    imul eax, dword ptr [_ee3_factor]
    push eax
    mov eax, ebx
    imul eax, dword ptr [_ee3_factor]
    push eax
    mov eax, dword ptr [esi+0x58]
    imul eax, dword ptr [_ee3_factor]
    jmp dword ptr [_ee3_video_back]
.p2align 4
.globl _ee3_stub_delta
_ee3_stub_delta:
    pushad
    push ecx
    push ebx
    lea eax, [ebp-0x34]
    push eax
    call _ee3_delta
    add esp, 12
    popad
    ret
.p2align 4
.globl _ee3_stub_moveev
_ee3_stub_moveev:
    pushad
    call _ee3_moveev
    popad
    jmp dword ptr [_ee3_moveev_target]
.att_syntax prefix
)");

// mode = the chosen entry of the program's mode list: width, height, refresh.
extern "C" __attribute__((force_align_arg_pointer)) void __cdecl ee3_choose(const int *mode) {
    int w = mode[0], h = mode[1];
    int f = 1;
    if (w >= ee3_cfg_minwidth) {
        f = ee3_cfg_factor;
        while (f > 1 && (w / f < 800 || h / f < 600))
            f--;
    }
    ee3_factor = f;
    ee3_fm1 = f - 1;
    ee3_inv = 1.0f / (float)f;
    ee_log("scale3: screen mode %dx%d -> interface factor %d (laid out as %dx%d)", w, h, f, w / f, h / f);
    ee3_pointer_hook();   // DXVK is loaded by now (the device exists)
    static bool watch;
    if (g_mouselog && !watch) {
        watch = true;
        HANDLE t = CreateThread(NULL, 0, mouse_watch, NULL, 0, NULL);
        if (t) CloseHandle(t);
        ee_log("scale3: MouseLog=1: mouse watcher started");
    }
}

// ---------------------------------------------------------------- pointer x factor
// DXVK (d3d9_cursor.cpp) makes a 32x32 Win32 cursor from the game's Direct3D cursor and shows it with
// ::SetCursor. A larger Direct3D cursor does not help (96x96 is rejected, 128x128 is cropped). So DXVK's own
// SetCursor import is redirected: the cursor it sets is replaced by an enlarged copy (hotspot x factor).
typedef void (*ee3_cursor_scale_fn)(const uint32_t *src, int w, int h, uint32_t *dst, int factor);
static ee3_cursor_scale_fn g_cur_scaler;
extern "C" void ee_pointer_enlarge(const uint32_t *src, int w, int h, uint32_t *dst, int f);   // src/ee/pointer9.cpp
extern "C" void ee_scale3_set_cursor_scaler(ee3_cursor_scale_fn fn) { g_cur_scaler = fn; }
static HCURSOR(WINAPI *o_SetCursor)(HCURSOR);
static int g_pointer_on = 1;

static uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 16777619u;
    return h;
}

struct CurEntry { uint32_t key; int f; HCURSOR big; };
static CurEntry g_cur[256];
static int g_cur_n, g_cur_next;

static HCURSOR enlarged(HCURSOR src, int f) {
    ICONINFO ii;
    if (!GetIconInfo(src, &ii)) return src;
    HCURSOR out = src;
    BITMAP bm;
    if (ii.hbmColor && GetObjectA(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmWidth <= 64 &&
        bm.bmHeight > 0 && bm.bmHeight <= 64) {
        int w = bm.bmWidth, h = bm.bmHeight, nw = w * f, nh = h * f;
        static uint32_t px[64 * 64];
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        HDC dc = GetDC(0);
        int got = GetDIBits(dc, ii.hbmColor, 0, h, px, &bi, DIB_RGB_COLORS);
        ReleaseDC(0, dc);
        if (got == h) {
            uint32_t key = fnv(px, (size_t)w * h * 4, fnv(&ii.xHotspot, 4, fnv(&ii.yHotspot, 4, (uint32_t)w * 131 + h)));
            for (int i = 0; i < g_cur_n; i++)
                if (g_cur[i].key == key && g_cur[i].f == f) { out = g_cur[i].big; goto done; }
            {
                uint32_t *dst = (uint32_t *)malloc((size_t)nw * nh * 4);
                int mp = ((nw + 15) / 16) * 2;
                uint8_t *mask = (uint8_t *)malloc((size_t)mp * nh);
                if (dst && mask) {
                    if (g_cur_scaler)
                        g_cur_scaler(px, w, h, dst, f);
                    else
                        ee_pointer_enlarge(px, w, h, dst, f);   // smooth (xBRZ), src/ee/pointer9.cpp
                    memset(mask, 0xff, (size_t)mp * nh);   // as DXVK: the alpha decides
                    ICONINFO ni = {};
                    ni.fIcon = FALSE;
                    ni.xHotspot = ii.xHotspot * f + (f - 1) / 2;   // centre of the enlarged pixel
                    ni.yHotspot = ii.yHotspot * f + (f - 1) / 2;
                    ni.hbmMask = CreateBitmap(nw, nh, 1, 1, mask);
                    ni.hbmColor = CreateBitmap(nw, nh, 1, 32, dst);
                    HCURSOR big = ni.hbmMask && ni.hbmColor ? CreateIconIndirect(&ni) : 0;
                    if (ni.hbmMask) DeleteObject(ni.hbmMask);
                    if (ni.hbmColor) DeleteObject(ni.hbmColor);
                    if (big) {
                        int slot = g_cur_n < 256 ? g_cur_n++ : g_cur_next++ % 256;   // 39 cursors / 180 frames fit
                        if (g_cur[slot].big && g_cur[slot].big != big) DestroyCursor(g_cur[slot].big);
                        g_cur[slot] = {key, f, big};
                        out = big;
                        ee_log("scale3: pointer %dx%d hotspot %lu,%lu -> %dx%d (%d cached)", w, h, ii.xHotspot,
                               ii.yHotspot, nw, nh, g_cur_n);
                    }
                }
                free(dst);
                free(mask);
            }
        }
    }
done:
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}

static HCURSOR WINAPI h_SetCursor(HCURSOR c) {
    int f = ee_scale3_factor();
    if (c && f > 1 && g_pointer_on) {
        static CRITICAL_SECTION cs;
        static LONG init;
        if (InterlockedCompareExchange(&init, 1, 0) == 0) InitializeCriticalSection(&cs), init = 2;
        while (init != 2) Sleep(0);
        EnterCriticalSection(&cs);
        c = enlarged(c, f);
        LeaveCriticalSection(&cs);
    }
    return o_SetCursor(c);
}

// The real (DXVK) d3d9: every module named d3d9.dll or d3d9_next.dll that is not this proxy.
extern "C" void ee3_pointer_hook(void) {
    static bool done;
    if (done || !g_pointer_on) return;
    HMODULE self = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ee3_pointer_hook, &self);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me;
    me.dwSize = sizeof me;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        if ((HMODULE)me.hModule == self) continue;
        if (lstrcmpiA(me.szModule, "d3d9.dll") && lstrcmpiA(me.szModule, "d3d9_next.dll")) continue;
        uint8_t *b = (uint8_t *)me.hModule;
        IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
        IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dd.VirtualAddress) continue;
        for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd.VirtualAddress); d->Name; d++) {
            if (lstrcmpiA((char *)(b + d->Name), "user32.dll")) continue;
            IMAGE_THUNK_DATA32 *names = (IMAGE_THUNK_DATA32 *)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            IMAGE_THUNK_DATA32 *iat = (IMAGE_THUNK_DATA32 *)(b + d->FirstThunk);
            for (; names->u1.AddressOfData; names++, iat++) {
                if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
                const char *fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData))->Name;
                if (strcmp(fn, "SetCursor")) continue;
                DWORD old;
                if (VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old)) {
                    o_SetCursor = (HCURSOR(WINAPI *)(HCURSOR))(uintptr_t)iat->u1.Function;
                    iat->u1.Function = (DWORD)(uintptr_t)h_SetCursor;
                    VirtualProtect(&iat->u1.Function, 4, old, &old);
                    done = true;
                    ee_log("scale3: pointer hook in %s (SetCursor import)", me.szExePath);
                }
            }
        }
    }
    CloseHandle(snap);
    if (!done) ee_log("scale3: pointer hook NOT placed (no DXVK d3d9 with a SetCursor import found)");
}

extern "C" int ee_scale3_factor(void) {
    unsigned char flag = *(volatile unsigned char *)(g_base + 0x4c1680);  // 0x8c1680
    if (!(flag & 2))
        return 1;
    return g_patched ? ee3_factor : 2;
}

// ---------------------------------------------------------------- mouse: move events + diagnostic
// READ (5291a092): the interface finds the window under the pointer (hover, 0x5b2d60 -> [0x8e5a3c]) only on a
// "mouse moved" event (type 8, posted by 0x4e3830(0) at 0x4e696e). That event is posted from 0x4e6820 only when the
// DirectInput movement of this batch, multiplied by 1/factor and TRUNCATED (cvttss2si, 0x4e64ff / 0x4e662d), is not 0.
// So a slow move (1..F-1 raw pixels per batch) posts nothing: the hover (and the row a click goes to) stays where the
// last event left it. At x2 one raw pixel is lost per batch, at x3 two. The position itself (0x4e6740) is exact
// (GetCursorPos x 1/factor), and the game never moves the system pointer (SetCursorPos only in replays).
// Fix (MouseFix=1, default): the two scaling blocks are replaced by ee3_delta, which keeps the remainder per axis
// (floor division), so the movement is passed on exactly and a move event comes every F raw pixels at most.
static volatile LONG g_rem_x, g_rem_y;
static volatile LONG g_last_raw_x, g_last_raw_y, g_last_q_x, g_last_q_y;
static volatile DWORD g_last_delta_tick, g_last_ev_tick;
static volatile LONG g_ev_x = -1, g_ev_y = -1, g_ev_n;

static int floordiv(int a, int b) { int q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }

extern "C" void __cdecl ee3_delta(int *out, int dx, int dy) {
    int f = ee3_factor > 1 ? ee3_factor : 1;
    int tx = g_rem_x + dx, ty = g_rem_y + dy;
    int qx = floordiv(tx, f), qy = floordiv(ty, f);
    g_rem_x = tx - qx * f;
    g_rem_y = ty - qy * f;
    out[0] = qx;
    out[1] = qy;
    g_last_raw_x = dx, g_last_raw_y = dy, g_last_q_x = qx, g_last_q_y = qy;
    g_last_delta_tick = GetTickCount();
}

extern "C" void __cdecl ee3_moveev(void) {   // a "mouse moved" event is posted now: the hover uses this position
    POINT p;
    if (GetCursorPos(&p)) g_ev_x = p.x, g_ev_y = p.y;
    g_last_ev_tick = GetTickCount();
    InterlockedIncrement(&g_ev_n);
}

// MouseLog=1: every 100 ms, when the real pointer is more than 6 px away from where the last move event saw it
// (= where the game's hover and clicks think the pointer is) and has been still for 150 ms, one line.
static DWORD WINAPI mouse_watch(LPVOID) {
    LONG last_x = -99999, last_y = -99999;
    for (;; Sleep(100)) {
        if (ee_scale3_factor() <= 1 || g_ev_x < 0) continue;
        POINT p;
        if (!GetCursorPos(&p)) continue;
        int f = ee_scale3_factor();
        LONG dx = p.x - g_ev_x, dy = p.y - g_ev_y;
        DWORD now = GetTickCount();
        if ((labs(dx) > 6 || labs(dy) > 6) && now - g_last_delta_tick > 150 && (p.x != last_x || p.y != last_y)) {
            ee_log("mouse: real %ld,%ld (layout %ld,%ld) | game's hover position from the last move event %ld,%ld "
                   "(layout %ld,%ld, %lu ms ago, event #%ld) | off by %ld,%ld real px | last movement batch raw %ld,%ld "
                   "-> passed on %ld,%ld (remainder %ld,%ld) %lu ms ago | MouseFix=%d",
                   p.x, p.y, p.x / f, p.y / f, g_ev_x, g_ev_y, g_ev_x / f, g_ev_y / f, now - g_last_ev_tick, g_ev_n, dx, dy,
                   g_last_raw_x, g_last_raw_y, g_last_q_x, g_last_q_y, g_rem_x, g_rem_y, now - g_last_delta_tick,
                   g_mousefix);
            last_x = p.x, last_y = p.y;
        }
    }
    return 0;
}

namespace {
struct Site {
    uint32_t addr;
    unsigned char old[40];
    unsigned char put[40];
    int n;
    const char *name;
};

void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }

// call/jmp rel32 from program address `at` (unrelocated) to `to` (absolute in this process)
void rel32(unsigned char *p, unsigned char op, uint32_t at, const void *to) {
    p[0] = op;
    put32(p + 1, (uint32_t)(uintptr_t)to - (uint32_t)(g_base + (at - 0x400000) + 5));
}
}  // namespace

extern "C" void ee_scale3_init(void) {
    ee3_cfg_factor = ee_ini_int("UiFactor", 3);
    ee3_cfg_minwidth = ee_ini_int("UiMinWidth", 2561);
    g_pointer_on = ee_ini_int("UiPointer", 1) != 0;
    g_mousefix = ee_ini_int("MouseFix", 1) != 0;
    g_mouselog = ee_ini_int("MouseLog", 0) == 1;
    if (ee3_cfg_factor <= 0) {
        ee_log("scale3: UiFactor=%d -> off, the program's own interface x2 stays", ee3_cfg_factor);
        return;
    }
    if (ee3_cfg_factor > 8)
        ee3_cfg_factor = 8;
    if (!ee_program_known()) {
        ee_log("scale3: unknown program, nothing patched");
        return;
    }
    g_base = (uintptr_t)GetModuleHandleA(NULL);
    const uint32_t d = (uint32_t)(g_base - 0x400000);  // relocation delta (0 when at its preferred base)
    const uint32_t half = 0x82aea4 + d;                // the program's shared 0.5
    const uint32_t inv = (uint32_t)(uintptr_t)&ee3_inv;
    const uint32_t fac = (uint32_t)(uintptr_t)&ee3_factor;
    ee3_video_back = 0x771c45 + d;
    ee3_moveev_target = 0x4e3830 + d;

    static Site s[24];
    int n = 0;
    // flag decision: cmp dword ptr [ebp-0x100], 0xa00 -> call stub_flag + 5-byte nop
    {
        Site &x = s[n++];
        x = {0x4e8f6b, {0x81, 0xbd, 0x00, 0xff, 0xff, 0xff, 0x00, 0x0a, 0x00, 0x00}, {}, 10, "flag decision"};
        rel32(x.put, 0xe8, x.addr, (void *)ee3_stub_flag);
        memcpy(x.put + 5, "\x0f\x1f\x44\x00\x00", 5);
    }
    // camera branch: cmp dword ptr [ebp-0x100], 0xa00 -> cmp dword ptr [g_factor], 1 + 3-byte nop
    {
        Site &x = s[n++];
        x = {0x4e8f8c, {0x81, 0xbd, 0x00, 0xff, 0xff, 0xff, 0x00, 0x0a, 0x00, 0x00}, {0x83, 0x3d}, 10, "camera branch"};
        put32(x.put + 2, fac);
        x.put[6] = 0x01;
        memcpy(x.put + 7, "\x0f\x1f\x00", 3);
    }
    // float operand sites: movss/mulss xmmN, [0x82aea4] -> [g_inv]
    struct F { uint32_t a; unsigned char op, modrm; const char *name; };
    static const F fl[] = {
        {0x4e8fcb, 0x10, 0x05, "camera size"},       {0x4e6351, 0x59, 0x05, "edge test x"},
        {0x4e6364, 0x59, 0x05, "edge test y"},       {0x4e647f, 0x10, 0x0d, "mouse delta 1"},
        {0x4e65fb, 0x10, 0x0d, "mouse delta 2"},     {0x4e6596, 0x59, 0x05, "mouse button x"},
        {0x4e65b0, 0x59, 0x05, "mouse button y"},    {0x4e6695, 0x59, 0x05, "mouse move x"},
        {0x4e66ac, 0x59, 0x05, "mouse move y"},      {0x4e677e, 0x59, 0x05, "mouse pos x"},
        {0x4e6794, 0x59, 0x05, "mouse pos y"},       {0x4e686a, 0x10, 0x0d, "edge scroll"},
    };
    for (const F &f : fl) {
        Site &x = s[n++];
        x = {f.a, {0xf3, 0x0f, f.op, f.modrm}, {0xf3, 0x0f, f.op, f.modrm}, 8, f.name};
        put32(x.old + 4, half);
        put32(x.put + 4, inv);
    }
    // root window size divisor: shr r,1; and r,1 -> call stub (r = factor-1 or 0; the program adds 1)
    {
        Site &x = s[n++];
        x = {0x5b2a4d, {0xd1, 0xee, 0x83, 0xe6, 0x01}, {}, 5, "root size 1"};
        rel32(x.put, 0xe8, x.addr, (void *)ee3_stub_div_esi);
    }
    {
        Site &x = s[n++];
        x = {0x5b2c16, {0xd1, 0xe9, 0x83, 0xe1, 0x01}, {}, 5, "root size 2"};
        rel32(x.put, 0xe8, x.addr, (void *)ee3_stub_div_ecx);
    }
    // video window rectangle: start of the x2 branch (add eax,eax; push eax; lea eax,[edi+edi]..) -> jmp stub
    {
        Site &x = s[n++];
        x = {0x771c2d, {0x03, 0xc0, 0x50, 0x8d, 0x04}, {}, 5, "video rectangle"};
        rel32(x.put, 0xe9, x.addr, (void *)ee3_stub_video);
    }
    // mouse: the two delta scaling blocks (36 bytes each) -> call ee3_stub_delta; jmp over the rest
    if (g_mousefix) {
        static const unsigned char blk[36] = {0x66, 0x0f, 0x6e, 0xc3, 0x0f, 0x5b, 0xc0, 0xf3, 0x0f, 0x59, 0xc1, 0xf3,
                                              0x0f, 0x2c, 0xc0, 0x66, 0x0f, 0x6e, 0xc1, 0x0f, 0x5b, 0xc0, 0x89, 0x45,
                                              0xcc, 0xf3, 0x0f, 0x59, 0xc1, 0xf3, 0x0f, 0x2c, 0xc0, 0x89, 0x45, 0xd0};
        static const uint32_t at[2] = {0x4e64ff, 0x4e662d};
        static const char *nm[2] = {"mouse movement 1 (remainder kept)", "mouse movement 2 (remainder kept)"};
        for (int k = 0; k < 2; k++) {
            Site &x = s[n++];
            x.addr = at[k];
            x.n = 36;
            x.name = nm[k];
            memcpy(x.old, blk, 36);
            memcpy(x.put, blk, 36);
            rel32(x.put, 0xe8, x.addr, (void *)ee3_stub_delta);
            x.put[5] = 0xeb;          // jmp short to the end of the block
            x.put[6] = 36 - 7;
        }
    }
    // MouseLog: the "mouse moved" event call at 0x4e696e -> stub that notes the position, then the game's 0x4e3830
    if (g_mouselog) {
        Site &x = s[n++];
        x = {0x4e696e, {0xe8, 0xbd, 0xce, 0xff, 0xff}, {}, 5, "mouse move event (log)"};
        rel32(x.put, 0xe8, x.addr, (void *)ee3_stub_moveev);
    }

    // all or nothing: check every site first
    for (int i = 0; i < n; i++) {
        const unsigned char *p = (const unsigned char *)(g_base + (s[i].addr - 0x400000));
        if (memcmp(p, s[i].old, s[i].n) && memcmp(p, s[i].put, s[i].n)) {
            char got[3 * 16 + 1] = "", want[3 * 16 + 1] = "";
            for (int k = 0; k < s[i].n && k < 16; k++) {
                sprintf(got + 3 * k, "%02x ", p[k]);
                sprintf(want + 3 * k, "%02x ", s[i].old[k]);
            }
            ee_log("scale3: site %s @%08x has unexpected bytes: NOTHING patched (found %s| expected %s)", s[i].name, s[i].addr, got, want);
            return;
        }
    }
    int ok = 0;
    for (int i = 0; i < n; i++)
        ok += ee_patch(s[i].addr, s[i].old, s[i].put, s[i].n, s[i].name) ? 1 : 0;
    g_patched = ok == n;
    ee_log("scale3: %d of %d sites patched; UiFactor=%d UiMinWidth=%d MouseFix=%d MouseLog=%d", ok, n, ee3_cfg_factor,
           ee3_cfg_minwidth, g_mousefix, g_mouselog);
}
