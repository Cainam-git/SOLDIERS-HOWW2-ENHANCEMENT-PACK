// splash4k: the start-up splash of the Enhanced Edition program in 4K, with the ENHANCED EDITION line.
//
// Their program (READ, WM_PAINT handler 0x4efb75..0x4efc8e) loads resource<lang>/interface/splash.bmp
// (800x600) with LoadImageA and paints it with ONE BitBlt, unscaled and centred in the client area,
// followed by four BLACKNESS BitBlts for the borders. BitBlt is taken from the import table once:
//   0x4efbee  8b 35 4c 80 82 00     mov esi, [0x82804c]   ; BitBlt (the only user of that import)
// and all five calls go through esi. We load esi with our function instead (be imm32 90, gated by ee_patch).
// Our function: when the source bitmap is exactly their 800x600 splash (content hash) and
// uiscale_hd\splash4k_ee.bin is valid, the whole client area is painted black and the 3x picture from the
// file is painted centred (the same place the pack's GOG splash uses: 2400x1800 in the middle of 4K);
// the four border fills of that paint are then skipped (they would cut into the bigger picture).
// Anything else (other picture, other language, missing or bad file, window smaller than the picture)
// goes to the real BitBlt unchanged.
// File (made by tools/brand/make_splash_eeprog.py): "SPHD", u32 version 1,
// u32 factor 3, u32 source w 800, u32 source h 600, u32 region count n; n x {i32 x, y, w, h (3x picture px,
// relative to the 2400x1800 frame; may lie outside it), u32 channels 1 = grey or 3 = B,G,R}; then the rows.
// uiscale.ini [EE] Splash4K=1 (0 = their own small splash).
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "ee.h"

namespace {

typedef BOOL(WINAPI *BitBltFn)(HDC, int, int, int, int, HDC, int, int, DWORD);
BitBltFn g_real;
HDC g_painted;             // hdc of the paint in which the 4K picture was drawn (its border fills are skipped)
int g_file = 0;            // 0 not looked at, 1 ok, -1 unusable
std::string g_path;
const unsigned long long kSplashHash = 0x039da709f82e5c2dULL;   // FNV-1a 64 of their splash.bmp as 24-bit DIB rows

#pragma pack(push, 1)
struct Head { char magic[4]; DWORD version, factor, srcw, srch, count; };
struct Reg { int x, y, w, h; DWORD ch; };
#pragma pack(pop)
std::vector<Reg> g_regs;
std::vector<DWORD> g_offs;
DWORD g_factor;

bool read_at(HANDLE f, DWORD off, void *buf, DWORD n) {
    if (SetFilePointer(f, (LONG)off, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return false;
    DWORD got = 0;
    return ReadFile(f, buf, n, &got, NULL) && got == n;
}

bool load_file() {
    if (g_file) return g_file > 0;
    g_file = -1;
    char p[MAX_PATH];
    g_path = ee_beside_program("uiscale_hd\\splash4k_ee.bin", p);
    HANDLE f = CreateFileA(g_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { ee_log("splash4k: no %s, their own splash", g_path.c_str()); return false; }
    Head hd;
    DWORD size = GetFileSize(f, NULL);
    bool ok = read_at(f, 0, &hd, sizeof hd) && !memcmp(hd.magic, "SPHD", 4) && hd.version == 1 && hd.factor >= 2 &&
              hd.factor <= 4 && hd.srcw == 800 && hd.srch == 600 && hd.count && hd.count <= 64;
    if (ok) {
        g_regs.resize(hd.count);
        ok = read_at(f, sizeof hd, g_regs.data(), hd.count * sizeof(Reg));
        DWORD off = sizeof hd + hd.count * sizeof(Reg);
        for (DWORD i = 0; ok && i < hd.count; i++) {
            const Reg &r = g_regs[i];
            if (r.w <= 0 || r.h <= 0 || r.w > 4096 || r.h > 4096 || (r.ch != 1 && r.ch != 3)) { ok = false; break; }
            g_offs.push_back(off);
            off += (DWORD)r.w * r.h * r.ch;
        }
        ok = ok && off == size;
    }
    CloseHandle(f);
    if (!ok) { ee_log("splash4k: %s is not valid, their own splash", g_path.c_str()); return false; }
    g_factor = hd.factor;
    g_file = 1;
    ee_log("splash4k: %s ok (%lu regions)", g_path.c_str(), (unsigned long)hd.count);
    return true;
}

// is the bitmap selected into src their 800x600 splash? (checked by content, once per bitmap handle)
bool is_their_splash(HDC src) {
    static HGDIOBJ last;
    static bool last_ok;
    HGDIOBJ bmp = GetCurrentObject(src, OBJ_BITMAP);
    if (!bmp) return false;
    if (bmp == last) return last_ok;
    last = bmp;
    last_ok = false;
    BITMAP bm;
    if (!GetObjectA(bmp, sizeof bm, &bm) || bm.bmWidth != 800 || bm.bmHeight != 600) return false;
    std::vector<unsigned char> px(800 * 600 * 3);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 800;
    bi.bmiHeader.biHeight = 600;   // bottom-up, as the file
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    // GetDIBits needs the bitmap NOT selected into a DC on Windows; read through a copy of the DC's pixels instead
    HDC mem = CreateCompatibleDC(src);
    HBITMAP copy = CreateCompatibleBitmap(src, 800, 600);
    bool ok = false;
    if (mem && copy) {
        HGDIOBJ old = SelectObject(mem, copy);
        g_real(mem, 0, 0, 800, 600, src, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        ok = GetDIBits(mem, copy, 0, 600, px.data(), &bi, DIB_RGB_COLORS) == 600;
    }
    if (copy) DeleteObject(copy);
    if (mem) DeleteDC(mem);
    if (!ok) { ee_log("splash4k: could not read the source picture"); return false; }
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (unsigned char c : px) { h ^= c; h *= 0x100000001b3ULL; }
    last_ok = h == kSplashHash;
    ee_log("splash4k: source picture 800x600 hash %016llx -> %s", h, last_ok ? "their splash" : "other picture, left alone");
    return last_ok;
}

bool paint_4k(HDC hdc) {
    HWND w = WindowFromDC(hdc);
    RECT rc;
    if (!w || !GetClientRect(w, &rc)) return false;
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    int fw = 800 * (int)g_factor, fh = 600 * (int)g_factor;
    if (cw < fw || ch < fh) { ee_log("splash4k: window %dx%d smaller than %dx%d, their own splash", cw, ch, fw, fh); return false; }
    int ox = (cw - fw) / 2, oy = (ch - fh) / 2;
    HANDLE f = CreateFileA(g_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    g_real(hdc, 0, 0, cw, ch, NULL, 0, 0, BLACKNESS);
    std::vector<unsigned char> row;
    std::vector<DWORD> img;
    for (size_t i = 0; i < g_regs.size(); i++) {
        const Reg &r = g_regs[i];
        row.resize((size_t)r.w * r.ch);
        img.assign((size_t)r.w * r.h, 0);
        bool ok = SetFilePointer(f, (LONG)g_offs[i], NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
        for (int y = 0; ok && y < r.h; y++) {
            DWORD got = 0;
            ok = ReadFile(f, row.data(), (DWORD)row.size(), &got, NULL) && got == row.size();
            DWORD *d = &img[(size_t)y * r.w];
            if (r.ch == 1)
                for (int x = 0; x < r.w; x++) d[x] = row[x] * 0x010101u;
            else
                for (int x = 0; x < r.w; x++) d[x] = row[3 * x] | (row[3 * x + 1] << 8) | (row[3 * x + 2] << 16);
        }
        if (!ok) { CloseHandle(f); ee_log("splash4k: read error in region %u", (unsigned)i); return true; }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = r.w;
        bi.bmiHeader.biHeight = -r.h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(hdc, ox + r.x, oy + r.y, r.w, r.h, 0, 0, 0, r.h, img.data(), &bi, DIB_RGB_COLORS);
    }
    CloseHandle(f);
    return true;
}

// test switch (environment EE_SPLASHDUMP=1): the painted client area once -> splash4k_dump.bmp beside the program
void dump_client(HDC hdc) {
    static bool done;
    const char *e = getenv("EE_SPLASHDUMP");
    if (done || !(e && *e == '1')) return;
    done = true;
    HWND w = WindowFromDC(hdc);
    RECT rc;
    if (!w || !GetClientRect(w, &rc)) return;
    int cw = rc.right, ch = rc.bottom;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, cw, ch);
    if (!mem || !bm) return;
    HGDIOBJ old = SelectObject(mem, bm);
    g_real(mem, 0, 0, cw, ch, hdc, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD stride = ((DWORD)cw * 3 + 3) & ~3u;
    std::vector<unsigned char> px((size_t)stride * ch);
    int got = GetDIBits(mem, bm, 0, ch, px.data(), &bi, DIB_RGB_COLORS);
    DeleteObject(bm);
    DeleteDC(mem);
    char p[MAX_PATH];
    HANDLE f = CreateFileA(ee_beside_program("splash4k_dump.bmp", p), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + (DWORD)px.size();
    DWORD wr;
    WriteFile(f, &fh, sizeof fh, &wr, NULL);
    WriteFile(f, &bi.bmiHeader, sizeof bi.bmiHeader, &wr, NULL);
    WriteFile(f, px.data(), (DWORD)px.size(), &wr, NULL);
    CloseHandle(f);
    ee_log("splash4k: test dump of the client area %dx%d written (%d rows)", cw, ch, got);
}

BOOL WINAPI splash_bitblt(HDC hdc, int x, int y, int cx, int cy, HDC src, int sx, int sy, DWORD rop) {
    if (rop == SRCCOPY && src && cx == 800 && cy == 600 && sx == 0 && sy == 0) {
        g_painted = NULL;
        if (load_file() && is_their_splash(src) && paint_4k(hdc)) {
            static int n;
            if (n++ < 3) ee_log("splash4k: 4K splash painted");
            g_painted = hdc;
            dump_client(hdc);
            return TRUE;
        }
    } else if (rop == BLACKNESS && hdc && hdc == g_painted) {
        return TRUE;   // border fill of a paint that already covered the whole window
    }
    return g_real(hdc, x, y, cx, cy, src, sx, sy, rop);
}

}   // namespace

extern "C" void ee_splash4k_init(void) {
    if (!ee_ini_int("Splash4K", 1)) { ee_log("splash4k: Splash4K=0, their own splash"); return; }
    g_real = (BitBltFn)GetProcAddress(GetModuleHandleA("gdi32.dll"), "BitBlt");
    if (!g_real) { ee_log("splash4k: no gdi32 BitBlt"); return; }
    // 0x4efbec: push ecx; push esi; mov esi,[0x82804c]; sub eax,edx  ->  mov esi, splash_bitblt; nop
    static const unsigned char orig[10] = {0x51, 0x56, 0x8b, 0x35, 0x4c, 0x80, 0x82, 0x00, 0x2b, 0xc2};
    unsigned char put[10];
    memcpy(put, orig, sizeof put);
    DWORD a = (DWORD)(uintptr_t)&splash_bitblt;
    put[2] = 0xbe;
    memcpy(put + 3, &a, 4);
    put[7] = 0x90;
    ee_patch(0x4efbec, orig, put, sizeof put, "splash4k: splash painter");
}
