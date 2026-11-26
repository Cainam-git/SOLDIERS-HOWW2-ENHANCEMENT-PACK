// Tested in the game: logo skip; Max FPS list "V-Sync/240/120/60/30" (menu + mission: 240 row shows 240.0,
// 120 row 119.7-120); Buffers 5/10/15/20/64 on the Audio page, no crash, APPLY keeps 64. Not yet run in the game:
// 60/30/V-Sync rows in a mission, stuck scroll, RESTORE DEFAULTS, camera zoom, keys + 64 for a new profile, the
// sound budget with sound on.
// src/ee/small.cpp: the small fixes of the Enhancement Pack on the Steam Enhanced Edition program
// (Soldiers.exe 5291a092, R1.28.8). All changes are made in memory at start; the program file is untouched.
// Every fix checks ALL its program bytes first (exact match, addresses at base 0x400000, moved with the
// program if it loads elsewhere) and writes nothing when any of them differs: then the game's own
// behaviour stays. uiscale.ini, section [EE]:
//
//   FpsList=1        Options > Video > Max FPS offers V-Sync (the game's own "-" row), 240, 120, 60, 30.
//   SoundVoices=64   Options > Audio > Buffers offers 64 too (sounds at once in battle; the game's most is 20),
//                    the page no longer crashes on other values, and profiles at 20 are raised to 64 once.
//   ScrollFix=1      a camera key let go while direct control has it also stops the camera scroll.
//   KeysDefaults=1   RESTORE DEFAULTS on the KEYS page gives our modern WASD layout.
//   NewProfileKeys=1 a NEW profile gets our modern WASD layout (an existing keys file is never touched).
//   CameraZoom=1     camera distance 700..3000, wheel step 150 (CameraMinHeight/CameraMaxHeight/CameraZoomStep).
//   SkipLogos=1      the four company logo videos at start are not played.
//
// Addresses: every one re-read from the program for this file.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include "ee.h"
#include "../keys/modern_keys.inc"

namespace {

uint32_t g_delta;   // where the program really is minus 0x400000

inline uint32_t R(uint32_t a) { return a + g_delta; }                      // program address, moved
inline unsigned char *P(uint32_t a) { return (unsigned char *)(uintptr_t)R(a); }

void put32(unsigned char *b, uint32_t v) { memcpy(b, &v, 4); }
uint32_t rel32(uint32_t from_addr, uint32_t insn_len, const void *to) {   // from_addr = program address
    return (uint32_t)(uintptr_t)to - (R(from_addr) + insn_len);
}

// A patch site: program address, bytes expected, bytes to put.
struct Site {
    uint32_t addr;
    unsigned char exp[64], put[64];
    size_t n;
};

// "found .. | expected .." for a log line (first 16 bytes)
void hex2(const unsigned char *found, const unsigned char *want, size_t n, char *out) {
    out[0] = 0;
    char *o = out;
    o += sprintf(o, "found ");
    for (size_t i = 0; i < n && i < 16; i++) o += sprintf(o, "%02x ", found[i]);
    o += sprintf(o, "| expected ");
    for (size_t i = 0; i < n && i < 16; i++) o += sprintf(o, "%02x ", want[i]);
}

// all sites hold the expected bytes (or all already hold the new ones)?
bool sites_ok(const Site *s, int k, const char *name) {
    for (int i = 0; i < k; i++) {
        const unsigned char *p = P(s[i].addr);
        if (IsBadReadPtr(p, s[i].n) || (memcmp(p, s[i].exp, s[i].n) && memcmp(p, s[i].put, s[i].n))) {
            char hx[128] = "not readable";
            if (!IsBadReadPtr(p, s[i].n)) hex2(p, s[i].exp, s[i].n, hx);
            ee_log("%s: bytes at %08x differ from the known program: NOT applied (game's own behaviour) (%s)", name, s[i].addr, hx);
            return false;
        }
    }
    return true;
}

// context bytes that must be there (not changed)
bool ctx_ok(uint32_t addr, const void *b, size_t n, const char *name) {
    const unsigned char *p = P(addr);
    if (IsBadReadPtr(p, n) || memcmp(p, b, n)) {
        char hx[128] = "not readable";
        if (!IsBadReadPtr(p, n)) hex2(p, (const unsigned char *)b, n, hx);
        ee_log("%s: context bytes at %08x differ: NOT applied (%s)", name, addr, hx);
        return false;
    }
    return true;
}

bool apply_sites(const Site *s, int k, const char *name) {
    if (!sites_ok(s, k, name))
        return false;
    bool all = true;
    for (int i = 0; i < k; i++)
        all &= ee_patch(s[i].addr, s[i].exp, s[i].put, s[i].n, name);
    return all;
}

Site site(uint32_t addr, std::initializer_list<int> e, std::initializer_list<int> p) {
    Site s = {};
    s.addr = addr;
    s.n = e.size();
    int i = 0;
    for (int v : e) s.exp[i++] = (unsigned char)v;
    i = 0;
    for (int v : p) s.put[i++] = (unsigned char)v;
    return s;
}

// ---------------------------------------------------------------------------------------------
// 1. Max FPS list: "-", 240, 120, 60, 30
// ---------------------------------------------------------------------------------------------
// The page fills the list with 14 blocks (0x523d44 "-", 0x523d73 "100", ... 0x524013 "7,5"); each block
// builds a string from a label, adds it (0x4fe0a0) and destroys the string. Row = stored value
// (options video/maxFPS): page open 0x527520 selects row = value (no upper bound check: 0x661960),
// page save 0x528e61 stores the selected row. The limiter 0x757dc0 turns the value into milliseconds
// (jump table 0x757f18, 14 cases, float constants); value 0 also means V-Sync on (0x4e8a7c).
// New: rows "-", "240", "120", "60", "30" with the stored values 0, 1, 2, 6, 8. 6 and 8 are the
// game's own 60 and 30; 1 and 2 (the game's 100 and 90) now mean 240 and 120 (4.1667 / 8.3333 ms), so
// without the pack such a profile simply runs at 100 / 90. Values of rows that are gone show and run
// as the nearest row: 80, 75, 70, 50 -> 60; 25 and lower -> 30; unknown values above 13 -> 240
// (the game itself runs those unlimited without V-Sync).
const char g_s240[] = "240", g_s120[] = "120", g_svs[] = "V-Sync";
const float g_f240 = 1000.0f / 240.0f, g_f120 = 1000.0f / 120.0f;
}   // namespace

extern "C" {
// stored value -> row (values 0..13); row -> stored value (rows 0..4)
unsigned char g_h68_v2r[14] = {0, 1, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4};
unsigned char g_h68_r2v[5] = {0, 1, 2, 6, 8};
void h68_fps_open(void);
void h68_fps_save(void);
}
// at 0x527520 (replaces mov ecx,[edi+0xe0]): eax = stored value in, row out; ecx = the list
asm(".text\n.globl _h68_fps_open\n_h68_fps_open:\n"
    "    movl 0xe0(%edi), %ecx\n"
    "    testl %eax, %eax\n"
    "    js 1f\n"                   // negative: unchanged (nothing selected, as the game)
    "    cmpl $13, %eax\n"
    "    ja 2f\n"
    "    movzbl _g_h68_v2r(%eax), %eax\n"
    "    ret\n"
    "2:  movl $1, %eax\n"
    "1:  ret\n");
// at 0x528e61 (replaces mov eax,[eax+0xc8]): eax = list in, stored value out
asm(".text\n.globl _h68_fps_save\n_h68_fps_save:\n"
    "    movl 0xc8(%eax), %eax\n"
    "    cmpl $4, %eax\n"
    "    ja 1f\n"                   // -1 (nothing selected) or unknown: unchanged
    "    movzbl _g_h68_r2v(%eax), %eax\n"
    "1:  ret\n");

namespace {
bool fix_fps_list() {
    Site s[16];
    int k = 0;
    // labels: "100" -> "240", "90" -> "120", "80" -> "60", "75" -> "30"; then skip the other 9 blocks
    auto push_site = [&](uint32_t a, uint32_t from, uint32_t to) {
        Site x = {};
        x.addr = a; x.n = 5;
        x.exp[0] = 0x68; put32(x.exp + 1, from);
        x.put[0] = 0x68; put32(x.put + 1, to);
        s[k++] = x;
    };
    push_site(0x523d44, R(0x839528), (uint32_t)(uintptr_t)g_svs);     // "-" (V-Sync, the game's own row) -> "V-Sync"
    push_site(0x523d73, R(0x83952c), (uint32_t)(uintptr_t)g_s240);
    push_site(0x523dab, R(0x839530), (uint32_t)(uintptr_t)g_s120);
    push_site(0x523de3, R(0x839534), R(0x839540));
    push_site(0x523e1b, R(0x839538), R(0x839548));
    {   // block "70" -> jmp 0x52404b (first instruction after the "7,5" block)
        Site x = {};
        x.addr = 0x523e53; x.n = 5;
        x.exp[0] = 0x68; put32(x.exp + 1, R(0x83953c));
        x.put[0] = 0xe9; put32(x.put + 1, 0x52404b - (0x523e53 + 5));
        s[k++] = x;
    }
    {   // page open: call h68_fps_open; nop
        Site x = site(0x527520, {0x8b, 0x8f, 0xe0, 0x00, 0x00, 0x00}, {0xe8, 0, 0, 0, 0, 0x90});
        put32(x.put + 1, rel32(0x527520, 5, (void *)h68_fps_open));
        s[k++] = x;
    }
    {   // page save: call h68_fps_save; nop
        Site x = site(0x528e61, {0x8b, 0x80, 0xc8, 0x00, 0x00, 0x00}, {0xe8, 0, 0, 0, 0, 0x90});
        put32(x.put + 1, rel32(0x528e61, 5, (void *)h68_fps_save));
        s[k++] = x;
    }
    auto movss_site = [&](uint32_t a, uint32_t from, const float *to) {
        Site x = {};
        x.addr = a; x.n = 8;
        const unsigned char op[4] = {0xf3, 0x0f, 0x10, 0x05};
        memcpy(x.exp, op, 4); put32(x.exp + 4, from);
        memcpy(x.put, op, 4); put32(x.put + 4, (uint32_t)(uintptr_t)to);
        s[k++] = x;
    };
    movss_site(0x757e46, R(0x82b158), &g_f240);   // case 1: 10 ms -> 4.1667 ms
    movss_site(0x757e53, R(0x85530c), &g_f120);   // case 2: 11.11 ms -> 8.3333 ms
    {   // jump table: cases 3,4,5,7 -> case 6 (60), cases 9..13 -> case 8 (30)
        static const uint32_t tab[14] = {0x757ed3, 0x757e46, 0x757e53, 0x757e5d, 0x757e67, 0x757e71, 0x757e7b,
                                         0x757e85, 0x757e8f, 0x757e99, 0x757ea3, 0x757ead, 0x757eb7, 0x757ec1};
        Site x = {};
        x.addr = 0x757f18; x.n = 56;
        for (int i = 0; i < 14; i++) {
            uint32_t to = tab[i];
            if (i == 3 || i == 4 || i == 5 || i == 7) to = 0x757e7b;
            if (i >= 9) to = 0x757e8f;
            put32(x.exp + 4 * i, R(tab[i]));
            put32(x.put + 4 * i, R(to));
        }
        s[k++] = x;
    }
    // context: the end of the list fill, the limiter's switch, the V-Sync test
    unsigned char c1[5] = {0x68}; put32(c1 + 1, R(0x839560));
    unsigned char c2full[16] = {0x83, 0xf8, 0x0d, 0x0f, 0x87, 0x99, 0x00, 0x00, 0x00, 0xff, 0x24, 0x85};
    put32(c2full + 12, R(0x757f18));
    const unsigned char c3[] = {0x83, 0xbd, 0x00, 0xff, 0xff, 0xff, 0x00, 0x0f, 0x94, 0x85, 0x06, 0xff, 0xff, 0xff};
    if (!ctx_ok(0x52404b, c1, 5, "fps list") || !ctx_ok(0x757e36, c2full, 16, "fps list") ||
        !ctx_ok(0x4e8a7c, c3, sizeof c3, "fps list"))
        return false;
    bool r = apply_sites(s, k, "fps list");
    if (r)
        ee_log("fps list: Max FPS rows V-Sync, 240, 120, 60, 30 (stored 0, 1, 2, 6, 8) applied");
    return r;
}

// ---------------------------------------------------------------------------------------------
// shared: MSVC std::string as the program uses it, and the profiles folder
// ---------------------------------------------------------------------------------------------
struct MsStr {
    union { char buf[16]; char *ptr; };
    uint32_t size, cap;
    const char *c() const { return cap >= 16 ? ptr : buf; }
};

// AppData\Local\Soldiers Heroes of World War 2\profiles\ (the program: SHGetFolderPathA(CSIDL_LOCAL_APPDATA)
// + "\Soldiers Heroes of World War 2", 0x6f8550..; VFS "@/profiles/"). From the environment first (works in
// DllMain); SHGetFolderPathA only later, from game code.
std::string profiles_dir(bool may_load) {
    char d[MAX_PATH] = "";
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", d, sizeof d);
    if (!n || n >= sizeof d) {
        d[0] = 0;
        if (!may_load) return std::string();
        typedef HRESULT(WINAPI * F)(HWND, int, HANDLE, DWORD, LPSTR);
        HMODULE sh = GetModuleHandleA("shell32.dll");
        if (!sh) sh = LoadLibraryA("shell32.dll");
        F f = sh ? (F)GetProcAddress(sh, "SHGetFolderPathA") : nullptr;
        if (!f || f(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, d) != S_OK || !*d) return std::string();
    }
    return std::string(d) + "\\Soldiers Heroes of World War 2\\profiles\\";
}

bool file_exists(const std::string &p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool read_text(const std::string &path, std::string &out) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, NULL), got = 0;
    bool ok = size != INVALID_FILE_SIZE && size < (1u << 20);
    if (ok) { out.assign(size, '\0'); ok = !size || (ReadFile(f, &out[0], size, &got, NULL) && got == size); }
    CloseHandle(f);
    return ok;
}

bool write_text(const std::string &path, const char *data, size_t n) {   // temp file + rename
    std::string tmp = path + ".ee-tmp";
    HANDLE o = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (o == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(o, data, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(o);
    if (ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileA(tmp.c_str());   // our own temp file only
    return false;
}

// ---------------------------------------------------------------------------------------------
// 2. More sounds at once (SoundVoices=N, 21..256; default 64)
// ---------------------------------------------------------------------------------------------
// The sound (re)start 0x4eef90 (game start, Options APPLY, mission load) reads the profile value
// options/sound/buffers with NO upper limit and stores it at 0x4ef022 into the sound manager 0x8e5928 (+0x3c),
// the budget of sounds playing at once (read by 0x4ed790). So the profile value itself is used; what stops a
// player from having 64 is only the Options > Audio page:
//  * its Buffers list holds 5/10/15/20 (blocks 0x52462b..0x524776, each: label + value string, add item 0x4fe0a0,
//    value 0x4fb0c0);
//  * opening Options selects the row whose value equals the stored text (0x527dc2 -> 0x4c3400 -> 0x6608d0); no
//    match = nothing selected;
//  * the save that runs when Options opens and on APPLY (0x528bb0) takes the selected row's value
//    (0x5295cc call 0x660dd0) and reads through it: nothing selected = null + 0x28 -> access violation at
//    0x5295d4 (seen in a test run: profile buffers "64", click OPTIONS).
// Changes: (a) one more row, "N", added after "20" (a copy of the "20" block in dll memory, entered from 0x524777);
// (b) if the stored value matches no row, the nearest row is selected (wrapper around the call at 0x527dc2);
// (c) the save never reads through a null row (wrapper around the call at 0x5295cc; cannot happen after (b));
// (d) profiles still holding the game's top value "20" get "N" once (marker file buffers.ee in the profile;
//     a later choice on the page is kept); new profiles get it when the game writes them. Any spacing around the
//     value counts (the default template has tabs); the marker is honest: see convert_buffers.
int g_voices;
char g_voices_txt[8];
int g_rows[5] = {5, 10, 15, 20, 64};
struct FakeItem { char pad[0x28]; MsStr s; };
FakeItem g_fake_item;
typedef void(__attribute__((thiscall)) *selstr_t)(void *, const MsStr *, int);
typedef void(__attribute__((thiscall)) *selrow_t)(void *, int);
typedef void *(__attribute__((thiscall)) *item_t)(void *, int);

void __attribute__((thiscall)) h68_buffers_select(void *combo, const MsStr *s, int flag) {
    ((selstr_t)(uintptr_t)R(0x4c3400))(combo, s, flag);
    char *list = combo ? *(char **)((char *)combo + 0xb4) : nullptr;
    if (!list || *(int *)(list + 0xc8) >= 0) return;
    int v = s ? atoi(s->c()) : 20, best = 0;
    for (int i = 1; i < 5; i++)
        if (abs(g_rows[i] - v) < abs(g_rows[best] - v)) best = i;
    ((selrow_t)(uintptr_t)R(0x4c2f40))(combo, best);
    ee_log("sound: stored Buffers \"%s\" is not in the list: page shows the nearest row %d", s ? s->c() : "?", g_rows[best]);
}

void *__attribute__((thiscall)) h68_buffers_item(void *list, int idx) {
    void *r = ((item_t)(uintptr_t)R(0x660dd0))(list, idx);
    if (r) return r;
    ee_log("sound: Buffers page had no row selected: saved \"%s\" instead of reading through nothing", g_fake_item.s.c());
    return &g_fake_item;
}

// The Buffers entry with any spacing. The game's own APPLY writes {buffers "20"},
// its default template {buffers<TAB>"20"<TAB>}; the earlier exact-text match missed the template. Finds
// "{buffers" + spaces/tabs + "digits" + spaces/tabs + "}"; vpos/vlen = the digits. False when there is none.
bool find_buffers(const std::string &t, size_t &vpos, size_t &vlen) {
    for (size_t p = t.find('{'); p != std::string::npos; p = t.find('{', p + 1)) {
        if (_strnicmp(t.c_str() + p + 1, "buffers", 7)) continue;
        size_t i = p + 8;
        if (i >= t.size() || (t[i] != ' ' && t[i] != '\t' && t[i] != '"')) continue;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
        if (i >= t.size() || t[i] != '"') continue;
        size_t a = ++i;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') i++;
        if (i == a || i >= t.size() || t[i] != '"') continue;
        size_t b = i++;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
        if (i >= t.size() || t[i] != '}') continue;
        vpos = a; vlen = b - a;
        return true;
    }
    return false;
}

// options text of one profile: Buffers "20" -> "N" once; marker buffers.ee.
// Marker rule: the marker is written only after the Buffers value was really read (and, at 20,
// really changed). A marker of this build ("sound check 2") or an old one that says "20 -> N" (that profile WAS
// converted, so a 20 there now is the player's own choice) is trusted: the profile is left alone. An old
// "kept (not 20)" marker is not trusted (the earlier build wrote it also when it had missed a template "20"):
// that profile is checked once more; a 20 found then becomes N, any other value is kept; then the new marker.
void convert_buffers(const std::string &dir, const char *name, const char *when) {
    std::string mk;
    bool old_mk = file_exists(dir + "buffers.ee");
    if (old_mk && read_text(dir + "buffers.ee", mk) &&
        (mk.find("sound check 2") != std::string::npos || mk.find("20 -> N") != std::string::npos))
        return;
    std::string opt;
    if (!read_text(dir + "options", opt)) { ee_log("sound: profile %s (%s): no readable options file: Buffers checked at a later start", name, when); return; }
    size_t vp = 0, vl = 0;
    if (!find_buffers(opt, vp, vl)) { ee_log("sound: profile %s (%s): options has no Buffers entry: nothing changed, checked again at the next start", name, when); return; }
    std::string val = opt.substr(vp, vl), what;
    if (val == "20") {
        opt.replace(vp, vl, g_voices_txt);
        if (!write_text(dir + "options", opt.data(), opt.size())) { ee_log("sound: profile %s: options not writable", name); return; }
        what = std::string("20 -> ") + g_voices_txt;
    } else {
        what = "kept at " + val;
    }
    char m[200];
    int n = snprintf(m, sizeof m, "sound check 2: buffers %s by the Enhancement Pack; a value chosen on the Audio page is kept\r\n", what.c_str());
    if (!write_text(dir + "buffers.ee", m, (size_t)n)) ee_log("sound: profile %s: marker buffers.ee COULD NOT be written (checked again next start)", name);
    ee_log("sound: profile %s (%s): Buffers %s%s", name, when, what.c_str(), old_mk ? " (old marker of an earlier build re-checked)" : "");
}

void convert_all_profiles() {
    std::string base = profiles_dir(false);
    if (base.empty()) { ee_log("sound: no LOCALAPPDATA: existing profiles not converted"); return; }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((base + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        convert_buffers(base + fd.cFileName + "\\", fd.cFileName, "at start");
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void *g_stub;   // the extra list row, executable copy of the "20" block

bool fix_sound(int n) {
    g_voices = n;
    snprintf(g_voices_txt, sizeof g_voices_txt, "%d", n);
    g_rows[4] = n;
    memset(&g_fake_item, 0, sizeof g_fake_item);
    strcpy(g_fake_item.s.buf, "20"); g_fake_item.s.size = 2; g_fake_item.s.cap = 15;
    // (a) the "20" block 0x524724..0x524776, then add [ebp-0x1f4],5 at 0x524777
    const uint32_t B = 0x524724, BLEN = 0x53;
    unsigned char blk[BLEN];
    const unsigned char *pb = P(B);
    if (IsBadReadPtr(pb, BLEN + 7)) return false;
    memcpy(blk, pb, BLEN);
    unsigned char exp_push[5] = {0x68}; put32(exp_push + 1, R(0x839550));            // push "20"
    const int calls[6] = {0x08, 0x19, 0x30, 0x3a, 0x42, 0x4e};
    const uint32_t targets[6] = {0x421a50, 0x421a50, 0x4fe0a0, 0x4fb0c0, 0x4234a0, 0x4234a0};
    bool ok = !memcmp(blk, exp_push, 5) && !memcmp(blk + 0x0d, exp_push, 5);
    for (int i = 0; i < 6 && ok; i++) {
        uint32_t rel; memcpy(&rel, blk + calls[i] + 1, 4);
        ok = blk[calls[i]] == 0xe8 && R(B) + calls[i] + 5 + rel == R(targets[i]);
    }
    const unsigned char tail[7] = {0x83, 0x85, 0x0c, 0xfe, 0xff, 0xff, 0x05};
    unsigned char after[5] = {0x68}; put32(after + 1, R(0x8396b4));                    // next control's label
    if (!ok || memcmp(pb + BLEN, tail, 7) || !ctx_ok(0x52477e, after, 5, "sound")) {
        ee_log("sound: Buffers list bytes differ: NOT applied (game's own behaviour)");
        return false;
    }
    unsigned char opn[7] = {0x8b, 0x8f, 0xfc, 0x00, 0x00, 0x00, 0x50};               // 0x527dbb mov ecx,[edi+0xfc]; push eax
    unsigned char sav[6] = {0x6a, 0xff}; sav[2] = 0xc6; sav[3] = 0x45; sav[4] = 0xfc; sav[5] = 0x1c;   // 0x5295c0
    unsigned char savtail[11] = {0x83, 0xc0, 0x28, 0x83, 0x78, 0x14, 0x10, 0x72, 0x02, 0x8b, 0x00};      // 0x5295d1
    if (!ctx_ok(0x527dbb, opn, 7, "sound") || !ctx_ok(0x5295c0, sav, 6, "sound") || !ctx_ok(0x5295d1, savtail, 11, "sound"))
        return false;
    // Review finding: h68_buffers_select calls the row selector 0x4c2f40 (thiscall, 1 argument, ret 4).
    // The game's own call to it at 0x527527 (push eax; call 0x4c2f40) is checked here, so 0x4c2f40 is known too.
    unsigned char selrow[6] = {0x50, 0xe8};
    put32(selrow + 2, 0x4c2f40 - (0x527527 + 5));
    if (!ctx_ok(0x527526, selrow, 6, "sound"))
        return false;
    unsigned char *stub = (unsigned char *)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return false;
    memcpy(stub, blk, BLEN);
    put32(stub + 1, (uint32_t)(uintptr_t)g_voices_txt);
    put32(stub + 0x0e, (uint32_t)(uintptr_t)g_voices_txt);
    for (int i = 0; i < 6; i++) put32(stub + calls[i] + 1, R(targets[i]) - ((uint32_t)(uintptr_t)stub + calls[i] + 5));
    memcpy(stub + BLEN, tail, 7);
    stub[BLEN + 7] = 0xe9;
    put32(stub + BLEN + 8, R(0x52477e) - ((uint32_t)(uintptr_t)stub + BLEN + 12));
    FlushInstructionCache(GetCurrentProcess(), stub, 256);
    g_stub = stub;
    Site s[3];
    s[0] = site(0x524777, {0x83, 0x85, 0x0c, 0xfe, 0xff, 0xff, 0x05}, {0xe9, 0, 0, 0, 0, 0x90, 0x90});
    put32(s[0].put + 1, rel32(0x524777, 5, stub));
    s[1] = site(0x527dc2, {0xe8, 0, 0, 0, 0}, {0xe8, 0, 0, 0, 0});
    put32(s[1].exp + 1, 0x4c3400 - (0x527dc2 + 5));
    put32(s[1].put + 1, rel32(0x527dc2, 5, (void *)h68_buffers_select));
    s[2] = site(0x5295cc, {0xe8, 0, 0, 0, 0}, {0xe8, 0, 0, 0, 0});
    put32(s[2].exp + 1, 0x660dd0 - (0x5295cc + 5));
    put32(s[2].put + 1, rel32(0x5295cc, 5, (void *)h68_buffers_item));
    bool r = apply_sites(s, 3, "sound");
    if (r) {
        ee_log("sound: Buffers list 5/10/15/20/%d, page safe for any stored value", n);
        convert_all_profiles();
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// 3. Camera scroll that keeps going after a key is let go
// ---------------------------------------------------------------------------------------------
// Key-release handler 0x59ed10 (eGameControl): when the active control mode ([this+0x2c], vtable +0x24,
// direct control) takes the key, `jne 0x59f175` at 0x59ee19 returns without the camera-key release
// 0x59d630(key, 0) at 0x59f16b. Same flaw as the old program (0x7752c1 / 0x775b50). The jne now goes to
// 0x59f16b: the release call with edi = key, esi = this, then the function's normal end.
bool fix_scroll() {
    Site s = site(0x59ee19, {0x0f, 0x85, 0x56, 0x03, 0x00, 0x00}, {0x0f, 0x85, 0x4c, 0x03, 0x00, 0x00});
    const unsigned char before[] = {0x8b, 0x4e, 0x2c, 0x85, 0xc9, 0x74, 0x10, 0x8b, 0x01, 0x57, 0x8b, 0x40, 0x24, 0xff, 0xd0, 0x84, 0xc0};
    unsigned char rel[10] = {0x6a, 0x00, 0x57, 0x8b, 0xce, 0xe8};
    put32(rel + 6, 0x59d630 - (0x59f170 + 5));
    const unsigned char tail[] = {0x8b, 0x4d, 0xf4};
    if (!ctx_ok(0x59ee08, before, sizeof before, "scroll fix") || !ctx_ok(0x59f16b, rel, 10, "scroll fix") ||
        !ctx_ok(0x59f175, tail, 3, "scroll fix") || !ctx_ok(0x59ed42, "\x8b\x7d\x0c", 3, "scroll fix"))
        return false;
    bool r = apply_sites(&s, 1, "scroll fix");
    if (r)
        ee_log("scroll fix: on (a key let go during direct control also stops the camera scroll)");
    return r;
}

// ---------------------------------------------------------------------------------------------
// 4 + 7. Our modern key layout: RESTORE DEFAULTS and new profiles
// ---------------------------------------------------------------------------------------------
// Profiles: SHGetFolderPathA(CSIDL_LOCAL_APPDATA) + "\Soldiers Heroes of World War 2" (0x6f8550..) +
// "\profiles\<name>\keys" (VFS "@/profiles/"). Program functions (MSVC std::string arguments):
//   0x770190 cdecl  find profile by name (list 0x8ebaf0..0x8ebaf4), 0 if none
//   0x76da70 this   Load(mask) (1 = keys, 7 = all), ret 4
//   0x76e7e0 this   Save(mask, std::string *name), ret 8
//   0x7716c0 cdecl  RESTORE DEFAULTS core: profile "default" (string 0x8c71b8) Load(1), Save(1, current
//                   name 0x8c71c0); called only at 0x659370 from the KEYS page handler 0x659160, which
//                   parks "keys" as "keys_current" around it and rebuilds the page list after it.
//   0x76f760 cdecl  create profile: "default" Load(7) then Save(7, new name) at 0x76fa9d.
typedef void(__cdecl *restore_t)(void);
typedef void *(__cdecl *find_t)(const MsStr *);
typedef void(__attribute__((thiscall)) *load_t)(void *, int);
typedef void(__attribute__((thiscall)) *save_t)(void *, int, const MsStr *);

std::string profile_keys_path(const char *name) {
    std::string d = profiles_dir(true);
    if (d.empty() || !name || !*name) return std::string();
    return d + name + "\\keys";
}

bool write_modern(const std::string &path) { return write_text(path, k_modern_keys, sizeof(k_modern_keys) - 1); }

// The KEYS page handler around this call (READ 0x659337-0x6593d3, strings "keys" 0x83fd08 and
// "keys_current" 0x84aea4): it parks the in-memory block "keys" (the bindings in use) as "keys_current"
// (0x6e99c0), calls the reset 0x7716c0, rebuilds the page list from "keys" (0x659840) and then moves
// "keys_current" back, so the bindings in use only change on APPLY. The reset itself (0x7716c0) loads the
// built-in "default" profile's keys and SAVES them into profiles/<current>/keys at once, so the game alone
// already changes the keys FILE on RESTORE DEFAULTS, even when the player then presses BACK.
// Here: the file as it was is read first; after the game's reset the modern layout is written and read into
// "keys" (that is what the page lists); then the file is put back exactly as it was. Result: the page shows
// the modern layout, APPLY writes it (the game's own APPLY), BACK leaves the file as before.
void __cdecl h68_restore_defaults() {
    const MsStr *cur = (const MsStr *)(uintptr_t)R(0x8c71c0);
    std::string path = profile_keys_path(cur->c());
    std::string before;   // the keys file before the reset, put back after the page is filled
    bool had_file = !path.empty() && file_exists(path) && read_text(path, before);
    ((restore_t)(uintptr_t)R(0x7716c0))();          // the game's own reset first (file = its defaults)
    if (path.empty() || !file_exists(path)) {
        ee_log("keys defaults: profile '%s': keys file not found (%s): game's own defaults kept", cur->c(), path.c_str());
        return;
    }
    if (!write_modern(path)) {
        ee_log("keys defaults: %s could not be written: game's own defaults kept", path.c_str());
        return;
    }
    void *prof = ((find_t)(uintptr_t)R(0x770190))(cur);
    if (!prof)
        ee_log("keys defaults: %s written, current profile not found in memory: page shows the game's defaults", path.c_str());
    else
        ((load_t)(uintptr_t)R(0x76da70))(prof, 1);  // the game reads the file into "keys" (the page's source)
    if (had_file) {   // the file goes back to what it was; only APPLY changes it
        bool ok = write_text(path, before.data(), before.size());
        ee_log("keys defaults: profile '%s': page filled with the modern layout; keys file %s (%s)", cur->c(),
               ok ? "put back as it was (APPLY saves the page)" : "COULD NOT be put back (it holds the modern layout)",
               path.c_str());
    } else {
        ee_log("keys defaults: profile '%s': page filled with the modern layout; there was no keys file before, the new one stays (%s)",
               cur->c(), path.c_str());
    }
}

bool g_newprof_on;     // keys for new profiles
bool g_newprof_hook;   // the Save call at 0x76fa9d goes through h68_newprofile_save
void __attribute__((thiscall)) h68_newprofile_save(void *prof, int mask, const MsStr *name) {
    std::string path = profile_keys_path(name ? name->c() : nullptr);
    bool existed = path.empty() || file_exists(path);
    ((save_t)(uintptr_t)R(0x76e7e0))(prof, mask, name);
    if (g_voices && !path.empty())
        convert_buffers(path.substr(0, path.size() - 4), name ? name->c() : "?", "new profile");
    if (!g_newprof_on)
        return;
    if (existed) {
        ee_log("new profile '%s': a keys file was already there: left alone", name ? name->c() : "?");
        return;
    }
    if (!file_exists(path)) {
        ee_log("new profile '%s': the game wrote no keys file (%s): nothing done", name ? name->c() : "?", path.c_str());
        return;
    }
    bool ok = write_modern(path);
    ee_log("new profile '%s': modern layout %s (%s)", name ? name->c() : "?", ok ? "written" : "COULD NOT BE WRITTEN", path.c_str());
}

// the four functions' first bytes (the calls below are only redirected when all are known)
bool keys_funcs_known(const char *name) {
    const unsigned char restore[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x1c};
    const unsigned char find[] = {0x55, 0x8b, 0xec, 0x51, 0xa1};
    unsigned char ld[6] = {0x8b, 0x15}; put32(ld + 2, R(0x8c71b8));
    unsigned char sv[5] = {0x68}; put32(sv + 1, R(0x8c71c0));
    return ctx_ok(0x7716c0, restore, sizeof restore, name) && ctx_ok(0x770190, find, sizeof find, name) &&
           ctx_ok(0x7716d0, ld, 6, name) && ctx_ok(0x77174b, sv, 5, name);
}

bool fix_keys_defaults() {
    if (!keys_funcs_known("keys defaults"))
        return false;
    // the load/save calls inside 0x7716c0 confirm the conventions (Load(1) on esi, Save(1, 0x8c71c0))
    unsigned char inner[20] = {0x6a, 0x01, 0x8b, 0xce, 0xe8};
    put32(inner + 5, 0x76da70 - (0x771746 + 5));
    inner[9] = 0x68; put32(inner + 10, R(0x8c71c0));
    inner[14] = 0x6a; inner[15] = 0x01; inner[16] = 0x8b; inner[17] = 0xce; inner[18] = 0xe8;
    if (!ctx_ok(0x771742, inner, 19, "keys defaults"))
        return false;
    Site s = site(0x659370, {0xe8, 0, 0, 0, 0}, {0xe8, 0, 0, 0, 0});
    put32(s.exp + 1, 0x7716c0 - (0x659370 + 5));
    put32(s.put + 1, rel32(0x659370, 5, (void *)h68_restore_defaults));
    bool r = apply_sites(&s, 1, "keys defaults");
    if (r)
        ee_log("keys defaults: RESTORE DEFAULTS gives the modern layout");
    return r;
}

bool install_newprofile_hook() {
    if (g_newprof_hook) return true;
    if (!keys_funcs_known("new profile hook"))
        return false;
    // 0x76fa8f push [ebp-0x94] (name); mov ecx,[ebp-0x8c] (profile "default"); push 7; call Save
    const unsigned char ctx[] = {0xff, 0xb5, 0x6c, 0xff, 0xff, 0xff, 0x8b, 0x8d, 0x74, 0xff, 0xff, 0xff, 0x6a, 0x07};
    if (!ctx_ok(0x76fa8f, ctx, sizeof ctx, "new profile hook"))
        return false;
    Site s = site(0x76fa9d, {0xe8, 0, 0, 0, 0}, {0xe8, 0, 0, 0, 0});
    put32(s.exp + 1, 0x76e7e0 - (0x76fa9d + 5));
    put32(s.put + 1, rel32(0x76fa9d, 5, (void *)h68_newprofile_save));
    g_newprof_hook = apply_sites(&s, 1, "new profile hook");
    return g_newprof_hook;
}

bool fix_new_profile_keys() {
    g_newprof_on = install_newprofile_hook();
    if (g_newprof_on)
        ee_log("new profile keys: a new profile gets the modern layout");
    return g_newprof_on;
}

// ---------------------------------------------------------------------------------------------
// 5. Camera zoom range
// ---------------------------------------------------------------------------------------------
// Camera distance limits min/max at 0x8c6560/0x8c6564 (start 1.0/100000.0), written only by the map/save
// reader (`fstp [0x8c6560]` 0x7180e9, `fstp [0x8c6564]` 0x7180f6, camera "Distance" min max) and used
// by the clamp in 0x71c870 / 0x71f96e (console variable clipmap, default 1). Wheel step: x50 at 0x71eb22
// (operand 0x71eb26 -> 50.0 at 0x82e0b8, a constant shared with other code, so only the operand moves).
// Our limits are written once; the reader's two stores go to two floats of ours, so every map and save
// keeps our range (as the old pack's camera thread did after each load).
float g_cam_min = 700.f, g_cam_max = 3000.f, g_cam_step = 150.f, g_cam_map_min, g_cam_map_max;
bool fix_camera() {
    Site s[5];
    s[0] = site(0x7180e9, {0xd9, 0x1d, 0, 0, 0, 0}, {0xd9, 0x1d, 0, 0, 0, 0});
    put32(s[0].exp + 2, R(0x8c6560)); put32(s[0].put + 2, (uint32_t)(uintptr_t)&g_cam_map_min);
    s[1] = site(0x7180f6, {0xd9, 0x1d, 0, 0, 0, 0}, {0xd9, 0x1d, 0, 0, 0, 0});
    put32(s[1].exp + 2, R(0x8c6564)); put32(s[1].put + 2, (uint32_t)(uintptr_t)&g_cam_map_max);
    s[2] = site(0x71eb22, {0xf3, 0x0f, 0x59, 0x0d, 0, 0, 0, 0}, {0xf3, 0x0f, 0x59, 0x0d, 0, 0, 0, 0});
    put32(s[2].exp + 4, R(0x82e0b8)); put32(s[2].put + 4, (uint32_t)(uintptr_t)&g_cam_step);
    const float one = 1.0f, big = 100000.0f;
    s[3] = Site{}; s[3].addr = 0x8c6560; s[3].n = 4; memcpy(s[3].exp, &one, 4); memcpy(s[3].put, &g_cam_min, 4);
    s[4] = Site{}; s[4].addr = 0x8c6564; s[4].n = 4; memcpy(s[4].exp, &big, 4); memcpy(s[4].put, &g_cam_max, 4);
    const float fifty = 50.0f;
    if (!ctx_ok(0x82e0b8, &fifty, 4, "camera zoom"))
        return false;
    bool r = apply_sites(s, 5, "camera zoom");
    if (r)
        ee_log("camera zoom: distance %.0f..%.0f, wheel step %.0f", g_cam_min, g_cam_max, g_cam_step);
    return r;
}

// ---------------------------------------------------------------------------------------------
// 6. Logo videos at start
// ---------------------------------------------------------------------------------------------
// set/guimedia.set "sting codies/1c/best way/nvidia" -> set/video.set /video/intro/1_codemasters.wmv,
// 2_1c.wmv, 3_bw.wmv, 4_nv.wmv. Every video is opened by 0x4e59f0: a DirectShow filter graph, then
// IGraphBuilder::RenderFile (vtable +0x34) at 0x4e5b77 with the wide path; on failure the video
// handle is -1 (0x4e45e3), as for a missing file. The call goes through h68_render_file, which answers
// "file not found" for the four logo files only.
const wchar_t *const kLogos[] = {L"1_codemasters.wmv", L"2_1c.wmv", L"3_bw.wmv", L"4_nv.wmv"};
bool is_logo(const wchar_t *p) {
    if (!p) return false;
    size_t n = wcslen(p);
    for (const wchar_t *nm : kLogos) {
        std::wstring tail = std::wstring(L"video\\intro\\") + nm;
        if (n < tail.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < tail.size() && eq; i++) {
            wchar_t a = p[n - tail.size() + i], b = tail[i];
            if (a == L'/') a = L'\\';
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
            eq = a == b;
        }
        if (eq && (n == tail.size() || p[n - tail.size() - 1] == L'\\' || p[n - tail.size() - 1] == L'/'))
            return true;
    }
    return false;
}

HRESULT __stdcall h68_render_file(void *graph, const wchar_t *file, const wchar_t *playlist) {
    if (is_logo(file)) {
        ee_log("skip logos: %ls not played", file);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    typedef HRESULT(__stdcall * F)(void *, const wchar_t *, const wchar_t *);
    F f = (F)(*(void ***)graph)[13];
    return f(graph, file, playlist);
}

bool fix_logos() {
    Site s = site(0x4e5b71, {0x6a, 0x00, 0x52, 0x50, 0x8b, 0x08, 0xff, 0x51, 0x34},
                  {0x6a, 0x00, 0x52, 0x50, 0xe8, 0, 0, 0, 0});
    put32(s.put + 5, rel32(0x4e5b75, 5, (void *)h68_render_file));
    const unsigned char ctx[] = {0x89, 0x85, 0xe4, 0xfd, 0xff, 0xff, 0x85, 0xc0, 0x0f, 0x88};
    const unsigned char ret[] = {0x83, 0xc9, 0xff, 0x84, 0xc0, 0x0f, 0x44, 0xf1};   // 0x4e45de or ecx,-1; test al,al; cmove esi,ecx
    if (!ctx_ok(0x4e5b7a, ctx, sizeof ctx, "skip logos") || !ctx_ok(0x4e45de, ret, sizeof ret, "skip logos"))
        return false;
    bool r = apply_sites(&s, 1, "skip logos");
    if (r)
        ee_log("skip logos: on (the four logo videos are not played)");
    return r;
}

// ---------------------------------------------------------------------------------------------
// log-only watcher (only with logging on): values the program really uses
// ---------------------------------------------------------------------------------------------
DWORD WINAPI watch(LPVOID) {
    int vb = -12345; float vf = -1, cmin = -1, cmax = -1, vdist = -1, vmapmin = 0, vmapmax = 0;
    for (;; Sleep(500)) {
        int b = *(volatile int *)(uintptr_t)R(0x8e5964);          // sound manager 0x8e5928 + 0x3c
        float f = *(volatile float *)(uintptr_t)R(0x8c6fe0);      // frame time of the limiter (ms)
        float a = *(volatile float *)(uintptr_t)R(0x8c6560), z = *(volatile float *)(uintptr_t)R(0x8c6564);
        if (b != vb) { ee_log("watch: sound budget (manager+0x3c) = %d", b); vb = b; }
        if (f != vf) { ee_log("watch: frame limiter = %.4f ms (%.1f fps)", f, f > 0 ? 1000.0 / f : 0.0); vf = f; }
        char *cam3d = *(char *volatile *)(uintptr_t)R(0x8c0e54);                 // 3D camera (0x71c8f0 reads +0x18)
        float dist = cam3d && !IsBadReadPtr(cam3d + 0x18, 4) ? *(volatile float *)(cam3d + 0x18) : -1.f;
        if (fabsf(dist - vdist) > 1.f) { ee_log("watch: camera distance %.1f", dist); vdist = dist; }
        if (g_cam_map_min != vmapmin || g_cam_map_max != vmapmax) {
            ee_log("watch: the map/save asked for camera distance %.1f .. %.1f (kept at %.1f .. %.1f)", g_cam_map_min, g_cam_map_max, a, z);
            vmapmin = g_cam_map_min; vmapmax = g_cam_map_max;
        }
        if (a != cmin || z != cmax) { ee_log("watch: camera distance limits %.1f .. %.1f (map asked %.1f .. %.1f)", a, z, g_cam_map_min, g_cam_map_max); cmin = a; cmax = z; }
    }
    return 0;
}

}   // namespace

extern "C" void ee_small_init(void) {
    g_delta = (uint32_t)(uintptr_t)GetModuleHandleA(NULL) - 0x400000;
    if (g_delta)
        ee_log("small: program loaded at %08x (moved by %08x)", R(0x400000), g_delta);
    if (ee_ini_int("FpsList", 1)) fix_fps_list(); else ee_log("fps list: off (FpsList=0)");
    int sv = ee_ini_int("SoundVoices", 64);
    if (sv >= 21 && sv <= 256) {
        if (fix_sound(sv) && !install_newprofile_hook()) ee_log("sound: new profiles keep 20 (hook not applied)");
    } else ee_log("sound: off (SoundVoices=%d; 21..256 = on): the game's own Buffers list", sv);
    if (ee_ini_int("ScrollFix", 1)) fix_scroll(); else ee_log("scroll fix: off (ScrollFix=0)");
    if (ee_ini_int("KeysDefaults", 1)) fix_keys_defaults(); else ee_log("keys defaults: off (KeysDefaults=0)");
    if (ee_ini_int("NewProfileKeys", 1)) fix_new_profile_keys(); else ee_log("new profile keys: off (NewProfileKeys=0)");
    if (ee_ini_int("CameraZoom", 1)) {
        g_cam_min = (float)ee_ini_int("CameraMinHeight", 700);
        g_cam_max = (float)ee_ini_int("CameraMaxHeight", 3000);
        g_cam_step = (float)ee_ini_int("CameraZoomStep", 150);
        if (g_cam_min >= 1 && g_cam_max > g_cam_min && g_cam_step > 0) fix_camera();
        else ee_log("camera zoom: off (values out of range)");
    } else ee_log("camera zoom: off (CameraZoom=0)");
    if (ee_ini_int("SkipLogos", 1)) fix_logos(); else ee_log("skip logos: off (SkipLogos=0)");
    if (ee_log_on()) {
        HANDLE h = CreateThread(NULL, 0, watch, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
}
