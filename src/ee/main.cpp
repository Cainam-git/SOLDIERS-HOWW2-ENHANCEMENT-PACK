// src/ee/main.cpp: pass-through d3d9.dll proxy for the Soldiers Enhanced Edition program,
// plus the shared helpers of ee.h (ee_patch / ee_ini_int / ee_log).
//
// Load: game folder d3d9.dll, Wine override d3d9=n,b. The real d3d9 is <system dir>\d3d9.dll
// (DXVK under Proton), or d3d9_next.dll beside this dll when that file exists.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "ee.h"

// ---------------------------------------------------------------- program check
// Soldiers.exe 5291a092... (Enhanced Edition R1.28.8, built 2025-10-08)
static const uint32_t KNOWN_BASE = 0x400000;
static const DWORD KNOWN_TIMESTAMP = 0x68e61d55;
static const DWORD KNOWN_SIZEOFIMAGE = 0x52f000;
static const DWORD KNOWN_ENTRY = 0x3af2e9;
// first bytes at the entry point and at the interface-scale switch 0x4e8f6b (cmp [ebp-0x100],0xa00)
static const unsigned char KNOWN_ENTRY_BYTES[] = {0xe8, 0xd4, 0x02, 0x00, 0x00, 0xe9, 0x7a, 0xfe};
static const unsigned char KNOWN_4E8F6B[] = {0x81, 0xbd, 0x00, 0xff, 0xff, 0xff, 0x00, 0x0a, 0x00, 0x00};

static HMODULE g_self;
static CRITICAL_SECTION g_cs;
static int g_known = -1;     // -1 not checked yet
static uintptr_t g_base;     // where the program really is

extern "C" char *ee_beside_program(const char *name, char *buf) {
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    char *s = strrchr(buf, '\\');
    char *d = s ? s + 1 : buf;
    snprintf(d, MAX_PATH - (d - buf), "%s", name);
    return buf;
}

// ---------------------------------------------------------------- log
static int g_logon = -1;
static FILE *g_logf;

extern "C" bool ee_log_on(void) {
    if (g_logon < 0) {
        const char *e = getenv("EE_LOG");
        g_logon = (e && *e == '1') || ee_ini_int("Log", 0) == 1;
    }
    return g_logon == 1;
}

extern "C" void ee_log(const char *fmt, ...) {
    if (!ee_log_on())
        return;
    EnterCriticalSection(&g_cs);
    if (!g_logf) {
        char p[MAX_PATH];
        g_logf = fopen(ee_beside_program("ee.log", p), "a");
        if (g_logf)
            fprintf(g_logf, "---- ee.log opened, pid %lu\n", GetCurrentProcessId());
    }
    if (g_logf) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_logf, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                GetCurrentThreadId());
        va_list a;
        va_start(a, fmt);
        vfprintf(g_logf, fmt, a);
        va_end(a);
        fputc('\n', g_logf);
        fflush(g_logf);
    }
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------- ini
extern "C" int ee_ini_int(const char *key, int def) {
    char p[MAX_PATH];
    return (int)GetPrivateProfileIntA("EE", key, def, ee_beside_program("uiscale.ini", p));
}

// String from uiscale.ini [EE] (for VersionText); def when the key is missing. Returns buf.
extern "C" char *ee_ini_str(const char *key, const char *def, char *buf, size_t n) {
    char p[MAX_PATH];
    GetPrivateProfileStringA("EE", key, def, buf, (DWORD)n, ee_beside_program("uiscale.ini", p));
    return buf;
}

// ---------------------------------------------------------------- patch
extern "C" bool ee_program_known(void) {
    if (g_known >= 0)
        return g_known == 1;
    g_base = (uintptr_t)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)g_base;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(g_base + dos->e_lfanew);
    // The build is identified by its PE timestamp plus the code bytes at one interface site. The
    // header fields (image size, entry point) are compared too, but a difference there only makes
    // the program a "variant" of the same build: every patch still checks its own
    // exact bytes in ee_patch, so a site that differs is skipped and logged, nothing else.
    // uiscale.ini [EE] StrictProgram=1 brings back the all-or-nothing header check.
    bool build = dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE &&
                 nt->FileHeader.TimeDateStamp == KNOWN_TIMESTAMP &&
                 !memcmp((const void *)(g_base + 0x4e8f6b - KNOWN_BASE), KNOWN_4E8F6B, sizeof KNOWN_4E8F6B);
    bool header = build && nt->OptionalHeader.SizeOfImage == KNOWN_SIZEOFIMAGE &&
                  nt->OptionalHeader.AddressOfEntryPoint == KNOWN_ENTRY &&
                  !memcmp((const void *)(g_base + KNOWN_ENTRY), KNOWN_ENTRY_BYTES, sizeof KNOWN_ENTRY_BYTES);
    bool ok = header || (build && ee_ini_int("StrictProgram", 0) != 1);
    g_known = ok ? 1 : 0;
    ee_log("program check: base %08lx, timestamp %08lx, size %08lx -> %s", (unsigned long)g_base,
           (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage,
           header ? "KNOWN Enhanced Edition 5291a092"
                  : ok ? "VARIANT of the R1.28.8 build (header differs): each patch checks its own bytes"
                       : "UNKNOWN program: no patches will be written");
    return ok;
}

// addr = address at the program's preferred base 0x400000 (as Ghidra shows it); moved if the
// program was loaded elsewhere.
extern "C" bool ee_patch(uint32_t addr, const void *expect, const void *put, size_t n, const char *name) {
    if (!name)
        name = "?";
    if (!ee_program_known()) {
        ee_log("patch %s @%08x: REFUSED (unknown program)", name, addr);
        return false;
    }
    unsigned char *p = (unsigned char *)(g_base + (addr - KNOWN_BASE));
    EnterCriticalSection(&g_cs);
    bool r = false;
    if (IsBadReadPtr(p, n)) {
        ee_log("patch %s @%08x: FAILED (not readable)", name, addr);
    } else if (!memcmp(p, put, n)) {
        ee_log("patch %s @%08x (%u bytes): already in place", name, addr, (unsigned)n);
        r = true;
    } else if (memcmp(p, expect, n)) {
        char got[3 * 16 + 1] = "", want[3 * 16 + 1] = "";
        for (size_t i = 0; i < n && i < 16; i++) {
            sprintf(got + 3 * i, "%02x ", p[i]);
            sprintf(want + 3 * i, "%02x ", ((const unsigned char *)expect)[i]);
        }
        ee_log("patch %s @%08x: SKIPPED, bytes differ (found %s| expected %s)", name, addr, got, want);
    } else {
        DWORD old;
        if (VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(p, put, n);
            VirtualProtect(p, n, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, n);
            r = true;
            ee_log("patch %s @%08x (%u bytes): applied", name, addr, (unsigned)n);
        } else {
            ee_log("patch %s @%08x: FAILED (VirtualProtect error %lu)", name, addr, GetLastError());
        }
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

// ---------------------------------------------------------------- stand-ins for missing modules
#ifndef EE_HAVE_SCALE3
extern "C" void ee_scale3_init(void) { ee_log("scale3: module not built in"); }
#endif
#ifndef EE_HAVE_SMALL
extern "C" void ee_small_init(void) { ee_log("small: module not built in"); }
#endif
#ifndef EE_HAVE_BRAND
extern "C" void ee_brand_init(void) { ee_log("brand: module not built in"); }
#endif
// d3d hooks (src/ee/d3dhook.cpp): gets the real IDirect3D9 right after creation
#ifndef EE_HAVE_MENUS
extern "C" void ee_menus_init(void) { ee_log("menus: module not built in"); }
#endif
#ifndef EE_HAVE_PROFILE
extern "C" void ee_profile_init(void) { ee_log("profile: module not built in"); }
#endif
#ifndef EE_HAVE_D3DHOOK
extern "C" void ee_d3d_created(void *d3d9, int ex) { (void)d3d9; (void)ex; }
extern "C" void ee_d3d_init(void) {}
#else
extern "C" void ee_d3d_created(void *d3d9, int ex);
extern "C" void ee_d3d_init(void);
#endif

// ---------------------------------------------------------------- next d3d9 + exports
static HMODULE g_next;

static HMODULE load_next() {
    if (g_next)
        return g_next;
    char p[MAX_PATH];
    GetModuleFileNameA(g_self, p, MAX_PATH);
    char *s = strrchr(p, '\\');
    if (s) {
        strcpy(s + 1, "d3d9_next.dll");
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES)
            g_next = LoadLibraryA(p);
    }
    if (!g_next) {
        GetSystemDirectoryA(p, MAX_PATH);
        strcat(p, "\\d3d9.dll");
        g_next = LoadLibraryA(p);
    }
    ee_log("next d3d9: %s at %p%s", p, (void *)g_next, g_next ? "" : " (LoadLibrary FAILED)");
    return g_next;
}

static FARPROC next_proc(const char *name) {
    HMODULE m = load_next();
    FARPROC f = m ? GetProcAddress(m, name) : nullptr;
    if (!f)
        ee_log("next d3d9 has no %s", name);
    return f;
}

struct IDirect3D9;
extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk) {
    typedef IDirect3D9 *(WINAPI * F)(UINT);
    static F f = (F)next_proc("Direct3DCreate9");
    IDirect3D9 *d = f ? f(sdk) : nullptr;
    ee_log("Direct3DCreate9(%u) -> %p", sdk, (void *)d);
    if (d)
        ee_d3d_created(d, 0);
    return d;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, void **out) {
    typedef HRESULT(WINAPI * F)(UINT, void **);
    static F f = (F)next_proc("Direct3DCreate9Ex");
    HRESULT r = f ? f(sdk, out) : E_NOTIMPL;
    ee_log("Direct3DCreate9Ex(%u) -> %08lx", sdk, (unsigned long)r);
    if (SUCCEEDED(r) && out && *out)
        ee_d3d_created(*out, 1);
    return r;
}

extern "C" int WINAPI D3DPERF_BeginEvent(DWORD col, LPCWSTR name) {
    typedef int(WINAPI * F)(DWORD, LPCWSTR);
    static F f = (F)next_proc("D3DPERF_BeginEvent");
    return f ? f(col, name) : 0;
}
extern "C" int WINAPI D3DPERF_EndEvent(void) {
    typedef int(WINAPI * F)(void);
    static F f = (F)next_proc("D3DPERF_EndEvent");
    return f ? f() : 0;
}
extern "C" void WINAPI D3DPERF_SetMarker(DWORD col, LPCWSTR name) {
    typedef void(WINAPI * F)(DWORD, LPCWSTR);
    static F f = (F)next_proc("D3DPERF_SetMarker");
    if (f)
        f(col, name);
}
extern "C" void WINAPI D3DPERF_SetRegion(DWORD col, LPCWSTR name) {
    typedef void(WINAPI * F)(DWORD, LPCWSTR);
    static F f = (F)next_proc("D3DPERF_SetRegion");
    if (f)
        f(col, name);
}
extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    typedef BOOL(WINAPI * F)(void);
    static F f = (F)next_proc("D3DPERF_QueryRepeatFrame");
    return f ? f() : FALSE;
}
extern "C" void WINAPI D3DPERF_SetOptions(DWORD o) {
    typedef void(WINAPI * F)(DWORD);
    static F f = (F)next_proc("D3DPERF_SetOptions");
    if (f)
        f(o);
}
extern "C" DWORD WINAPI D3DPERF_GetStatus(void) {
    typedef DWORD(WINAPI * F)(void);
    static F f = (F)next_proc("D3DPERF_GetStatus");
    return f ? f() : 0;
}

// ---------------------------------------------------------------- entry
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
        char p[MAX_PATH];
        GetModuleFileNameA(NULL, p, MAX_PATH);
        ee_log("ee d3d9 proxy loaded into %s (built %s %s)", p, __DATE__, __TIME__);
        if (ee_program_known()) {
            // first, before small.cpp converts profiles (Buffers 20 -> 64) and before any game code:
            // Soldiers.exe imports d3d9.dll, so this DllMain runs before the program's entry point
            ee_profile_init();
            ee_scale3_init();
            ee_small_init();
            ee_d3d_init();
            ee_brand_init();
            ee_menus_init();   // main menu without MULTI PLAYER / MODS
        }
        ee_log("init done");
    } else if (reason == DLL_PROCESS_DETACH) {
        ee_log("unloading");
    }
    return TRUE;
}
