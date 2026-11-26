// hdtext: sharp interface text (Direct3D 9). See hdtext9.h.
// Linked into the proxy dll: compile hdtext.cpp WITHOUT -DHDTEXT_SHIM and call hdtext_install()
// once + hdtext_overlay() after every 3x copy of a texture level 0.
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include "../ee.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <string>
#include "hdtext9.h"

namespace {

// ---- game addresses (Enhanced Edition Soldiers.exe R1.28.8, sha256 5291a092...) ----------
const uintptr_t VT_MONO = 0x854f74, VT_HQ = 0x854fa8;   // eFontMono / eFontHQ vtables
const uintptr_t SLOT11_MONO = VT_MONO + 11 * 4, SLOT11_HQ = VT_HQ + 11 * 4;
const uintptr_t FN_MONO = 0x752c40, FN_HQ = 0x7527c0;   // original glyph painters
// EE: the eSurface lock/unlock are inlined in the game (guard 0x7443f0 / 0x744b50); same protocol, done here:
// lock = LockRect(whole, NOSYSLOCK) when bits (+0x40) is 0, then count (+0x48, u16) + 1;
// unlock = count - 1, at 0 UnlockRect and bits = 0.
const unsigned char BYTES_MONO[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0xf0, 0x3e, 0x81, 0x00};
const unsigned char BYTES_HQ[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0x90, 0x7d, 0x7f, 0x00};
const unsigned char BYTES_LOCK[] = {0x66, 0xff, 0x46, 0x48};   // inc word [esi+0x48] at 0x744462
const unsigned char BYTES_UNLOCK[] = {0x56, 0x8b, 0xf1, 0xb8, 0xff, 0xff, 0x00, 0x00, 0x66, 0x01, 0x46, 0x48};   // 0x744b50
const uintptr_t FN_LOCK = 0x744462, FN_UNLOCK = 0x744b50;   // byte checks only

typedef void(__fastcall *PaintFn)(void *self, void *edx, int *pos, unsigned ch, unsigned char *color,
                                  void *target, int *clip);
typedef void(__fastcall *LockFn)(void *esurf);
void es_lock(void *esv) {
    char *es = (char *)esv;
    if (!*(void **)(es + 0x40)) {
        IDirect3DSurface9 *s = *(IDirect3DSurface9 **)(es + 0x18);
        D3DLOCKED_RECT lr;
        if (s && SUCCEEDED(s->lpVtbl->LockRect(s, &lr, nullptr, D3DLOCK_NOSYSLOCK))) {
            *(int *)(es + 0x44) = lr.Pitch;
            *(void **)(es + 0x40) = lr.pBits;
        }
    }
    (*(unsigned short *)(es + 0x48))++;
}
void es_unlock(void *esv) {
    char *es = (char *)esv;
    if (--(*(unsigned short *)(es + 0x48)) == 0) {
        IDirect3DSurface9 *s = *(IDirect3DSurface9 **)(es + 0x18);
        if (s) s->lpVtbl->UnlockRect(s);
        *(void **)(es + 0x40) = nullptr;
    }
}

struct Font {
    int type, h, hqoff;
    char name[32];
    bool plain;   // listed in hdtext_plain.txt (or the built-in default): left to the plain result
    unsigned char widths[256];
    const unsigned char *glyph[256];   // (3w x 3h) alpha
};
struct Rec {
    int x0, y0, x1, y1;   // clipped 1x rectangle
    int cx, cy;           // cell origin (1x)
    int font, ch;
    uint32_t color;       // ARGB
    unsigned ascale = 256; // glyph alpha x ascale/256 (layer copies blend weaker than plain "over")
    float err = 0;         // self-check: HD glyph boxed down to 1x vs what the game painted (0 = same)
    bool hd = true;        // false: this glyph is left to the plain result (and so is all text touching it)
    std::vector<uint32_t> before, after;
};

std::vector<Font> g_fonts;
std::vector<unsigned char> g_pack;
std::unordered_map<void *, std::vector<Rec>> g_recs;     // key = game texture (wrapper)
const GUID kIID_Tex8 = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};   // IID_IDirect3DTexture9
int g_installed = 0;
int g_builtin_plain = 1;
int g_close_copies = 0;    // ListText: a glyph repeated 1 px away (shadow/emboss pass) is drawn 1 real px away   // built-in plain list in use (the proxy's ListText=1 turns it off)
bool g_busy = false;
FILE *g_log = nullptr;
long g_nglyph = 0, g_nrec = 0, g_nover = 0, g_nskipfont = 0;

void logf(const char *fmt, ...) {
    if (!g_log) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fflush(g_log);
}

int font_index(void *self) {
    const unsigned char *f = (const unsigned char *)self;
    uintptr_t vt = *(const uintptr_t *)f;
    int type = vt == VT_HQ ? 1 : 0;
    int h = f[0x125];
    for (size_t i = 0; i < g_fonts.size(); i++) {
        const Font &F = g_fonts[i];
        if (F.plain || F.type != type || F.h != h) continue;
        // the game changes a few widths after loading (e.g. char 128); allow up to 4 differences,
        // glyphs whose width differs are then left to the original painter only
        int nd = 0;
        for (int k = 0; k < 256 && nd <= 4; k++) nd += F.widths[k] != f[0x25 + k];
        if (nd > 4) continue;
        return (int)i;
    }
    return -1;
}

// no cache: the game re-creates textures at the same addresses
void *texture_of(IDirect3DSurface9 *s) {
    void *t = nullptr;
    if (FAILED(s->lpVtbl->GetContainer(s, kIID_Tex8, &t)) || !t) return nullptr;
    ((IUnknown *)t)->lpVtbl->Release((IUnknown *)t);   // key only; the game keeps it alive
    return t;
}

// one lock for all record maps (textures are touched from more than one thread)
CRITICAL_SECTION g_cs;
struct Guard {
    Guard() { EnterCriticalSection(&g_cs); }
    ~Guard() { LeaveCriticalSection(&g_cs); }
};

#ifdef HDTEXT_SHIM
void shim_seen(IDirect3DSurface9 *s, void *key);
#endif

bool same_place(const Rec &o, const Rec &r) {
    return o.cx == r.cx && o.cy == r.cy && o.font == r.font && o.ch == r.ch && o.color == r.color &&
           o.x0 == r.x0 && o.y0 == r.y0 && o.x1 == r.x1 && o.y1 == r.y1;
}

void over(uint32_t &dst, uint32_t col, unsigned a);

// Self-check of one record (never worse than off): draw the HD glyph over the record's own 1x
// background at 3x, box it back down to 1x and compare with what the game's painter produced.
// Result = sum |hd - game| / (sum |game - bg| + sum |hd - bg|), premultiplied ARGB.
// 0 = identical, ~0.5 = same ink in a different shape, >= 1 = HD ink where the game has none.
float g_maxerr = 0.75f;
long g_nfail = 0, g_nfinalfail = 0;
float hd_error(const Rec &r) {
    const Font &F = g_fonts[r.font];
    const unsigned char *g = F.glyph[r.ch];
    int gw = 3 * F.widths[r.ch], rw = r.x1 - r.x0;
    unsigned ca = r.color >> 24;
    uint32_t col = r.color & 0xffffff;
    double diff = 0, ink = 0;
    auto pm = [](uint32_t p, int sh) { return (double)((p >> sh) & 255) * (sh == 24 ? 255 : (p >> 24)) / 255.0; };
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            int k = (y - r.y0) * rw + x - r.x0;
            double acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    uint32_t px = r.before[k];
                    unsigned a = g[(3 * (y - r.cy) + sy) * gw + 3 * (x - r.cx) + sx];
                    if (a) {
                        unsigned aa = (a * ca + 127) / 255 * r.ascale / 256;
                        over(px, col, aa > 255 ? 255 : aa);
                    }
                    for (int c = 0; c < 4; c++) acc[c] += pm(px, 8 * c);
                }
            for (int c = 0; c < 4; c++) {
                double h = acc[c] / 9, a = pm(r.after[k], 8 * c), b = pm(r.before[k], 8 * c);
                diff += fabs(h - a);
                ink += fabs(a - b) + fabs(h - b);
            }
        }
    return ink > 0 ? (float)(diff / ink) : 0.f;
}

// live records that may be drawn in HD: drop every record whose self-check failed, and every live
// record whose rectangle touches a dropped one (transitively), so that region stays plain
bool is_fx(const Rec &r);
void restrict_to_hd(const std::vector<Rec> &v, std::vector<char> &ok) {
    std::vector<char> bad(v.size(), 0);
    bool changed = true;
    // a failed glow/shadow-mask glyph (e.g. the space between two hovered words: the game's glow
    // spills into it, the HD space is empty) only drops itself: its 1x pixels stay in the smooth
    // copy; it must not turn the whole word plain (seen on hovered SINGLE PLAYER / MULTI PLAYER)
    for (size_t i = 0; i < v.size(); i++)
        if (ok[i] && !v[i].hd && is_fx(v[i])) ok[i] = 0;
    for (size_t i = 0; i < v.size(); i++) bad[i] = ok[i] && !v[i].hd;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < v.size(); i++) {
            if (!ok[i] || bad[i]) continue;
            for (size_t j = 0; j < v.size(); j++)
                if (bad[j] && v[i].x0 < v[j].x1 && v[j].x0 < v[i].x1 && v[i].y0 < v[j].y1 && v[j].y0 < v[i].y1) {
                    bad[i] = 1;
                    changed = true;
                    break;
                }
        }
    }
    for (size_t i = 0; i < v.size(); i++)
        if (bad[i]) ok[i] = 0;
}

// store a record (before/after already filled); key = game texture
void add_record(void *key, Rec &r, IDirect3DSurface9 *s) {
    Guard g_;
    if (r.before == r.after) {
        // nothing changed: blank glyph, or a repaint of a glyph that is already there; in the
        // second case keep its record (refresh the after-image) so it stays live
        auto it = g_recs.find(key);
        if (it != g_recs.end())
            for (Rec &o : it->second)
                if (same_place(o, r)) {
                    o.after.swap(r.after);
                    break;
                }
        return;
    }
    std::vector<Rec> &v = g_recs[key];
    // the same glyph repainted at the same place (some lists repaint every frame WITHOUT clearing)
    // replaces its old record but keeps the old background, else the 1x glyph would show through
    for (size_t i = 0; i < v.size(); i++)
        if (same_place(v[i], r)) {
            r.before.swap(v[i].before);
            v.erase(v.begin() + i);
            break;
        }
    if (v.size() >= 16384) v.erase(v.begin(), v.begin() + 4096);
    r.err = hd_error(r);
    r.hd = r.err <= g_maxerr;
    if (!r.hd) {
        g_nfail++;
        static int nlog = 0;
        if (nlog < 200) {
            nlog++;
            logf("self-check FAIL key %p font %d ch %d cell %d,%d col %08x ascale %u err %.2f\n", key, r.font, r.ch, r.cx,
                 r.cy, r.color, r.ascale, r.err);
        }
    }
    {
        static int nlog = 0;
        if (nlog < 400) {
            nlog++;
            logf("rec key %p font %d ch %c cell %d,%d rect %d,%d-%d,%d col %08x ascale %u err %.2f\n", key, r.font, r.ch, r.cx,
                 r.cy, r.x0, r.y0, r.x1, r.y1, r.color, r.ascale, r.err);
        }
    }
    v.push_back(std::move(r));
    g_nrec++;
#ifdef HDTEXT_SHIM
    shim_seen(s, key);
#else
    (void)s;
#endif
}

// which records are still live: newest first. A record is live if every pixel it changed (its ink:
// after != before) that is not covered by the ink of a newer live record still holds its
// after-image, and at least one such pixel exists (fully covered = invisible = dead). Other pixels
// of its rectangle must still hold the after-image too unless a newer live record claimed them.
// Effect layers: the blurred shadow/glow mask font (bookman_hq_shadow_12b) is composited
// UNDER the letters in the normal state but OVER them in the hover state (glow pass after the
// letters, with a blend that leaves the white letter cores white). Its records must therefore
// never hide the letter records underneath (else the letters died and only the orange glow was
// drawn in HD: "orange letters, one white letter"), and they are always painted below letters.
bool is_fx(const Rec &r) { return g_fonts[r.font].name[0] && strstr(g_fonts[r.font].name, "shadow"); }

std::vector<char> live(const std::vector<Rec> &v, const uint32_t *src1x, int pitch1x, int w, int h) {
    // mark: 1 = claimed by the ink of a newer live letter record, 2 = only by a newer effect record
    std::vector<unsigned char> mark(w * h, 0);
    std::vector<char> ok(v.size(), 0);
    for (size_t i = v.size(); i-- > 0;) {
        const Rec &r = v[i];
        if (r.x1 > w || r.y1 > h) continue;
        bool good = true;
        int own = 0;
        int rw = r.x1 - r.x0;
        for (int y = r.y0; y < r.y1 && good; y++) {
            const uint32_t *row = (const uint32_t *)((const char *)src1x + y * pitch1x);
            for (int x = r.x0; x < r.x1; x++) {
                unsigned char m = mark[y * w + x];
                int k = (y - r.y0) * rw + x - r.x0;
                if (m == 1) continue;
                if (m == 2) {   // changed by a newer glow pass: cannot be compared, still this glyph's ink
                    own += r.after[k] != r.before[k];
                    continue;
                }
                if (row[x] != r.after[k]) { good = false; break; }
                own += r.after[k] != r.before[k];
            }
        }
        if (!good || !own) continue;
        ok[i] = 1;
        unsigned char tag = is_fx(r) ? 2 : 1;
        for (int y = r.y0; y < r.y1; y++)
            for (int x = r.x0; x < r.x1; x++) {
                int k = (y - r.y0) * rw + x - r.x0;
                if (r.after[k] != r.before[k] && mark[y * w + x] != 1) mark[y * w + x] = tag;
            }
    }
    return ok;
}

void __fastcall paint_hook(void *self, void *edx, int *pos, unsigned ch, unsigned char *color,
                           void *target, int *clip) {
    uintptr_t vt = *(uintptr_t *)self;
    PaintFn orig = (PaintFn)(vt == VT_HQ ? FN_HQ : FN_MONO);
    g_nglyph++;
    char *es = target ? *(char **)((char *)target + 0x64) : nullptr;
    int fi = (es && !g_busy) ? font_index(self) : -1;
    if (fi < 0) {
        if (es && !g_busy) {
            g_nskipfont++;
            static void *seen[16];
            static int nseen = 0;
            bool known = false;
            for (int i = 0; i < nseen; i++) known |= seen[i] == self;
            if (!known && nseen < 16) {
                seen[nseen++] = self;
                const unsigned char *f = (const unsigned char *)self;
                logf("unknown font %p vt %08x h %d ch %u widths[A..H]:", self, *(unsigned *)f, f[0x125], ch & 0xff);
                for (int k = 0; k < 8; k++) logf(" %d", f[0x25 + 65 + k]);
                logf(" | +0x524 %d\n", f[0x52c]);
                for (size_t i = 0; i < g_fonts.size(); i++) {
                    if (g_fonts[i].h != f[0x125]) continue;
                    int nd = 0, first = -1;
                    for (int k = 0; k < 256; k++)
                        if (g_fonts[i].widths[k] != f[0x25 + k]) { nd++; if (first < 0) first = k; }
                    logf("   vs pack font %d: %d widths differ, first at %d (pack %d game %d)\n", (int)i, nd, first,
                         first >= 0 ? g_fonts[i].widths[first] : 0, first >= 0 ? f[0x25 + first] : 0);
                }
            }
        }
        orig(self, edx, pos, ch, color, target, clip);
        return;
    }
    IDirect3DSurface9 *s = *(IDirect3DSurface9 **)(es + 0x18);
    D3DSURFACE_DESC d;
    void *key = nullptr;
    if (s && SUCCEEDED(s->lpVtbl->GetDesc(s, &d)) &&
        (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8))
        key = texture_of(s);
    if (!key) {
        orig(self, edx, pos, ch, color, target, clip);
        return;
    }
    const Font &F = g_fonts[fi];
    int c = ch & 0xff;
    if (F.widths[c] != ((unsigned char *)self)[0x25 + c]) {
        orig(self, edx, pos, ch, color, target, clip);
        return;
    }
    int off = F.type == 1 ? ((unsigned char *)self)[0x52c] : 0;
    Rec r;
    r.cx = pos[0] + clip[0] - off;
    r.cy = pos[1] + clip[1] - off;
    r.x0 = r.cx > clip[0] ? r.cx : clip[0];
    r.y0 = r.cy > clip[1] ? r.cy : clip[1];
    r.x1 = r.cx + F.widths[c] < clip[2] ? r.cx + F.widths[c] : clip[2];
    r.y1 = r.cy + F.h < clip[3] ? r.cy + F.h : clip[3];
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > (int)d.Width) r.x1 = d.Width;
    if (r.y1 > (int)d.Height) r.y1 = d.Height;
    if (r.x0 >= r.x1 || r.y0 >= r.y1) {
        orig(self, edx, pos, ch, color, target, clip);
        return;
    }
    es_lock(es);
    const unsigned char *bits = *(unsigned char **)(es + 0x40);
    int pitch = *(int *)(es + 0x44);
    if (!bits) {
        es_unlock(es);
        orig(self, edx, pos, ch, color, target, clip);
        return;
    }
    int n = (r.x1 - r.x0) * (r.y1 - r.y0);
    r.before.resize(n);
    for (int y = r.y0, k = 0; y < r.y1; y++, k += r.x1 - r.x0)
        memcpy(&r.before[k], bits + y * pitch + r.x0 * 4, (r.x1 - r.x0) * 4);
    orig(self, edx, pos, ch, color, target, clip);
    r.after.resize(n);
    for (int y = r.y0, k = 0; y < r.y1; y++, k += r.x1 - r.x0)
        memcpy(&r.after[k], bits + y * pitch + r.x0 * 4, (r.x1 - r.x0) * 4);
    r.font = fi;
    r.ch = c;
    memcpy(&r.color, color, 4);   // bytes B,G,R,A = D3DCOLOR
    add_record(key, r, s);
    es_unlock(es);   // the real UnlockRect happens here (lock count back to 0)
}


// ---- layer composite (text styles 3/4) ---------------------------------------------------------
// 0x49b650 redirects such text into two 1024x64 layer surfaces; at the string end 0x49b5b0 copies
// each layer pixel by pixel into the real target through 0x4b1b10 (call site 0x49b640). The hook
// carries the live glyph records of the source layer over to the target (translated, with the
// target's own before/after pixels), so the target gets HD glyphs too.
const uintptr_t FN_BLIT = 0x422290;
const uintptr_t CALL_BLIT_EE[] = {0x74fa30, 0x74fb44, 0x74fbcc};
const unsigned char BYTES_BLIT[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0x55, 0xdb, 0x7d, 0x00};
typedef void(__cdecl *BlitFn)(void *src, void *dst, int *srcRect, int *dstPos, void *op);
long g_nprop = 0;

bool surf_info(char *es, IDirect3DSurface9 **ps, void **pkey, D3DSURFACE_DESC *d) {
    if (!es) return false;
    IDirect3DSurface9 *s = *(IDirect3DSurface9 **)(es + 0x18);
    if (!s || FAILED(s->lpVtbl->GetDesc(s, d))) return false;
    if (d->Format != D3DFMT_A8R8G8B8 && d->Format != D3DFMT_X8R8G8B8) return false;
    *ps = s;
    *pkey = texture_of(s);
    return *pkey != nullptr;
}

void over(uint32_t &dst, uint32_t col, unsigned a);

void __cdecl blit_hook(void *src, void *dst, int *srcRect, int *dstPos, void *op) {
    BlitFn orig = (BlitFn)FN_BLIT;
    char *esS = src ? *(char **)((char *)src + 0x64) : nullptr;
    char *esD = dst ? *(char **)((char *)dst + 0x64) : nullptr;
    IDirect3DSurface9 *sS, *sD;
    void *kS, *kD;
    D3DSURFACE_DESC dS, dD;
    if (g_busy || !surf_info(esS, &sS, &kS, &dS) || !surf_info(esD, &sD, &kD, &dD) || !hdtext_has_text(kS)) {
        orig(src, dst, srcRect, dstPos, op);
        return;
    }
    es_lock(esS);
    es_lock(esD);
    const uint32_t *bS = *(const uint32_t **)(esS + 0x40);
    int pS = *(int *)(esS + 0x44);
    uint32_t *bD = *(uint32_t **)(esD + 0x40);
    int pD = *(int *)(esD + 0x44);
    if (!bS || !bD) {
        es_unlock(esD);
        es_unlock(esS);
        orig(src, dst, srcRect, dstPos, op);
        return;
    }
    // copy the live source records (the record maps are never held across a call into the game
    // or the proxy: no lock-order problems with the proxy's own locks)
    std::vector<Rec> vs;
    {
        Guard g_;
        std::vector<Rec> &all = g_recs[kS];
        std::vector<char> ok = live(all, bS, pS, dS.Width, dS.Height);
        for (size_t i = 0; i < all.size(); i++)
            if (ok[i]) vs.push_back(all[i]);
    }
    int dx = dstPos[0] - srcRect[0], dy = dstPos[1] - srcRect[1];
    // destination rectangle touched by the copy
    int X0 = dstPos[0], Y0 = dstPos[1], X1 = X0 + srcRect[2] - srcRect[0], Y1 = Y0 + srcRect[3] - srcRect[1];
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > (int)dD.Width) X1 = dD.Width;
    if (Y1 > (int)dD.Height) Y1 = dD.Height;
    std::vector<uint32_t> before;
    int RW = X1 - X0;
    if (RW > 0 && Y1 > Y0) {
        before.resize(RW * (Y1 - Y0));
        for (int y = Y0; y < Y1; y++)
            memcpy(&before[(y - Y0) * RW], (char *)bD + y * pD + X0 * 4, RW * 4);
    }
    orig(src, dst, srcRect, dstPos, op);
    if (!before.empty()) {
        for (size_t i = 0; i < vs.size(); i++) {
            const Rec &o = vs[i];
            Rec r;
            r.cx = o.cx + dx;
            r.cy = o.cy + dy;
            r.x0 = std::max(std::max(o.x0, srcRect[0]) + dx, X0);
            r.y0 = std::max(std::max(o.y0, srcRect[1]) + dy, Y0);
            r.x1 = std::min(std::min(o.x1, srcRect[2]) + dx, X1);
            r.y1 = std::min(std::min(o.y1, srcRect[3]) + dy, Y1);
            if (r.x0 >= r.x1 || r.y0 >= r.y1) continue;
            r.font = o.font;
            r.ch = o.ch;
            r.color = o.color;
            int w = r.x1 - r.x0;
            r.before.resize(w * (r.y1 - r.y0));
            r.after.resize(w * (r.y1 - r.y0));
            for (int y = r.y0; y < r.y1; y++) {
                memcpy(&r.before[(y - r.y0) * w], &before[(y - Y0) * RW + r.x0 - X0], w * 4);
                memcpy(&r.after[(y - r.y0) * w], (char *)bD + y * pD + r.x0 * 4, w * 4);
            }
            // how strongly did the game's blend apply this layer? compare the observed change with
            // what plain "over" of the layer pixel would give, and scale the HD glyph alpha by it
            {
                double obs = 0, exp = 0;
                for (int y = r.y0; y < r.y1; y++)
                    for (int x = r.x0; x < r.x1; x++) {
                        int k = (y - r.y0) * w + x - r.x0;
                        uint32_t sp = *(const uint32_t *)((const char *)bS + (y - dy) * pS + (x - dx) * 4);
                        uint32_t e = r.before[k];
                        over(e, r.color & 0xffffff, sp >> 24);
                        for (int sh = 0; sh < 32; sh += 8) {
                            int b0 = (r.before[k] >> sh) & 255;
                            obs += abs((int)((r.after[k] >> sh) & 255) - b0);
                            exp += abs((int)((e >> sh) & 255) - b0);
                        }
                    }
                double q = exp > 0 ? obs / exp : 1.0;
                if (q > 1.5) q = 1.5;
                r.ascale = (unsigned)(q * 256 + 0.5);
            }
            add_record(kD, r, sD);
            g_nprop++;
        }
    }
    es_unlock(esD);
    es_unlock(esS);
}

// ---- Device::CopyRects carry-over (list font) -----------------------------------------------------
// The login/load lists paint each row into its own small text texture (records made there) and then
// Device::CopyRects the row into one list canvas texture (256x256), which is drawn at the same place.
// Without records the canvas shows the plain 1x letters under the HD ones (the "doubled" list text).
// The proxy calls this around its CopyRects with the source level as it is, and the destination
// region before/after the copy; live source records are carried over translated (plain copy: full
// strength). Returns the number of records carried.
long g_ncopy = 0, g_ncopydiff = 0;
int g_carry_old = -1;
int hdtext_carry_keys(void *kS, const uint32_t *src1x, int pitchS, int wS, int hS, void *kD,
                      const uint32_t *before, const uint32_t *after, int X0, int Y0, int RW, int RH, int sx0, int sy0,
                      int sx1, int sy1, int dx, int dy, IDirect3DSurface9 *dst_surface);
extern "C" int hdtext_carry_copy(void *src_surface, const uint32_t *src1x, int pitchS, int wS, int hS,
                                 void *dst_surface, const uint32_t *before, const uint32_t *after, int X0, int Y0,
                                 int RW, int RH, int sx0, int sy0, int sx1, int sy1, int dx, int dy) {
    if (!g_installed || g_installed < 0 || !src_surface || !dst_surface) return 0;
    void *kS = texture_of((IDirect3DSurface9 *)src_surface), *kD = texture_of((IDirect3DSurface9 *)dst_surface);
    if (!kS || !kD || kS == kD || !hdtext_has_text(kS)) return 0;
    int n = hdtext_carry_keys(kS, src1x, pitchS, wS, hS, kD, before, after, X0, Y0, RW, RH, sx0, sy0, sx1, sy1, dx, dy,
                              (IDirect3DSurface9 *)dst_surface);   // core shared with scopy_hook
    g_ncopy += n;
    return n;
}

// core of the carry-over (keys = game textures)
int hdtext_carry_keys(void *kS, const uint32_t *src1x, int pitchS, int wS, int hS, void *kD,
                      const uint32_t *before, const uint32_t *after, int X0, int Y0, int RW, int RH, int sx0, int sy0,
                      int sx1, int sy1, int dx, int dy, IDirect3DSurface9 *dst_surface) {
    if (g_carry_old < 0) { const char *e = getenv("HDTEXT_CARRYOLD"); g_carry_old = e && *e == '1'; }   // moved here (both callers)
    std::vector<Rec> vs;
    {
        Guard g_;
        std::vector<Rec> &all = g_recs[kS];
        std::vector<char> ok = live(all, src1x, pitchS, wS, hS);
        for (size_t i = 0; i < all.size(); i++)
            if (ok[i]) vs.push_back(all[i]);
    }
    int n = 0;
    for (const Rec &o : vs) {
        Rec r;
        r.cx = o.cx + dx;
        r.cy = o.cy + dy;
        r.x0 = std::max(std::max(o.x0, sx0) + dx, X0);
        r.y0 = std::max(std::max(o.y0, sy0) + dy, Y0);
        r.x1 = std::min(std::min(o.x1, sx1) + dx, X0 + RW);
        r.y1 = std::min(std::min(o.y1, sy1) + dy, Y0 + RH);
        if (r.x0 >= r.x1 || r.y0 >= r.y1) continue;
        r.font = o.font;
        r.ch = o.ch;
        r.color = o.color;
        r.ascale = o.ascale;
        int w = r.x1 - r.x0;
        r.before.resize(w * (r.y1 - r.y0));
        r.after.resize(w * (r.y1 - r.y0));
        // CopyRects REPLACES the destination pixels: the glyph-free background at the new place is
        // the source record's own background, not what the canvas held before the copy
        // (that would put old canvas content back under the glyph when the place was not cleared).
        // HDTEXT_CARRYOLD=1 (test only): old behaviour, background from the canvas.
        int ow = o.x1 - o.x0;
        bool differs = false;
        for (int y = r.y0; y < r.y1; y++) {
            const uint32_t *ob = &o.before[(size_t)(y - dy - o.y0) * ow + (r.x0 - dx - o.x0)];
            const uint32_t *cb = &before[(y - Y0) * RW + r.x0 - X0];
            differs |= memcmp(ob, cb, w * 4) != 0;
            memcpy(&r.before[(y - r.y0) * w], g_carry_old ? cb : ob, w * 4);
            memcpy(&r.after[(y - r.y0) * w], &after[(y - Y0) * RW + r.x0 - X0], w * 4);
        }
        if (differs) {
            g_ncopydiff++;
            static int nlog = 0;
            if (nlog < 40) {
                nlog++;
                logf("carry: key %p -> %p ch %d cell %d,%d: canvas background before the copy differed from the source background (%s used)\n",
                     kS, kD, r.ch, r.cx, r.cy, g_carry_old ? "canvas, HDTEXT_CARRYOLD" : "source");
            }
        }
        add_record(kD, r, dst_surface);
        n++;
    }
    return n;   // the caller counts
}

// ---- surface fill (hover colour defect) ----------------------------------------------------------
// 0x4e8460 = thiscall eSurface::Fill(int rect[4] l,t,r,b, color), ret 8; the text controls clear
// their texture with it before every repaint (0x49fe22). Records of glyphs inside a filled area
// can never be visible again; without this they could be judged live by pixel comparison alone
// (seen: hovered menu items showing some letters in the old white HD glyph over the new orange
// text). Every record touching the filled rectangle is dropped.
const uintptr_t FN_FILL = 0x7445b0;
const uintptr_t CALL_FILL[] = {0x4e8884, 0x5b17e3, 0x7352ba, 0x7356b6, 0x754a12};
typedef void(__fastcall *FillFn)(void *es, void *edx, int *rect, unsigned color);
long g_nfill = 0, g_nfilldrop = 0;
void __fastcall fill_hook(void *es, void *edx, int *rect, unsigned color) {
    ((FillFn)FN_FILL)(es, edx, rect, color);
    IDirect3DSurface9 *s;
    void *key;
    D3DSURFACE_DESC d;
    if (!rect || !surf_info((char *)es, &s, &key, &d)) return;
    Guard g_;
    auto it = g_recs.find(key);
    g_nfill++;
    if (it == g_recs.end()) return;
    std::vector<Rec> &v = it->second;
    size_t n0 = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Rec &r) {
                return r.x0 < rect[2] && rect[0] < r.x1 && r.y0 < rect[3] && rect[1] < r.y1;
            }), v.end());
    g_nfilldrop += (long)(n0 - v.size());
}

// ---- canvas copy carry-over (double-text fix) -----------------------------------
// Their program has no Device::CopyRects. A window with its own canvas (flag 0x40 in +0x24, e.g. every
// eCombobox: 0x4c2280) is painted into that canvas, and the canvas is then (a) copied into the parent
// window's canvas on the CPU and (b) drawn on screen by itself in the direct pass (0x4f4290 paints the
// children twice: cached pass into the parent canvas, then direct pass). The CPU copy is
// eSurface::CopyRect 0x7447f0 = thiscall(ecx = source eSurface, dest eSurface, int srcRect[4] l,t,r,b,
// int dstPos[2]), ret 0xc, plain replace (*dst = convert(*src) | (*dst & ~masks)), called from
// 0x7352f0 (canvas -> canvas, via 0x5b19f0 / 0x735850) and 0x745330 (surface format copy).
// Without carrying the glyph records, the parent canvas shows the game's own 1x letters (plain
// enlargement) under the sharp ones of the child canvas, up to 1 game px apart = the doubled letters
// in the Options drop-down values.
const uintptr_t FN_SCOPY = 0x7447f0;
const uintptr_t CALL_SCOPY[] = {0x735453, 0x7453e5};
const unsigned char BYTES_SCOPY[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0xed, 0x31, 0x81, 0x00};
typedef void(__fastcall *SCopyFn)(void *srcES, void *edx, void *dstES, int *rect, int *pos);
long g_nscopy = 0, g_nscopyrec = 0;
int g_scopy_on = 1;   // the proxy's ListText (0 = no carry-over, the old look)
int hdtext_carry_keys(void *kS, const uint32_t *src1x, int pitchS, int wS, int hS, void *kD,
                      const uint32_t *before, const uint32_t *after, int X0, int Y0, int RW, int RH, int sx0, int sy0,
                      int sx1, int sy1, int dx, int dy, IDirect3DSurface9 *sD);
void __fastcall scopy_hook(void *srcES, void *edx, void *dstES, int *rect, int *pos) {
    SCopyFn orig = (SCopyFn)FN_SCOPY;
    IDirect3DSurface9 *sS, *sD;
    void *kS, *kD;
    D3DSURFACE_DESC dS, dD;
    if (!g_scopy_on || g_busy || !rect || !pos || !surf_info((char *)srcES, &sS, &kS, &dS) ||
        !surf_info((char *)dstES, &sD, &kD, &dD) || kS == kD || dS.Format != D3DFMT_A8R8G8B8 || dD.Format != D3DFMT_A8R8G8B8 || !hdtext_has_text(kS)) {
        orig(srcES, edx, dstES, rect, pos);
        return;
    }
    // the rectangle the original copies (it does no clipping of its own: the caller clips)
    int sx0 = rect[0], sy0 = rect[1], sx1 = rect[2], sy1 = rect[3];
    int dx = pos[0] - sx0, dy = pos[1] - sy0;
    int X0 = std::max(pos[0], 0), Y0 = std::max(pos[1], 0);
    int X1 = std::min(pos[0] + (sx1 - sx0), (int)dD.Width), Y1 = std::min(pos[1] + (sy1 - sy0), (int)dD.Height);
    if (sx0 < 0 || sy0 < 0 || sx1 > (int)dS.Width || sy1 > (int)dS.Height || X1 <= X0 || Y1 <= Y0) {
        orig(srcES, edx, dstES, rect, pos);
        return;
    }
    es_lock((char *)srcES);
    es_lock((char *)dstES);
    const uint32_t *bS = *(const uint32_t **)((char *)srcES + 0x40);
    int pS = *(int *)((char *)srcES + 0x44);
    uint32_t *bD = *(uint32_t **)((char *)dstES + 0x40);
    int pD = *(int *)((char *)dstES + 0x44);
    if (!bS || !bD) {
        es_unlock((char *)dstES);
        es_unlock((char *)srcES);
        orig(srcES, edx, dstES, rect, pos);
        return;
    }
    int RW = X1 - X0, RH = Y1 - Y0;
    std::vector<uint32_t> before((size_t)RW * RH), after((size_t)RW * RH);
    for (int y = 0; y < RH; y++) memcpy(&before[(size_t)y * RW], (char *)bD + (size_t)(Y0 + y) * pD + X0 * 4, RW * 4);
    orig(srcES, edx, dstES, rect, pos);
    for (int y = 0; y < RH; y++) memcpy(&after[(size_t)y * RW], (char *)bD + (size_t)(Y0 + y) * pD + X0 * 4, RW * 4);
    // the source is read in place (locked; the copy never writes it)
    int n = hdtext_carry_keys(kS, bS, pS, (int)dS.Width, (int)dS.Height, kD, before.data(),
                              after.data(), X0, Y0, RW, RH, sx0, sy0, sx1, sy1, dx, dy, sD);
    g_nscopy++;
    g_nscopyrec += n;
    {
        static int nlog = 0;
        if (n > 0 && nlog < 12) {
            nlog++;
            ee_log("hdtext: canvas copy %ux%u -> %ux%u at %d,%d: %d glyph records carried", dS.Width, dS.Height,
                   dD.Width, dD.Height, pos[0], pos[1], n);
            logf("canvas copy key %p -> %p rect %d,%d-%d,%d to %d,%d: %d records carried\n", kS, kD, sx0, sy0, sx1, sy1,
                 pos[0], pos[1], n);
        }
    }
    es_unlock((char *)dstES);   // the real UnlockRect happens here (lock count back to 0)
    es_unlock((char *)srcES);
}

bool patch_call(uintptr_t at, const unsigned char *expect, void *target) {
    DWORD old;
    if (memcmp((void *)at, expect, 5)) return false;
    if (!VirtualProtect((void *)at, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t *)(at + 1) = (int32_t)((uintptr_t)target - (at + 5));
    VirtualProtect((void *)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, 5);
    return true;
}

void over(uint32_t &dst, uint32_t col, unsigned a) {
    // non-premultiplied "source over"; a = source alpha 0..255
    if (!a) return;
    if (a >= 255) { dst = col | 0xff000000u; return; }
    unsigned da = dst >> 24;
    unsigned oa = a * 255 + da * (255 - a);   // x255
    if (!oa) { dst = 0; return; }
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        unsigned sc = (col >> sh) & 255, dc = (dst >> sh) & 255;
        unsigned v = (sc * a * 255 + dc * da * (255 - a) + oa / 2) / oa;
        out |= (v > 255 ? 255 : v) << sh;
    }
    unsigned A = (oa + 127) / 255;
    dst = out | ((A > 255 ? 255 : A) << 24);
}

bool patch_ptr(uintptr_t at, uintptr_t expect, uintptr_t val) {
    DWORD old;
    if (*(uintptr_t *)at != expect) return false;
    if (!VirtualProtect((void *)at, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(uintptr_t *)at = val;
    VirtualProtect((void *)at, 4, old, &old);
    return true;
}

// ---------------------------------------------------------------------------------------------
#ifdef HDTEXT_SHIM
// Test shim for a dll without hdtext support: in-place slot patches on the proxy's wrapper
// vtables. Surface::UnlockRect marks a text texture dirty; Device::SetTexture binds a 3x
// companion texture (nearest 3x copy + hdtext_overlay) instead of the game texture.
struct Comp {
    IDirect3DTexture9 *comp = nullptr;
    IDirect3DSurface9 *surf = nullptr;
    UINT w = 0, h = 0;
    bool dirty = true;
    int glyphs = 0;
};
std::unordered_map<void *, Comp> g_comp;
std::unordered_map<void *, void *> g_surfkey;
typedef HRESULT(STDMETHODCALLTYPE *UnlockFn)(IDirect3DSurface9 *);
typedef HRESULT(STDMETHODCALLTYPE *SetTexFn)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
UnlockFn g_origUnlock = nullptr;
SetTexFn g_origSetTex = nullptr;
void **g_vtSurf = nullptr, **g_vtDev = nullptr;
long g_nbind = 0, g_nrebuild = 0;

HRESULT STDMETHODCALLTYPE unlock_hook(IDirect3DSurface9 *This) {
    HRESULT r = g_origUnlock(This);
    Guard g_;
    if (!g_busy) {
        auto it = g_surfkey.find(This);
        if (it != g_surfkey.end()) g_comp[it->second].dirty = true;
    }
    return r;
}

void rebuild(IDirect3DDevice9 *dev, void *key, Comp &c) {
    D3DSURFACE_DESC d;
    if (FAILED(c.surf->lpVtbl->GetDesc(c.surf, &d))) return;
    if (c.comp && (c.w != d.Width || c.h != d.Height)) {
        c.comp->lpVtbl->Release(c.comp);
        c.comp = nullptr;
    }
    if (!c.comp) {
        if (FAILED(dev->lpVtbl->CreateTexture(dev, d.Width * 3, d.Height * 3, 1, 0, D3DFMT_A8R8G8B8,
                                              D3DPOOL_MANAGED, &c.comp)) || !c.comp) {
            c.comp = nullptr;
            return;
        }
        c.w = d.Width;
        c.h = d.Height;
    }
    g_busy = true;
    D3DLOCKED_RECT a, b;
    if (SUCCEEDED(c.surf->lpVtbl->LockRect(c.surf, &a, nullptr, D3DLOCK_READONLY))) {
        if (SUCCEEDED(c.comp->lpVtbl->LockRect(c.comp, 0, &b, nullptr, 0))) {
            for (UINT y = 0; y < c.h * 3; y++) {
                const uint32_t *s = (const uint32_t *)((const char *)a.pBits + (y / 3) * a.Pitch);
                uint32_t *o = (uint32_t *)((char *)b.pBits + y * b.Pitch);
                for (UINT x = 0; x < c.w; x++) o[3 * x] = o[3 * x + 1] = o[3 * x + 2] = s[x];
            }
            c.glyphs = hdtext_overlay(key, (const uint32_t *)a.pBits, a.Pitch, (uint32_t *)b.pBits,
                                      b.Pitch, c.w, c.h);
            c.comp->lpVtbl->UnlockRect(c.comp, 0);
        }
        c.surf->lpVtbl->UnlockRect(c.surf);
    }
    g_busy = false;
    c.dirty = false;
    g_nrebuild++;
}

HRESULT STDMETHODCALLTYPE settex_hook(IDirect3DDevice9 *This, DWORD stage, IDirect3DBaseTexture9 *t) {
    Guard g_;   // test shim only
    if (t) {
        auto it = g_comp.find(t);
        if (it != g_comp.end()) {
            Comp &c = it->second;
            if (c.dirty) rebuild(This, t, c);
            if (c.comp && c.glyphs > 0) {
                g_nbind++;
                return g_origSetTex(This, stage, (IDirect3DBaseTexture9 *)c.comp);
            }
        }
    }
    return g_origSetTex(This, stage, t);
}

bool patch_slot(void **vt, size_t slot, void *hook, void **orig) {
    DWORD old;
    if (!VirtualProtect(&vt[slot], 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], 4, old, &old);
    return true;
}

void shim_seen(IDirect3DSurface9 *s, void *key) {
    void **vt = *(void ***)s;
    if (!g_vtSurf) {
        if (patch_slot(vt, offsetof(IDirect3DSurface9Vtbl, UnlockRect) / 4, (void *)unlock_hook,
                       (void **)&g_origUnlock)) {
            g_vtSurf = vt;
            logf("shim: surface vtable %p UnlockRect hooked (orig %p)\n", vt, g_origUnlock);
        }
        IDirect3DDevice9 *dev = nullptr;
        if (SUCCEEDED(s->lpVtbl->GetDevice(s, &dev)) && dev) {
            void **dvt = *(void ***)dev;
            if (patch_slot(dvt, offsetof(IDirect3DDevice9Vtbl, SetTexture) / 4, (void *)settex_hook,
                           (void **)&g_origSetTex)) {
                g_vtDev = dvt;
                logf("shim: device vtable %p SetTexture hooked (orig %p)\n", dvt, g_origSetTex);
            }
            dev->lpVtbl->Release(dev);
        }
    } else if (vt != g_vtSurf) {
        logf("shim: WARNING surface %p has another vtable %p\n", s, vt);
        return;
    }
    g_surfkey[s] = key;
    Comp &c = g_comp[key];
    c.surf = s;
    c.dirty = true;
}
#endif

DWORD WINAPI stats_thread(void *) {
    for (;;) {
        Sleep(5000);
        logf("stats: glyph calls %ld, records %ld (carried by layer copies %ld, by CopyRects %ld of which %ld over a different canvas background, failed self-check %ld, put back to plain after drawing %ld), unknown-font calls %ld, overlays %ld glyphs, textures %u, fills %ld (records dropped %ld), canvas copies %ld (records carried %ld)"   // canvas copies
#ifdef HDTEXT_SHIM
             ", shim binds %ld rebuilds %ld"
#endif
             "\n",
             g_nglyph, g_nrec, g_nprop, g_ncopy, g_ncopydiff, g_nfail, g_nfinalfail, g_nskipfont, g_nover, (unsigned)g_recs.size(), g_nfill, g_nfilldrop, g_nscopy, g_nscopyrec
#ifdef HDTEXT_SHIM
             , g_nbind, g_nrebuild
#endif
        );
    }
    return 0;
}

}  // namespace

extern "C" int hdtext_has_text(void *key) {
    Guard g_;
    auto it = g_recs.find(key);
    return it != g_recs.end() && !it->second.empty();
}


// debug: HDTEXT_DUMP=N dumps the first N overlays of textures at least 256 wide into
// <game>/hdtext_dump/ (src 1x, dst 3x before and after, record list)
static int g_dumpleft = -1;
static void dump_raw(const char *name, const void *p, int pitch, int w, int h) {
    FILE *f = fopen(name, "wb");
    if (!f) return;
    for (int y = 0; y < h; y++) fwrite((const char *)p + y * pitch, 4, w, f);
    fclose(f);
}
static void dump_overlay(void *key, const uint32_t *src1x, int pitch1x, const uint32_t *dst3x, int pitch3x, int w,
                         int h, const char *stage, int seq) {
    char n[256];
    CreateDirectoryA("hdtext_dump", nullptr);
    snprintf(n, sizeof n, "hdtext_dump/%04d_%p_%dx%d_%s_src.raw", seq, key, w, h, stage);
    dump_raw(n, src1x, pitch1x, w, h);
    if (dst3x) {
        snprintf(n, sizeof n, "hdtext_dump/%04d_%p_%dx%d_%s_dst.raw", seq, key, w, h, stage);
        dump_raw(n, dst3x, pitch3x, 3 * w, 3 * h);
    }
}
static int g_dumpseq = 0;
static bool dump_wanted(int w) {
    if (g_dumpleft < 0) {
        const char *e = getenv("HDTEXT_DUMP");
        g_dumpleft = e ? atoi(e) : 0;
    }
    static int minw = -1;
    if (minw < 0) { const char *e = getenv("HDTEXT_DUMPMINW"); minw = e ? atoi(e) : 256; }
    return g_dumpleft > 0 && w >= minw;
}

static int overlay_impl(void *key, const uint32_t *src1x, int pitch1x, uint32_t *dst3x, int pitch3x, int w,
                        int h, bool paint_bg) {
    Guard g_;
    auto it = g_recs.find(key);
    if (it == g_recs.end() || it->second.empty()) return 0;
    std::vector<Rec> &v = it->second;
    std::vector<char> ok = live(v, src1x, pitch1x, w, h);
    std::vector<unsigned char> mark(w * h, 0);
    int seq = -1;
    static std::unordered_map<void *, int> dumped;
    static int every = -1;
    if (every < 0) { const char *e = getenv("HDTEXT_DUMPEVERY"); every = e && atoi(e) > 0 ? atoi(e) : 40; }
    if (dump_wanted(w) && dumped[key]++ % every == 0) {
        seq = g_dumpseq++;
        g_dumpleft--;
        dump_overlay(key, src1x, pitch1x, dst3x, pitch3x, w, h, paint_bg ? "ovl" : "ovt", seq);
        char n[256];
        snprintf(n, sizeof n, "hdtext_dump/%04d_recs.txt", seq);
        FILE *f = fopen(n, "w");
        if (f) {
            for (size_t i = 0; i < v.size(); i++)
                fprintf(f, "%u live=%d font=%d ch=%d cell=%d,%d rect=%d,%d-%d,%d col=%08x ascale=%u err=%.2f\n", (unsigned)i, ok[i],
                        v[i].font, v[i].ch, v[i].cx, v[i].cy, v[i].x0, v[i].y0, v[i].x1, v[i].y1, v[i].color, v[i].ascale, v[i].err);
            fclose(f);
        }
    }
    // drop stale records
    std::vector<Rec> keep;
    keep.reserve(v.size());
    for (size_t i = 0; i < v.size(); i++)
        if (ok[i]) keep.push_back(std::move(v[i]));
    {
        static int nlog = 0;
        if (nlog < 300) {
            nlog++;
            logf("overlay key %p %dx%d: records %u live %u\n", key, w, h, (unsigned)v.size(), (unsigned)keep.size());
        }
    }
    v.swap(keep);
    std::vector<char> draw(v.size(), 1);
    restrict_to_hd(v, draw);
    // background first (oldest record touching a pixel wins), then the HD glyphs: effect-layer
    // records (shadow/glow mask) below all letter records, each class oldest first
    std::fill(mark.begin(), mark.end(), 0);
    int n = 0;
    if (paint_bg)
        for (size_t ri = 0; ri < v.size(); ri++) {
            const Rec &r = v[ri];
            if (!draw[ri]) continue;
            int rw = r.x1 - r.x0;
            for (int y = r.y0; y < r.y1; y++)
                for (int x = r.x0; x < r.x1; x++) {
                    if (mark[y * w + x]) continue;
                    mark[y * w + x] = 1;
                    uint32_t b = r.before[(y - r.y0) * rw + x - r.x0];
                    for (int k = 0; k < 3; k++) {
                        uint32_t *o = (uint32_t *)((char *)dst3x + (3 * y + k) * pitch3x) + 3 * x;
                        o[0] = o[1] = o[2] = b;
                    }
                }
        }
    for (int pass = 0; pass < 2; pass++)
    for (size_t ri = 0; ri < v.size(); ri++) {
        const Rec &r = v[ri];
        if (!draw[ri] || (pass == 0) != is_fx(r)) continue;
        const Font &F = g_fonts[r.font];
        const unsigned char *g = F.glyph[r.ch];
        int gw = 3 * F.widths[r.ch];
        unsigned ca = r.color >> 24;
        uint32_t col = r.color & 0xffffff;
        // ListText: the same glyph painted again 1 game px away by a newer record (emboss or
        // drop shadow of lists, drop-downs, edit fields) is 3 real px away at 3x and reads as a
        // doubled letter with sharp strokes; this older copy is drawn 1 real px from the newer one
        int sx = 0, sy = 0;
        if (g_close_copies)
            for (size_t j = ri + 1; j < v.size(); j++) {
                const Rec &q = v[j];
                int ddx = q.cx - r.cx, ddy = q.cy - r.cy;
                if (draw[j] && q.font == r.font && q.ch == r.ch && ddx >= -1 && ddx <= 1 && ddy >= -1 && ddy <= 1 &&
                    (ddx || ddy) && q.color != r.color) {
                    sx = 2 * ddx;
                    sy = 2 * ddy;
                    break;
                }
            }
        // a letter that the game covered with a glow pass AFTERWARDS keeps its full strength (the
        // glow blend leaves the letter cores as they are; the measured layer strength would let the
        // glow colour show through the HD letter)
        unsigned asc = r.ascale;
        if (pass == 1 && asc < 256)
            for (size_t j = ri + 1; j < v.size(); j++)
                if (draw[j] && is_fx(v[j]) && v[j].x0 < r.x1 && r.x0 < v[j].x1 && v[j].y0 < r.y1 && r.y0 < v[j].y1) {
                    asc = 256;
                    break;
                }
        // under a later glow pass the game's letter pixels are not the plain letter colour (cores
        // white, edges tinted pale yellow by the glow): take the colour from the game's own 1x pixels
        // (bilinear), the shape from the HD glyph
        bool tint = false;   // tried (colour from the 1x pixels): edges turned orange and blotchy; letters stay plain white
        for (int Y = 3 * r.y0 + sy; Y < 3 * r.y1 + sy; Y++) {
            if (Y < 0 || Y >= 3 * h) continue;
            uint32_t *o = (uint32_t *)((char *)dst3x + Y * pitch3x);
            const unsigned char *gr = g + (Y - sy - 3 * r.cy) * gw;
            for (int X = 3 * r.x0 + sx; X < 3 * r.x1 + sx; X++) {
                if (X < 0 || X >= 3 * w) continue;
                unsigned a = gr[X - sx - 3 * r.cx];
                if (a) {
                    unsigned aa = (a * ca + 127) / 255 * asc / 256;
                    uint32_t c = col;
                    if (tint) {
                        float fx = (X + 0.5f) / 3 - 0.5f, fy = (Y + 0.5f) / 3 - 0.5f;
                        int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                        float tx = fx - x0, ty = fy - y0;
                        float acc[3] = {0, 0, 0};
                        for (int j = 0; j < 4; j++) {
                            int xx = x0 + (j & 1), yy = y0 + (j >> 1);
                            if (xx < r.x0) xx = r.x0;
                            if (xx >= r.x1) xx = r.x1 - 1;
                            if (yy < r.y0) yy = r.y0;
                            if (yy >= r.y1) yy = r.y1 - 1;
                            float wgt = ((j & 1) ? tx : 1 - tx) * ((j >> 1) ? ty : 1 - ty);
                            uint32_t p = ((const uint32_t *)((const char *)src1x + yy * pitch1x))[xx];
                            for (int k = 0; k < 3; k++) acc[k] += wgt * ((p >> (8 * k)) & 255);
                        }
                        c = 0;
                        for (int k = 0; k < 3; k++) c |= (uint32_t)(acc[k] + 0.5f) << (8 * k);
                    }
                    over(o[X], c, aa > 255 ? 255 : aa);
                }
            }
        }
        n++;
    }
    // final check (never worse than off): box the finished 3x result back down to 1x over every
    // drawn glyph and compare with what the game has in its texture; a mismatch (wrong layer order,
    // a blend the "over" model does not describe, ...) puts that glyph and every drawn glyph touching
    // it back to the plain point-sampled look (nearest 3x of the game's own pixels)
    {
        auto pm = [](uint32_t p, int sh) { return (double)((p >> sh) & 255) * (sh == 24 ? 255 : (p >> 24)) / 255.0; };
        std::vector<char> bad(v.size(), 0);
        for (size_t ri = 0; ri < v.size(); ri++) {
            if (!draw[ri]) continue;
            const Rec &r = v[ri];
            int rw = r.x1 - r.x0;
            double diff = 0, ink = 0;
            for (int y = r.y0; y < r.y1; y++) {
                const uint32_t *srow = (const uint32_t *)((const char *)src1x + y * pitch1x);
                for (int x = r.x0; x < r.x1; x++) {
                    double acc[4] = {0, 0, 0, 0};
                    for (int sy = 0; sy < 3; sy++) {
                        const uint32_t *o = (const uint32_t *)((const char *)dst3x + (3 * y + sy) * pitch3x) + 3 * x;
                        for (int sx = 0; sx < 3; sx++)
                            for (int c = 0; c < 4; c++) acc[c] += pm(o[sx], 8 * c);
                    }
                    uint32_t b = r.before[(y - r.y0) * rw + x - r.x0];
                    for (int c = 0; c < 4; c++) {
                        double hv = acc[c] / 9, a = pm(srow[x], 8 * c), bb = pm(b, 8 * c);
                        diff += fabs(hv - a);
                        ink += fabs(a - bb) + fabs(hv - bb);
                    }
                }
            }
            float e = ink > 0 ? (float)(diff / ink) : 0.f;
            if (e > g_maxerr) bad[ri] = 1;
        }
        bool any = false, changed = true;
        for (char b : bad) any |= b;
        while (any && changed) {
            changed = false;
            for (size_t i = 0; i < v.size(); i++) {
                if (!draw[i] || bad[i]) continue;
                for (size_t j = 0; j < v.size(); j++)
                    if (bad[j] && v[i].x0 < v[j].x1 && v[j].x0 < v[i].x1 && v[i].y0 < v[j].y1 && v[j].y0 < v[i].y1) {
                        bad[i] = 1;
                        changed = true;
                        break;
                    }
            }
        }
        if (any) {
            for (size_t ri = 0; ri < v.size(); ri++) {
                if (!bad[ri]) continue;
                const Rec &r = v[ri];
                for (int y = r.y0; y < r.y1; y++) {
                    const uint32_t *srow = (const uint32_t *)((const char *)src1x + y * pitch1x);
                    for (int k = 0; k < 3; k++) {
                        uint32_t *o = (uint32_t *)((char *)dst3x + (3 * y + k) * pitch3x);
                        for (int x = r.x0; x < r.x1; x++) o[3 * x] = o[3 * x + 1] = o[3 * x + 2] = srow[x];
                    }
                }
                n--;
                g_nfinalfail++;
            }
        }
    }
    if (seq >= 0) {
        char n2[256];
        snprintf(n2, sizeof n2, "hdtext_dump/%04d_%p_%dx%d_after_dst.raw", seq, key, w, h);
        dump_raw(n2, dst3x, pitch3x, 3 * w, 3 * h);
    }
    g_nover += n;
    return n;
}

extern "C" int hdtext_overlay(void *key, const uint32_t *src1x, int pitch1x, uint32_t *dst3x, int pitch3x,
                              int w, int h) {
    return overlay_impl(key, src1x, pitch1x, dst3x, pitch3x, w, h, true);
}

extern "C" int hdtext_overlay_text(void *key, const uint32_t *src1x, int pitch1x, uint32_t *dst3x,
                                   int pitch3x, int w, int h) {
    return overlay_impl(key, src1x, pitch1x, dst3x, pitch3x, w, h, false);
}

// The proxy re-uses its texture wrapper when the game creates a new texture at the same address:
// the old texture's records must go.
extern "C" void hdtext_forget(void *key) {
    Guard g_;
    g_recs.erase(key);
}

extern "C" void hdtext_set_builtin_plain(int on) { g_builtin_plain = on; }
extern "C" void hdtext_set_close_copies(int on) { g_close_copies = on; g_scopy_on = on; }   // ListText also gates the canvas copy carry-over

extern "C" int hdtext_strip(void *key, uint32_t *img1x, int pitch, int w, int h) {
    Guard g_;
    auto it = g_recs.find(key);
    if (it == g_recs.end() || it->second.empty()) return 0;
    const std::vector<Rec> &v = it->second;
    std::vector<char> ok = live(v, img1x, pitch, w, h);
    restrict_to_hd(v, ok);
    std::vector<unsigned char> mark(w * h, 0);
    int n = 0;
    for (size_t i = 0; i < v.size(); i++) {
        if (!ok[i]) continue;
        const Rec &r = v[i];
        int rw = r.x1 - r.x0;
        for (int y = r.y0; y < r.y1; y++) {
            uint32_t *row = (uint32_t *)((char *)img1x + y * pitch);
            for (int x = r.x0; x < r.x1; x++) {
                if (mark[y * w + x]) continue;
                mark[y * w + x] = 1;
                row[x] = r.before[(y - r.y0) * rw + x - r.x0];
            }
        }
        n++;
    }
    return n;
}

extern "C" int hdtext_install(const char *pack_path) {
    if (g_installed) return g_installed;
    InitializeCriticalSection(&g_cs);
    char path[MAX_PATH], logp[MAX_PATH];
    HMODULE me = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&hdtext_install, &me);
    GetModuleFileNameA(me, path, MAX_PATH);
    char *sl = strrchr(path, '\\');
    if (sl) sl[1] = 0;
    snprintf(logp, sizeof logp, "%shdtext.log", path);
    {
        // hdtext.log only with logging on: HDTEXT_LOG=1, or the proxy's D3D8PROXY_LOG
        // set to anything but 0; the 5-s stats line is written to it only then
        const char *l1 = getenv("HDTEXT_LOG");
        bool on = l1 && *l1 ? *l1 != '0' : ee_log_on();
        if (on) g_log = fopen(logp, "w");
    }
    if (!pack_path) {
        strncat(path, "hdtext_fonts.bin", MAX_PATH - strlen(path) - 1);
        pack_path = path;
    }
    if (const char *me_ = getenv("HDTEXT_MAXERR")) g_maxerr = (float)atof(me_);
    const char *env = getenv("HDTEXT");
    if (env && env[0] == '0') {
        logf("HDTEXT=0: off\n");
        return g_installed = -1;
    }
    FILE *f = fopen(pack_path, "rb");
    if (!f) {
        logf("pack %s not found: off\n", pack_path);
        return g_installed = -2;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_pack.resize(sz);
    if (fread(g_pack.data(), 1, sz, f) != (size_t)sz) sz = 0;
    fclose(f);
    if (sz < 12 || memcmp(g_pack.data(), "HDTX", 4)) {
        logf("pack %s bad: off\n", pack_path);
        return g_installed = -3;
    }
    const unsigned char *p = g_pack.data() + 12, *end = g_pack.data() + sz;
    unsigned nf = *(const uint32_t *)(g_pack.data() + 8);
    unsigned ver = *(const uint32_t *)(g_pack.data() + 4);
    // fonts left plain: hdtext_plain.txt next to the pack (one name per line, # = comment) or,
    // without that file, the built-in list. main_hq_11: the game draws its lists/drop-downs twice,
    // 1 game px apart (shadow/emboss at draw time), which reads as a doubled glyph with sharp
    // HD strokes.
    std::vector<std::string> plain;
    {
        std::string pp = pack_path;
        size_t sl2 = pp.find_last_of("\\/");
        pp = (sl2 == std::string::npos ? std::string() : pp.substr(0, sl2 + 1)) + "hdtext_plain.txt";
        FILE *pf = fopen(pp.c_str(), "r");
        if (pf) {
            char line[128];
            while (fgets(line, sizeof line, pf)) {
                std::string l = line;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
                if (!l.empty() && l[0] != '#') plain.push_back(l);
            }
            fclose(pf);
            logf("plain list from %s: %u fonts\n", pp.c_str(), (unsigned)plain.size());
        } else
            if (g_builtin_plain) plain = {"main_hq_11"};
    }
    for (unsigned i = 0; i < nf; i++) {
        if (p + 4 + 32 + 256 > end) { logf("pack truncated\n"); return g_installed = -3; }
        Font F;
        F.type = p[0];
        F.h = p[1];
        F.hqoff = p[2];
        if (p[3] != 3) { logf("pack scale %d unsupported\n", p[3]); return g_installed = -3; }
        p += 4;
        F.name[0] = 0;
        if (ver >= 2) {
            memcpy(F.name, p, 32);
            F.name[31] = 0;
            p += 32;
        }
        F.plain = false;
        for (auto &n : plain) F.plain |= n == F.name;
        if (F.plain) logf("font %s: plain (HD off)\n", F.name);
        memcpy(F.widths, p, 256);
        p += 256;
        for (int c = 0; c < 256; c++) {
            F.glyph[c] = p;
            p += 9 * F.widths[c] * F.h;
        }
        if (p > end) { logf("pack truncated\n"); return g_installed = -3; }
        g_fonts.push_back(F);
    }
    logf("pack %s: %u fonts\n", pack_path, nf);
    // Review finding: every program address in this file is fixed at the program's usual place
    // 0x400000 (no load-address correction). If the program was loaded elsewhere, reading them would crash:
    // sharp text stays off then (logged). Under Proton the program is always at 0x400000.
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        logf("program loaded at %p, not at 0x400000: sharp text off\n", (void *)GetModuleHandleA(NULL));
        ee_log("hdtext: program loaded at %p, not at 0x400000: sharp text off (fixed addresses)", (void *)GetModuleHandleA(NULL));
        return g_installed = -6;
    }
    // verify code bytes, then patch the two vtable slots in place
    if (memcmp((void *)FN_MONO, BYTES_MONO, sizeof BYTES_MONO) || memcmp((void *)FN_HQ, BYTES_HQ, sizeof BYTES_HQ) ||
        memcmp((void *)FN_LOCK, BYTES_LOCK, sizeof BYTES_LOCK) ||
        memcmp((void *)FN_UNLOCK, BYTES_UNLOCK, sizeof BYTES_UNLOCK)) {
        logf("code bytes differ (other exe?): off\n");
        return g_installed = -4;
    }
    bool a = patch_ptr(SLOT11_MONO, FN_MONO, (uintptr_t)&paint_hook);
    bool b = patch_ptr(SLOT11_HQ, FN_HQ, (uintptr_t)&paint_hook);
    logf("slot 11 hooks: mono %s, hq %s\n", a ? "ok" : "FAILED", b ? "ok" : "FAILED");
    if (!a || !b) {
        if (a) patch_ptr(SLOT11_MONO, (uintptr_t)&paint_hook, FN_MONO);
        if (b) patch_ptr(SLOT11_HQ, (uintptr_t)&paint_hook, FN_HQ);
        return g_installed = -5;
    }
    int c = 0;
    if (!memcmp((void *)FN_BLIT, BYTES_BLIT, sizeof BYTES_BLIT))
        for (uintptr_t at : CALL_BLIT_EE) {
            unsigned char e[5] = {0xe8};
            *(int32_t *)(e + 1) = (int32_t)(FN_BLIT - (at + 5));
            c += patch_call(at, e, (void *)&blit_hook);
        }
    logf("layer composite hook: %d of 3 call sites\n", c);
    int nf_ok = 0;
    for (uintptr_t at : CALL_FILL) {
        unsigned char e[5] = {0xe8};
        *(int32_t *)(e + 1) = (int32_t)(FN_FILL - (at + 5));
        nf_ok += patch_call(at, e, (void *)&fill_hook);
    }
    logf("surface fill hooks: %d of %d call sites\n", nf_ok, (int)(sizeof CALL_FILL / sizeof CALL_FILL[0]));
    // canvas copies (eSurface::CopyRect 0x7447f0) carry the glyph records
    int ns_ok = 0;
    if (!memcmp((void *)FN_SCOPY, BYTES_SCOPY, sizeof BYTES_SCOPY))
        for (uintptr_t at : CALL_SCOPY) {
            unsigned char e[5] = {0xe8};
            *(int32_t *)(e + 1) = (int32_t)(FN_SCOPY - (at + 5));
            ns_ok += patch_call(at, e, (void *)&scopy_hook);
        }
    logf("canvas copy hooks: %d of %d call sites\n", ns_ok, (int)(sizeof CALL_SCOPY / sizeof CALL_SCOPY[0]));
    ee_log("hdtext: canvas copy hook: %d of %d call sites, carry-over %s", ns_ok,
           (int)(sizeof CALL_SCOPY / sizeof CALL_SCOPY[0]), g_scopy_on ? "on (ListText=1)" : "off (ListText=0)");
    if (g_log) CloseHandle(CreateThread(nullptr, 0, stats_thread, nullptr, 0, nullptr));
    return g_installed = (int)nf;
}

#ifdef HDTEXT_ASI
BOOL WINAPI DllMain(HINSTANCE, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) hdtext_install(nullptr);
    return TRUE;
}
#endif
