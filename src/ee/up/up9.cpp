// Interface-texture upscaler, see up9.h.
// The game uploads textures from more than one thread (a crash in an unlocked map proved it), so every
// entry point takes g_uplk (uncontended EnterCriticalSection costs about 20 ns).
#include "up9.h"
#include "compat9.h"

#ifdef UP_HDTEXT
#include "hdtext9.h"   // HD glyphs painted into the 3x copies
#endif
#include <unordered_map>
#include <vector>
#include <set>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace {
const GUID kIID_Tex9up = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};

struct UpTex {
    IDirect3DTexture9 *real = nullptr;   // the game's texture (real object)
    UINT w = 0, h = 0;
    bool eligible = false;
    bool alpha = true;
    bool dirty = true;
    IDirect3DTexture9 *shadow = nullptr;
    unsigned long long hash = 0;
    DWORD last_use = 0;                  // GetTickCount of the last interface use
    unsigned refreshes = 0;
    bool hdtext = false;                 // last refresh painted HD glyphs
    bool photo = false;                  // noisy photographic content without text: keep the original
    bool painted = false;                // painted art on a transparent sheet (opaque rule): cache file, else xBRZ
    void *key = nullptr;                 // the game's texture (wrapper) = hdtext key
    bool logged_stretch = false;
    bool rt = false;                     // a render-target texture (drag label)
    // drag sprite: proxy-owned 3x render target used in place of this game render target
    IDirect3DTexture9 *comp = nullptr;
    IDirect3DSurface9 *comp_s = nullptr; // its level 0
    IDirect3DSurface9 *game_s = nullptr; // the game's real level-0 surface (not referenced)
    bool comp_off = false;               // an unexpected use was seen: this texture stays 1x for good
    bool comp_valid = false;             // the companion holds the picture (bind it instead)
    // TileEdges: texel rectangle of the 1:1 single-quad draws of this texture (frame pieces:
    // 16x16 textures with the art in a 10x10 rect). 0 = none seen, 1 = one rect, 2 = several/other
    int rstate = 0;
    int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
    // SmoothShrink: copies at the exact real-pixel size the texture is drawn at
    struct Mini { UINT W = 0, H = 0; IDirect3DTexture9 *t = nullptr; unsigned long long hash = 0; DWORD last_use = 0; };
    std::vector<Mini> minis;
    bool mini_dirty = true;              // the original was written since the copies were made
    unsigned long long mini_hash = 0;    // content hash the copies were made from
    bool mini_checked = false;           // format/pool/size checked
    bool mini_ok = false;                // may get copies
    UINT shrink_logW = 0, shrink_logH = 0;   // last drawn size written to the log
    unsigned long long x4_hash = 0;      // content hash the 4x-file lookup below was made for
    bool x4_none = false;                // no uiscale_hd\<hash>_<w>x<h>_x4.raw for that content
};

bool g_on = false;          // feature configured on
bool g_mirrors = false;     // buffer mirrors + half-pixel classification: Upscale=1 or PixelFix=1
bool g_photosmooth = false; // PhotoSmooth=1: Lanczos 3x copies for photographic art without cache files
bool g_painted = true;     // PaintedArt=1: opaque-pixel rule for art without text, see refresh()
bool g_tileedges = true;   // TileEdges=1: xBRZ copies of drawn tiles continue their border texels
bool g_thick = true;       // ThickLines=1: interface lines drawn 1 game px wide (up_thick_lines)
bool g_dblfix = false;      // ListText=1 (hdtext pulls 1-px shadow/emboss copies together)
bool g_active = true;       // runtime toggle (test file upscale.off), shadows only bound while true
bool g_toggle_file = false; // D3D8PROXY_UPFILE=1: check <game>\upscale.off every 30 frames
bool g_hd = false;          // hdtext installed (module linked in, factor 3, pack loaded)
bool g_stats = false;
bool g_hdcache = true;      // load uiscale_hd/<hash>_<w>x<h>.raw (offline 3x enlargements) when present
bool g_hddump = false;      // D3D8PROXY_UPDUMP=1: write photographic textures into the uiscale_hd todo folder
std::unordered_map<unsigned long long, int> g_hdknown;   // hash -> 1 file present, 0 absent
// (D3D8PROXY_UPSTATS=1: upscale_stats.txt next to the game every 5 s)
int g_f = 3;
std::string g_dir;
#define g_uplk g_ee_cs   // the proxy's hook lock (src/ee/d3dhook.cpp)
struct UpLock { UpLock() { EnterCriticalSection(&g_uplk); } ~UpLock() { LeaveCriticalSection(&g_uplk); } };

std::unordered_map<void *, UpTex *> g_tex;          // texture wrapper -> info
std::unordered_map<void *, void *> g_surf0;         // real level-0 surface -> texture wrapper

IDirect3DBaseTexture9 *g_game_w[8];     // what the game bound (wrapper)
IDirect3DBaseTexture9 *g_game_r[8];     // its real object
bool g_game_known[8];
IDirect3DBaseTexture9 *g_bound[8];      // what is really bound in the real device
UpTex *g_stage_u[8];
bool g_stage_looked[8];
bool g_ortho = false;
// DragText: render-target companions. The game paints its drag sprite (item picture + name
// label) once into a 256x128 render-target texture and draws that texture 1:1 every frame; the
// upscaler leaves render targets alone, so the sprite showed as 3x3 blocks. While the game renders
// into that texture (no depth buffer), the real target is a 3x companion (viewport and Clear scaled,
// shadows bound like for the back buffer) and the companion is bound wherever the texture is used.
bool g_comp_on = true;                 // uiscale.ini DragText=1 (default), env D3D8PROXY_DRAGTEXT
UpTex *g_rt_comp = nullptr;            // render target currently redirected to this texture's companion
bool g_rt_real_comp = false;           // the device's real target is a companion surface (stays set when
                                       // the texture is re-created and g_rt_comp is cleared)
int g_comp_n = 0;                      // companions alive
std::vector<IDirect3DTexture9 *> g_comp_orphans;   // companions of re-created textures, released at Reset/next use
float g_proj11 = 0, g_proj22 = 0;   // current projection scale (ThickLines guard)
bool g_shrink = true;                  // uiscale.ini SmoothShrink=1 (default), env D3D8PROXY_SMOOTHSHRINK
unsigned long long st_draw_shrunk = 0, st_shrink_made = 0;

// statistics
LARGE_INTEGER g_qpf;
const unsigned kFlatPercent = 65;   // below this share of similar neighbours = photographic
const unsigned kOpaqueFlatPercent = 80;   // same over opaque pairs, RGB only (painted art, no text)
unsigned long long st_painted_xbrz = 0;
unsigned long long st_hdload = 0, st_hddump = 0, st_text = 0, st_glyphs = 0, st_photo = 0, st_reused = 0, st_refresh = 0, st_same = 0, st_created = 0, st_skipped = 0, st_evicted = 0, st_failed = 0;
double st_ms = 0, st_ms_max = 0;
unsigned long long st_draw_ui = 0, st_draw_shadow = 0, st_draw_stretched = 0, st_draw_unknown = 0;
unsigned long long st_px = 0;
// per 5 s window
unsigned long long w_refresh = 0, w_same = 0;
double w_ms = 0, w_ms_max = 0;
DWORD w_t0 = 0;
long w_f0 = 0;
std::string g_stats_hist;   // history lines

double now_ms() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / g_qpf.QuadPart;
}

UpTex *info_for(IDirect3DBaseTexture9 *wrapper, IDirect3DBaseTexture9 *real) {
    auto it = g_tex.find(wrapper);
    if (it != g_tex.end())
        return it->second;
    UpTex *u = new UpTex;
    u->key = wrapper;
    g_tex[wrapper] = u;
    if (!real || real->GetType() != D3DRTYPE_TEXTURE)
        return u;
    IDirect3DTexture9 *t = (IDirect3DTexture9 *)real;
    D3DSURFACE_DESC d;
    if (FAILED(t->GetLevelDesc(0, &d)))
        return u;
    u->real = t;
    u->w = d.Width;
    u->h = d.Height;
    u->alpha = d.Format == D3DFMT_A8R8G8B8;
    u->rt = (d.Usage & D3DUSAGE_RENDERTARGET) != 0;
    u->eligible = (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) && d.Pool == D3DPOOL_MANAGED &&
                  !(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) &&
                  ((d.Width <= 1024 && d.Height <= 1024) ||
                   (d.Width <= 2048 && d.Height <= 64)) &&   // 2048-wide text strips (loading-screen status line, 2048x16)
                  d.Width >= 2 && d.Height >= 2;
    if (!u->eligible) {
        st_skipped++;
        log_text("[upscale] not eligible: %ux%u %s pool=%d usage=0x%lx", d.Width, d.Height, fmtname(d.Format).c_str(),
                 (int)d.Pool, (unsigned long)d.Usage);
        // test switch D3D8PROXY_UPDUMPALL=1: level 0 of a too-large interface texture once ->
        // uiscale_hd\all\big_<key>_<w>x<h>.raw (1x BGRA), to see what the 2048-wide ones are
        const char *e = getenv("D3D8PROXY_UPDUMPALL");
        D3DLOCKED_RECT lr;
        if (e && *e == '1' && d.Format == D3DFMT_A8R8G8B8 && d.Pool == D3DPOOL_MANAGED &&
            SUCCEEDED(t->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) {
            std::string dir = g_dir + "uiscale_hd\\";
            CreateDirectoryA(dir.c_str(), NULL);
            dir += "all\\";
            CreateDirectoryA(dir.c_str(), NULL);
            char b[96];
            snprintf(b, sizeof b, "big_%p_%ux%u.raw", (void *)wrapper, d.Width, d.Height);
            HANDLE f = CreateFileA((dir + b).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD wr;
                for (UINT y = 0; y < d.Height; y++) WriteFile(f, (const BYTE *)lr.pBits + (size_t)y * lr.Pitch, d.Width * 4, &wr, NULL);
                CloseHandle(f);
            }
            t->UnlockRect(0);
        }
    }
    return u;
}

std::vector<uint32_t> g_src, g_dst, g_tmp;

// Offline enlargement cache (uiscale_hd next to the game): <hash>_<w>x<h>.raw = (3w x 3h) BGRA
// bytes, made by scripts/hdcache.py with Real-ESRGAN from the 1x dumps in uiscale_hd\todo\.
std::string hd_name(unsigned long long hs, UINT w, UINT h) {
    char b[64];
    snprintf(b, sizeof b, "%016llx_%ux%u.raw", hs, w, h);
    return b;
}
bool hd_load(unsigned long long hs, UINT w, UINT h, std::vector<uint32_t> &dst) {
    auto it = g_hdknown.find(hs);
    if (it != g_hdknown.end() && !it->second)
        return false;
    const bool first_look = it == g_hdknown.end();   // log each content key once
    std::string fn = g_dir + "uiscale_hd\\" + hd_name(hs, w, h);
    HANDLE f = CreateFileA(fn.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        g_hdknown[hs] = 0;
        log_text("[cache] key %016llx %ux%u: cache file not found (uiscale_hd\\%s)", hs, w, h, hd_name(hs, w, h).c_str());
        return false;
    }
    if (first_look)
        log_text("[cache] key %016llx %ux%u: cache file found (uiscale_hd\\%s)", hs, w, h, hd_name(hs, w, h).c_str());
    size_t n = (size_t)w * g_f * h * g_f;
    dst.resize(n);
    DWORD got = 0;
    BOOL ok = ReadFile(f, dst.data(), (DWORD)(n * 4), &got, NULL) && got == n * 4;
    CloseHandle(f);
    g_hdknown[hs] = ok ? 1 : 0;
    if (!ok)
        log_text("[upscale] cache file %s has the wrong size, ignored", fn.c_str());
    return ok;
}
void hd_dump(unsigned long long hs, UINT w, UINT h, const std::vector<uint32_t> &src) {
    std::string dir = g_dir + "uiscale_hd\\";
    CreateDirectoryA(dir.c_str(), NULL);
    dir += "todo\\";
    CreateDirectoryA(dir.c_str(), NULL);
    std::string fn = dir + hd_name(hs, w, h);
    HANDLE f = CreateFileA(fn.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;   // already dumped
    DWORD wr;
    WriteFile(f, src.data(), (DWORD)(src.size() * 4), &wr, NULL);
    CloseHandle(f);
    st_hddump++;
}

// test helper (D3D8PROXY_UPDUMPALL=1): level 0 of a texture that is drawn stretched -> uiscale_hd\all
void dump_stretched(UpTex *u) {
    const char *e = getenv("D3D8PROXY_UPDUMPALL");
    if (!(e && *e == '1') || !u->real) return;
    D3DLOCKED_RECT lr;
    if (FAILED(u->real->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return;
    std::vector<uint32_t> px((size_t)u->w * u->h);
    for (UINT y = 0; y < u->h; y++) memcpy(&px[(size_t)y * u->w], (const BYTE *)lr.pBits + (size_t)y * lr.Pitch, u->w * 4);
    u->real->UnlockRect(0);
    std::string dir = g_dir + "uiscale_hd\\";
    CreateDirectoryA(dir.c_str(), NULL);
    dir += "all\\";
    CreateDirectoryA(dir.c_str(), NULL);
    char b[96];
    snprintf(b, sizeof b, "%p_%ux%u_stretched.raw", u->key, u->w, u->h);
    HANDLE f = CreateFileA((dir + b).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f, px.data(), (DWORD)(px.size() * 4), &wr, NULL); CloseHandle(f); }
}

void refresh(IDirect3DDevice9 *dev, UpTex *u) {
    u->dirty = false;
    double t0 = now_ms();
    D3DLOCKED_RECT lr;
    if (FAILED(u->real->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) {
        st_failed++;
        return;
    }
    const UINT w = u->w, h = u->h;
    g_src.resize((size_t)w * h);
    for (UINT y = 0; y < h; y++)
        memcpy(&g_src[(size_t)y * w], (const BYTE *)lr.pBits + (size_t)y * lr.Pitch, w * 4);
    u->real->UnlockRect(0);
    if (!u->alpha)
        for (auto &p : g_src) p |= 0xff000000u;
    unsigned long long hs = 1469598103934665603ULL;
    for (uint32_t p : g_src) { hs ^= p; hs *= 1099511628211ULL; }
    if (u->shadow && hs == u->hash) {
        st_same++;
        w_same++;
        return;
    }
    u->hash = hs;
    // Photographic / noisy art (stone, grass, photos) turns blotchy under xBRZ; pixel-art style
    // content (text, line art, flat panels) has many neighbours of (nearly) equal colour.
    bool text = false, photo = false, painted = false;
    {
        // old share: transparent pairs count as similar (kept for text textures)
        // opaque share (classification rule): RGB only, over pairs where both
        // pixels have alpha >= 16; painted art on mostly transparent sheets (inventory figure, bag,
        // wood weapon bar, helmet, ammo, map pages) is below kOpaqueFlatPercent, line art above
        size_t similar = 0, total = 0, osim = 0, otot = 0;
        for (UINT y = 0; y < h; y++) {
            const uint32_t *r = &g_src[(size_t)y * w];
            for (UINT x = 0; x + 1 < w; x++) {
                uint32_t a = r[x], b = r[x + 1];
                bool ta = (a >> 24) < 16, tb = (b >> 24) < 16;
                if (ta && tb) { similar++; total++; continue; }   // both transparent
                int d = 0;
                for (int sh = 0; sh < 24; sh += 8) {
                    int dd = (int)((a >> sh) & 255) - (int)((b >> sh) & 255);
                    if (dd < 0) dd = -dd;
                    if (dd > d) d = dd;
                }
                if (!ta && !tb) { osim += d <= 6; otot++; }
                int da = (int)(a >> 24) - (int)(b >> 24);
                if (da < 0) da = -da;
                if (da > d) d = da;
                similar += d <= 6;
                total++;
            }
        }
        photo = total && similar * 100 < total * kFlatPercent;
        unsigned oshare = otot > 50 ? (unsigned)(osim * 100 / otot) : 100;
#ifdef UP_HDTEXT
        text = g_hd && hdtext_has_text(u->key);
#endif
        // painted = photographic only by the opaque rule: cache file if present, else xBRZ as before
        painted = g_painted && !text && !photo && otot > 50 && osim * 100 < otot * kOpaqueFlatPercent;
        if (photo != u->photo || painted != u->painted || u->refreshes == 0)
            log_text("[upscale] %ux%u similar-neighbour share %u%% (opaque %u%%) -> %s", w, h,
                     (unsigned)(total ? similar * 100 / total : 0), oshare,
                     photo ? "photographic, original kept" : painted ? "painted (cache, else xBRZ)" : "xBRZ");
        u->photo = photo && !text;
        u->painted = painted;
        u->hdtext = text;
    }
    const UINT W = w * g_f, H = h * g_f;
    {
        // test switch D3D8PROXY_UPDUMPALL=1: every refreshed interface texture once per content hash
        // -> <game>\uiscale_hd\all\<hash>_<w>x<h>_<kind>_<texture pointer>.raw (1x BGRA), for choosing panel art
        static int dall = -1;
        if (dall < 0) { const char *e = getenv("D3D8PROXY_UPDUMPALL"); dall = e && *e == '1'; }
        if (dall) {
            std::string dir = g_dir + "uiscale_hd\\";
            CreateDirectoryA(dir.c_str(), NULL);
            dir += "all\\";
            CreateDirectoryA(dir.c_str(), NULL);
            char b[96];
            snprintf(b, sizeof b, "%016llx_%ux%u_%s_%p.raw", hs, w, h, text ? "text" : u->photo ? "photo" : painted ? "painted" : "xbrz", u->key);
            HANDLE f = CreateFileA((dir + b).c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD wr;
                WriteFile(f, g_src.data(), (DWORD)(g_src.size() * 4), &wr, NULL);
                CloseHandle(f);
            }
        }
    }
    bool cached = false;
    if ((u->photo || painted) && g_f == 3) {
        if (g_hddump)
            hd_dump(hs, w, h, g_src);
        if (g_hdcache && hd_load(hs, w, h, g_dst)) {
            cached = true;
            u->photo = false;   // bind the offline enlargement
            st_hdload++;
        } else if (painted)
            st_painted_xbrz++;   // no cache file: today's xBRZ copy (never worse than before the rule)
    }
    if (u->photo && !g_photosmooth) {
        st_photo++;
        return;
    }
    g_dst.resize((size_t)W * H);
    if (cached) {
    } else if (u->photo) {
        // PhotoSmooth=1: photographic art without an offline cache file gets a Lanczos 3x copy
        // instead of the original (the game samples interface art with POINT: 3x3 blocks)
        up_smooth_image(g_src.data(), (int)w, (int)h, g_dst.data(), g_f);
        st_photo++;
    } else if (!text) {
        // TileEdges=1: xBRZ reads everything outside the image as transparent, and so does the
        // transparent padding around the art of a frame piece; where the art continues into the next
        // tile, it rounded the line ends: one sub-pixel of each line lost alpha (dark dot) and a light
        // speck appeared next to it at every tile joint. For a texture drawn 1:1 as one
        // quad with one texel rectangle, everything outside which is transparent, that rectangle is
        // scaled with its border texels repeated 2 texels outwards, like the quad edge cuts the art.
        bool tile = false;
        if (g_tileedges && u->rstate == 1) {
            tile = true;
            for (UINT y = 0; y < h && tile; y++)
                for (UINT x = 0; x < w; x++)
                    if ((x < (UINT)u->rx0 || x >= (UINT)u->rx1 || y < (UINT)u->ry0 || y >= (UINT)u->ry1) && (g_src[(size_t)y * w + x] >> 24)) {
                        tile = false;
                        break;
                    }
        }
        if (!tile) {
            up_scale_image(g_src.data(), (int)w, (int)h, g_dst.data(), g_f, u->alpha);
        } else {
            const int P = 2, rw = u->rx1 - u->rx0, rh = u->ry1 - u->ry0, pw = rw + 2 * P, ph = rh + 2 * P;
            if (u->rx0 || u->ry0 || rw != (int)w || rh != (int)h)
                up_scale_image(g_src.data(), (int)w, (int)h, g_dst.data(), g_f, u->alpha);   // outside the rect: as before
            // A side is continued only where the art runs straight across it (its border line of texels
            // equals the next line inside: a frame line or fill going on into the neighbour tile); where
            // a shape ends at the side (the round end of a title plate) it stays transparent as before
            auto at = [&](int x, int y) { return g_src[(size_t)(y + u->ry0) * w + x + u->rx0]; };
            auto alike = [](uint32_t a, uint32_t b) {
                for (int sh = 0; sh < 32; sh += 8) { int d = (int)((a >> sh) & 255) - (int)((b >> sh) & 255); if (d > 3 || d < -3) return false; }
                return true;
            };
            bool sl = rw > 1, sr = rw > 1, st = rh > 1, sb = rh > 1;
            for (int y = 0; y < rh && (sl || sr); y++) { sl = sl && alike(at(0, y), at(1, y)); sr = sr && alike(at(rw - 1, y), at(rw - 2, y)); }
            for (int x = 0; x < rw && (st || sb); x++) { st = st && alike(at(x, 0), at(x, 1)); sb = sb && alike(at(x, rh - 1), at(x, rh - 2)); }
            std::vector<uint32_t> pin((size_t)pw * ph), pout((size_t)pw * ph * g_f * g_f);
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++) {
                    int ix = x - P, iy = y - P;
                    bool out = (ix < 0 && !sl) || (ix >= rw && !sr) || (iy < 0 && !st) || (iy >= rh && !sb);
                    pin[(size_t)y * pw + x] = out ? 0u : at(std::min(std::max(ix, 0), rw - 1), std::min(std::max(iy, 0), rh - 1));
                }
            up_scale_image(pin.data(), pw, ph, pout.data(), g_f, u->alpha);
            const int PW = pw * g_f;
            for (int y = 0; y < rh * g_f; y++)
                memcpy(&g_dst[(size_t)(u->ry0 * g_f + y) * W + u->rx0 * g_f], &pout[(size_t)(y + P * g_f) * PW + P * g_f],
                       (size_t)rw * g_f * 4);
            static int nlog = 0;
            if (nlog < 40) { nlog++; log_text("[tileedges] %ux%u texture %p: rect %d,%d-%d,%d, sides continued L%d T%d R%d B%d", w, h, u->key, u->rx0, u->ry0, u->rx1, u->ry1, (int)sl, (int)st, (int)sr, (int)sb); }
        }
    } else {
#ifdef UP_HDTEXT
        // Order: put the background back under live glyphs, smooth-scale the background
        // (nearest for photographic backgrounds = the original look), then draw HD glyphs from
        // the original level 0.
        g_tmp = g_src;
        static int nostrip = -1;   // test switch D3D8PROXY_HDSTRIP=0: skip hdtext_strip (diagnosis)
        if (nostrip < 0) { const char *e = getenv("D3D8PROXY_HDSTRIP"); nostrip = e && *e == '0'; }
        if (!nostrip)
            hdtext_strip(u->key, g_tmp.data(), (int)w * 4, (int)w, (int)h);
        // photographic background under text: the offline cache is looked up by the
        // hash of the STRIPPED 1x (background without the live glyphs); dumped with UPDUMP
        bool bgcached = false;
        if (photo && g_f == 3) {
            unsigned long long hb = 1469598103934665603ULL;
            for (uint32_t p : g_tmp) { hb ^= p; hb *= 1099511628211ULL; }
            if (g_hddump)
                hd_dump(hb, w, h, g_tmp);
            if (g_hdcache && hd_load(hb, w, h, g_dst)) {
                bgcached = true;
                st_hdload++;
            }
        }
        if (bgcached) {
        } else if (photo && g_photosmooth) {
            up_smooth_image(g_tmp.data(), (int)w, (int)h, g_dst.data(), g_f);   // text on photographic art
        } else if (photo) {
            for (UINT y = 0; y < H; y++)
                for (UINT x = 0; x < W; x++)
                    g_dst[(size_t)y * W + x] = g_tmp[(size_t)(y / g_f) * w + x / g_f];
        } else
            up_scale_image(g_tmp.data(), (int)w, (int)h, g_dst.data(), g_f, u->alpha);
        int n = hdtext_overlay_text(u->key, g_src.data(), (int)w * 4, g_dst.data(), (int)W * 4, (int)w, (int)h);
        // test switch D3D8PROXY_TEXTDUMP=1: write 1x (as the game wrote it), stripped 1x and the
        // finished 3x of every text refresh to <game>\textdump\ (diagnosis)
        static int tdump = -1;
        if (tdump < 0) { const char *e = getenv("D3D8PROXY_TEXTDUMP"); tdump = e && *e == '1'; }
        if (tdump) {
            static unsigned seq = 0;
            std::string dir = g_dir + "textdump\\";
            CreateDirectoryA(dir.c_str(), NULL);
            char b[96];
            const std::vector<uint32_t> *imgs[3] = {&g_src, &g_tmp, &g_dst};
            const char *tag[3] = {"src", "strip", "out"};
            for (int k = 0; k < 3; k++) {
                UINT ww = k == 2 ? W : w, hh = k == 2 ? H : h;
                snprintf(b, sizeof b, "%05u_%p_%ux%u_%s.raw", seq, u->key, ww, hh, tag[k]);
                HANDLE f = CreateFileA((dir + b).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                if (f != INVALID_HANDLE_VALUE) {
                    DWORD wr;
                    WriteFile(f, imgs[k]->data(), (DWORD)((size_t)ww * hh * 4), &wr, NULL);
                    CloseHandle(f);
                }
            }
            seq++;
        }
        st_text++;
        if (n > 0) st_glyphs += (unsigned)n;
#endif
    }
    if (!u->shadow) {
        if (FAILED(dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &u->shadow, nullptr)) || !u->shadow) {
            u->shadow = nullptr;
            u->eligible = false;
            st_failed++;
            log_text("[upscale] CreateTexture %ux%u FAILED: texture left at its original size", W, H);
            return;
        }
        st_created++;
    }
    if (FAILED(u->shadow->LockRect(0, &lr, NULL, 0))) {
        st_failed++;
        return;
    }
    for (UINT y = 0; y < H; y++)
        memcpy((BYTE *)lr.pBits + (size_t)y * lr.Pitch, &g_dst[(size_t)y * W], W * 4);
    u->shadow->UnlockRect(0);
    u->refreshes++;
    double ms = now_ms() - t0;
    st_refresh++;
    st_ms += ms;
    st_px += (size_t)w * h;
    if (ms > st_ms_max) st_ms_max = ms;
    w_refresh++;
    w_ms += ms;
    if (ms > w_ms_max) w_ms_max = ms;
}

void write_stats() {
    DWORD now = GetTickCount();
    double secs = (now - w_t0) / 1000.0;
    if (secs <= 0) secs = 1;
    long fr = g_frames - w_f0;
    size_t alive = 0, bytes = 0;
    for (auto &kv : g_tex)
        if (kv.second->shadow) {
            alive++;
            bytes += (size_t)kv.second->w * kv.second->h * g_f * g_f * 4;
        }
    char line[512];
    snprintf(line, sizeof line,
             "t=%7.1fs fps=%6.1f refresh/s=%6.2f same-content/s=%6.2f scaler ms/s=%7.2f max ms=%6.2f | shadows=%zu "
             "(%.1f MB) on=%d\r\n",
             now / 1000.0, fr / secs, w_refresh / secs, w_same / secs, w_ms / secs, w_ms_max, alive,
             bytes / 1048576.0, (int)(g_on && g_active));
    g_stats_hist += line;
    w_refresh = w_same = 0;
    w_ms = w_ms_max = 0;
    w_t0 = now;
    w_f0 = g_frames;
    std::string o = "upscale statistics (D3D8PROXY_UPSTATS=1), factor " + std::to_string(g_f) + "\r\n";
    char tot[512];
    snprintf(tot, sizeof tot,
             "totals: refreshes=%llu (%.1f ms, avg %.3f ms, max %.2f ms, %.2f Mpx source), same-content skips=%llu, "
             "cache loads=%llu, cache dumps=%llu, painted without cache file (xBRZ)=%llu, refreshes with HD text=%llu (%llu glyphs), photographic refreshes (original kept)=%llu, shadows created=%llu, reused after re-creation=%llu, evicted=%llu, not eligible=%llu, failures=%llu, textures seen=%zu\r\n",
             st_refresh, st_ms, st_refresh ? st_ms / st_refresh : 0.0, st_ms_max, st_px / 1e6, st_same, st_hdload, st_hddump, st_painted_xbrz, st_text, st_glyphs, st_photo, st_created,
             st_reused, st_evicted, st_skipped, st_failed, g_tex.size());
    char dr[256];
    snprintf(dr, sizeof dr, "interface draws=%llu: with shadow=%llu, kept original (stretched/scaled/rotated)=%llu, "
             "layout unknown (shadow used)=%llu, shrunk copy (H19)=%llu, shrunk copies made=%llu\r\n\r\n", st_draw_ui, st_draw_shadow, st_draw_stretched, st_draw_unknown, st_draw_shrunk, st_shrink_made);
    o += tot;
    o += dr;
    o += g_stats_hist;
    std::string fn = g_dir + "upscale_stats.txt";
    HANDLE f = CreateFileA(fn.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr;
        WriteFile(f, o.data(), (DWORD)o.size(), &wr, NULL);
        CloseHandle(f);
    }
}

}   // namespace

void up_init(const char *ini, int factor) {
    // (g_uplk = the proxy's lock, initialised by ee_d3d_init)
    QueryPerformanceFrequency(&g_qpf);
    int v = (int)GetPrivateProfileIntA("uiscale", "Upscale", 1, ini);
    const char *e = getenv("D3D8PROXY_UPSCALE");
    if (e && *e)
        v = atoi(e);
    g_f = factor;
    g_on = v != 0 && factor >= 2 && factor <= 6;
    {
        // PixelFix needs the per-draw half-pixel class, which needs the vertex mirrors: keep them
        // (and only them) when the upscaler is off but PixelFix is on; else every interface draw
        // was shifted by 1 real px
        int pf = (int)GetPrivateProfileIntA("uiscale", "PixelFix", 1, ini);
        const char *pe = getenv("D3D8PROXY_PIXFIX");
        if (pe && *pe) pf = atoi(pe);
        g_mirrors = factor >= 2 && (g_on || pf != 0);
    }
    e = getenv("D3D8PROXY_UPFILE");
    g_toggle_file = e && *e == '1';
    g_hdcache = GetPrivateProfileIntA("uiscale", "HDCache", 1, ini) != 0;
    g_photosmooth = GetPrivateProfileIntA("uiscale", "PhotoSmooth", 0, ini) != 0;
    g_painted = GetPrivateProfileIntA("uiscale", "PaintedArt", 1, ini) != 0;
    e = getenv("D3D8PROXY_PAINTEDART");
    if (e && *e) g_painted = atoi(e) != 0;
    g_thick = GetPrivateProfileIntA("uiscale", "ThickLines", 1, ini) != 0;
    g_tileedges = GetPrivateProfileIntA("uiscale", "TileEdges", 1, ini) != 0;
    e = getenv("D3D8PROXY_TILEEDGES");
    if (e && *e) g_tileedges = atoi(e) != 0;
    e = getenv("D3D8PROXY_THICKLINES");
    if (e && *e) g_thick = atoi(e) != 0;
    e = getenv("D3D8PROXY_PHOTOSMOOTH");
    if (e && *e) g_photosmooth = atoi(e) != 0;
    e = getenv("D3D8PROXY_UPDUMP");
    g_hddump = e && *e == '1';
    e = getenv("D3D8PROXY_UPSTATS");
    g_stats = e && *e == '1';
    g_shrink = GetPrivateProfileIntA("uiscale", "SmoothShrink", 1, ini) != 0;
    e = getenv("D3D8PROXY_SMOOTHSHRINK");
    if (e && *e) g_shrink = atoi(e) != 0;
    g_comp_on = GetPrivateProfileIntA("EE", "DragText", 0, ini) != 0;   // off on the EE port (d3d9 render-target companions not ported)
    e = getenv("D3D8PROXY_DRAGTEXT");
    if (e && *e) g_comp_on = atoi(e) != 0;
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    char *s = strrchr(p, '\\');
    if (s) s[1] = 0;
    g_dir = p;
    w_t0 = GetTickCount();
    log_text("[upscale] %s (factor %d, Upscale=%d, PaintedArt=%d, HDCache=%d, ThickLines=%d, TileEdges=%d, SmoothShrink=%d)", g_on ? "ON" : "off", factor, v, (int)g_painted, (int)g_hdcache, (int)g_thick, (int)g_tileedges, (int)g_shrink);
#ifdef UP_HDTEXT
    // HD text: only with the upscaler on and factor 3 (the glyph pack is 3x).
    // uiscale.ini HDText=0 or D3D8PROXY_HDTEXT=0 leaves the text to xBRZ.
    // [EE] HDText / ListText win (the keys the ini documents; d3dhook.cpp reads them too),
    // [uiscale] only as the old fallback: before, [uiscale] (default 1) overrode [EE] HDText=0 / ListText=0 here
    int hd = (int)GetPrivateProfileIntA("EE", "HDText", (int)GetPrivateProfileIntA("uiscale", "HDText", 1, ini), ini);
    e = getenv("D3D8PROXY_HDTEXT");
    if (e && *e) hd = atoi(e);
    int lt = (int)GetPrivateProfileIntA("EE", "ListText", (int)GetPrivateProfileIntA("uiscale", "ListText", 1, ini), ini);   // default on; EE fix: [EE] first
    e = getenv("D3D8PROXY_LISTTEXT");
    if (e && *e) lt = atoi(e);
    g_dblfix = lt != 0;
    if (g_on && factor == 3 && hd) {
        // ListText=1: hdtext draws 1-px shadow/emboss copies 1 real px apart (not 3), so the list
        // font (main_hq_11) no longer needs to stay plain
        hdtext_set_builtin_plain(g_dblfix ? 0 : 1);
        hdtext_set_close_copies(g_dblfix ? 1 : 0);
        int r = hdtext_install(NULL);
        g_hd = r > 0;
        log_text("[upscale] hdtext_install -> %d (%s), ListText %d", r, g_hd ? "HD text on" : "HD text off", (int)g_dblfix);
    }
#endif
}

bool up_enabled() { return g_on; }
bool up_mirrors() { return g_mirrors; }

void up_set_texture(DWORD stage, IDirect3DBaseTexture9 *w, IDirect3DBaseTexture9 *r) {
    if (!g_on || stage >= 8)
        return;
    UpLock lk_;
    // While a shadow is bound in its place the real device holds no reference on the game's texture;
    // keep one ourselves, like D3D does for a set texture (else a texture the game
    // releases while "set" could be freed and later re-bound or locked here)
    if (r) r->AddRef();
    if (g_game_r[stage]) g_game_r[stage]->Release();
    g_game_w[stage] = w;
    g_game_r[stage] = r;
    g_game_known[stage] = true;
    g_bound[stage] = r;
    g_stage_looked[stage] = false;
}

namespace { bool g_view_unit = true; }   // ThickLines: the view matrix has unit scale (set in up_set_view)

void up_before_reset() {
    if (!g_on)
        return;
    UpLock lk_;
    g_view_unit = true;   // a Reset starts from the default view again
    for (int s = 0; s < 8; s++) {
        if (g_game_r[s]) g_game_r[s]->Release();
        g_game_r[s] = nullptr;
        g_game_w[s] = nullptr;
        g_game_known[s] = false;
        g_bound[s] = nullptr;
        g_stage_u[s] = nullptr;
        g_stage_looked[s] = false;
    }
}

IDirect3DBaseTexture9 *up_game_texture(DWORD stage, bool *known) {
    *known = g_on && stage < 8 && g_game_known[stage];
    return *known ? g_game_w[stage] : nullptr;
}

void up_set_projection(const D3DMATRIX *m) {
    g_ortho = m && m->_34 == 0.0f && m->_44 == 1.0f && m->_14 == 0.0f && m->_24 == 0.0f;
    if (m) { g_proj11 = m->_11; g_proj22 = m->_22; }
}

// ---- buffer mirrors ---------------------------------------------------------------------------
namespace {
struct Mirror {
    std::vector<BYTE> mem;
    UINT size = 0;
    int elem = 0;
    BYTE *realp = nullptr;
    UINT off = 0, len = 0;
};
std::unordered_map<void *, Mirror *> g_mir;
const UINT kMirrorMax = 262144;
float g_w11 = 1, g_w12 = 0, g_w21 = 0, g_w22 = 1, g_w41 = 0, g_w42 = 0;
float g_v11 = 1, g_v22 = 1, g_v41 = 0, g_v42 = 0, g_v12 = 0, g_v21 = 0;   // view matrix (D3D8PROXY_RTLOG diagnosis only)


Mirror *mirror_of(void *w) {
    auto it = g_mir.find(w);
    return it == g_mir.end() ? nullptr : it->second;
}

// Position (game pixels, world 2x2 + translation applied) of the first vertex of a draw.
bool first_vertex(const UpDraw &d, float *x, float *y) {
    if (!d.fvf_valid || (d.fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || !d.stride)
        return false;
    const BYTE *vb = d.up_vtx;
    UINT vbsize = 0xffffffffu;
    if (!vb) {
        Mirror *m = mirror_of(d.vb);
        if (!m || m->elem != 0) return false;
        vb = m->mem.data();
        vbsize = m->size;
    }
    UINT first = d.start;
    if (d.indexed) {
        UINT i = d.start;
        if (d.up_idx)
            first = d.idx32 ? ((const uint32_t *)d.up_idx)[i] : ((const uint16_t *)d.up_idx)[i];
        else {
            Mirror *mi = mirror_of(d.ib);
            if (!mi || mi->elem == 0 || (unsigned long long)(i + 1) * mi->elem > mi->size) return false;
            first = mi->elem == 4 ? ((const uint32_t *)mi->mem.data())[i] : ((const uint16_t *)mi->mem.data())[i];
        }
        first += d.basevtx;
    }
    if ((unsigned long long)first * d.stride + 12 > vbsize) return false;
    const float *p = (const float *)(vb + (size_t)first * d.stride);
    *x = p[0] * g_w11 + p[1] * g_w21 + g_w41;
    *y = p[0] * g_w12 + p[1] * g_w22 + g_w42;
    return true;
}

// true = every checked triangle maps texels to game pixels 1:1 (or the layout is unknown)
bool one_to_one(const UpDraw &d, UINT tw, UINT th, float *box = nullptr) {
    if (!d.fvf_valid || (d.fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ) { st_draw_unknown++; return true; }
    if (((d.fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) == 0 || (d.fvf & 0x30000))
        { st_draw_unknown++; return true; }   // no texture coordinates / set 0 not float2
    UINT uvo = 12;
    if (d.fvf & D3DFVF_NORMAL) uvo += 12;
    if (d.fvf & D3DFVF_PSIZE) uvo += 4;
    if (d.fvf & D3DFVF_DIFFUSE) uvo += 4;
    if (d.fvf & D3DFVF_SPECULAR) uvo += 4;
    const BYTE *vb;
    UINT vbsize, stride = d.stride;
    if (d.up_vtx) { vb = d.up_vtx; vbsize = 0xffffffffu; }
    else {
        Mirror *m = mirror_of(d.vb);
        if (!m || m->elem != 0) { st_draw_unknown++; return true; }
        vb = m->mem.data(); vbsize = m->size;
    }
    if (stride < uvo + 8) { st_draw_unknown++; return true; }
    const BYTE *ib = nullptr;
    UINT ibcount = 0xffffffffu;
    bool i32 = false;
    if (d.indexed) {
        if (d.up_idx) { ib = (const BYTE *)d.up_idx; i32 = d.idx32; }
        else {
            Mirror *m = mirror_of(d.ib);
            if (!m || m->elem == 0) { st_draw_unknown++; return true; }
            ib = m->mem.data(); i32 = m->elem == 4; ibcount = m->size / m->elem;
        }
    }
    auto vidx = [&](UINT k, UINT *out) -> bool {   // k = k-th index/vertex of the draw
        UINT i = d.start + k;
        if (d.indexed) {
            if (i >= ibcount) return false;
            UINT v = i32 ? ((const uint32_t *)ib)[i] : ((const uint16_t *)ib)[i];
            *out = v + d.basevtx;
        } else
            *out = i;
        return (unsigned long long)*out * stride + uvo + 8 <= vbsize;
    };
    UINT n = d.prims < 128 ? d.prims : 128;
    for (UINT t = 0; t < n; t++) {
        UINT k[3];
        switch (d.pt) {
        case D3DPT_TRIANGLELIST: k[0] = 3 * t; k[1] = 3 * t + 1; k[2] = 3 * t + 2; break;
        case D3DPT_TRIANGLESTRIP: k[0] = t; k[1] = t + 1; k[2] = t + 2; break;
        case D3DPT_TRIANGLEFAN: k[0] = 0; k[1] = t + 1; k[2] = t + 2; break;
        default: return true;   // points/lines
        }
        float px[3], py[3], tu[3], tv[3];
        for (int j = 0; j < 3; j++) {
            UINT v;
            if (!vidx(k[j], &v)) { st_draw_unknown++; return true; }
            const float *p = (const float *)(vb + (size_t)v * stride);
            const float *uv = (const float *)(vb + (size_t)v * stride + uvo);
            px[j] = p[0] * g_w11 + p[1] * g_w21;
            py[j] = p[0] * g_w12 + p[1] * g_w22;
            tu[j] = uv[0] * tw;
            tv[j] = uv[1] * th;
            if (box) {   // texel bounding box of the draw (min u, min v, max u, max v)
                box[0] = std::min(box[0], tu[j]); box[1] = std::min(box[1], tv[j]);
                box[2] = std::max(box[2], tu[j]); box[3] = std::max(box[3], tv[j]);
            }
        }
        float a1 = px[1] - px[0], a2 = px[2] - px[0], b1 = py[1] - py[0], b2 = py[2] - py[0];
        float u1 = tu[1] - tu[0], u2 = tu[2] - tu[0], v1 = tv[1] - tv[0], v2 = tv[2] - tv[0];
        float detP = a1 * b2 - a2 * b1, detT = u1 * v2 - u2 * v1;
        if (std::fabs(detP) < 1e-3f) continue;          // degenerate triangle: draws nothing
        if (std::fabs(detT) < 1e-3f) return false;      // one texel (or a line of texels) stretched
        float j11 = (a1 * v2 - a2 * v1) / detT, j12 = (a2 * u1 - a1 * u2) / detT;
        float j21 = (b1 * v2 - b2 * v1) / detT, j22 = (b2 * u1 - b1 * u2) / detT;
        if (std::fabs(j12) > 0.05f || std::fabs(j21) > 0.05f || std::fabs(std::fabs(j11) - 1) > 0.05f ||
            std::fabs(std::fabs(j22) - 1) > 0.05f)
            return false;
    }
    return true;
}

// SmoothShrink: the game draws some interface art smaller than its own size (the 2048x512 menu
// logo at about 1200 real px) with POINT sampling: at 4K every real pixel picks one texel, the rest
// are skipped -> stair-steps. Such a draw gets a copy of the texture shrunk to the exact real-pixel
// size it is drawn at (area average over the covered texels, alpha-weighted colour = no dark or
// light fringe at transparent edges), so point sampling then reads it 1:1.
// shrink_scale: true = every checked triangle maps texels axis-aligned with ONE scale per axis, and
// that scale is below 1 real px per texel on at least one axis and not above 1 on the other.
UINT g_shr_vpw = 0, g_shr_vph = 0; int g_shr_why = 0; float g_shr_jx = 0, g_shr_jy = 0, g_shr_j12 = 0, g_shr_j21 = 0;   // test switch D3D8PROXY_SHRINKDIAG
bool shrink_scale(IDirect3DDevice9 *dev, const UpDraw &d, UINT tw, UINT th, float *rsx, float *rsy) {
    if (!g_sc.gw || !g_sc.gh || std::fabs(g_v12) > 1e-4f || std::fabs(g_v21) > 1e-4f) return (g_shr_why = 15, false);
    if (!d.fvf_valid || (d.fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ) return (g_shr_why = 1, false);
    if (((d.fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) == 0 || (d.fvf & 0x30000)) return (g_shr_why = 2, false);
    if (d.pt != D3DPT_TRIANGLELIST && d.pt != D3DPT_TRIANGLESTRIP && d.pt != D3DPT_TRIANGLEFAN) return (g_shr_why = 3, false);
    UINT uvo = 12;
    if (d.fvf & D3DFVF_NORMAL) uvo += 12;
    if (d.fvf & D3DFVF_PSIZE) uvo += 4;
    if (d.fvf & D3DFVF_DIFFUSE) uvo += 4;
    if (d.fvf & D3DFVF_SPECULAR) uvo += 4;
    const BYTE *vb;
    UINT vbsize, stride = d.stride;
    if (d.up_vtx) { vb = d.up_vtx; vbsize = 0xffffffffu; }
    else {
        Mirror *m = mirror_of(d.vb);
        if (!m || m->elem != 0) return (g_shr_why = 4, false);
        vb = m->mem.data(); vbsize = m->size;
    }
    if (stride < uvo + 8) return (g_shr_why = 5, false);
    const BYTE *ib = nullptr;
    UINT ibcount = 0xffffffffu;
    bool i32 = false;
    if (d.indexed) {
        if (d.up_idx) { ib = (const BYTE *)d.up_idx; i32 = d.idx32; }
        else {
            Mirror *m = mirror_of(d.ib);
            if (!m || m->elem == 0) return (g_shr_why = 6, false);
            ib = m->mem.data(); i32 = m->elem == 4; ibcount = m->size / m->elem;
        }
    }
    auto vidx = [&](UINT k, UINT *out) -> bool {
        UINT i = d.start + k;
        if (d.indexed) {
            if (i >= ibcount) return (g_shr_why = 7, false);
            UINT v = i32 ? ((const uint32_t *)ib)[i] : ((const uint16_t *)ib)[i];
            *out = v + d.basevtx;
        } else
            *out = i;
        return (unsigned long long)*out * stride + uvo + 8 <= vbsize;
    };
    // world units -> real px through view x projection x the REAL viewport (the menu logo quad is
    // 896 units wide but drawn into a smaller viewport: about 0.86 real px per texel on screen)
    D3DVIEWPORT9 vp = {};
    if (FAILED(dev->GetViewport(&vp)) || !vp.Width || !vp.Height) return (g_shr_why = 16, false);
    const float kx = std::fabs(g_proj11 * g_v11) * vp.Width * 0.5f, ky = std::fabs(g_proj22 * g_v22) * vp.Height * 0.5f;
    g_shr_vpw = vp.Width; g_shr_vph = vp.Height;
    UINT n = d.prims < 128 ? d.prims : 128;
    float sx = 0, sy = 0;
    bool any = false;
    for (UINT t = 0; t < n; t++) {
        UINT k[3];
        switch (d.pt) {
        case D3DPT_TRIANGLELIST: k[0] = 3 * t; k[1] = 3 * t + 1; k[2] = 3 * t + 2; break;
        case D3DPT_TRIANGLESTRIP: k[0] = t; k[1] = t + 1; k[2] = t + 2; break;
        default: k[0] = 0; k[1] = t + 1; k[2] = t + 2; break;
        }
        float px[3], py[3], tu[3], tv[3];
        for (int j = 0; j < 3; j++) {
            UINT v;
            if (!vidx(k[j], &v)) return (g_shr_why = 8, false);
            const float *p = (const float *)(vb + (size_t)v * stride);
            const float *uv = (const float *)(vb + (size_t)v * stride + uvo);
            px[j] = p[0] * g_w11 + p[1] * g_w21;
            py[j] = p[0] * g_w12 + p[1] * g_w22;
            tu[j] = uv[0] * tw;
            tv[j] = uv[1] * th;
        }
        float a1 = px[1] - px[0], a2 = px[2] - px[0], b1 = py[1] - py[0], b2 = py[2] - py[0];
        float u1 = tu[1] - tu[0], u2 = tu[2] - tu[0], v1 = tv[1] - tv[0], v2 = tv[2] - tv[0];
        float detP = a1 * b2 - a2 * b1, detT = u1 * v2 - u2 * v1;
        if (std::fabs(detP) < 1e-3f) continue;
        if (std::fabs(detT) < 1e-3f) return (g_shr_why = 9, false);
        float j11 = (a1 * v2 - a2 * v1) / detT, j12 = (a2 * u1 - a1 * u2) / detT;
        float j21 = (b1 * v2 - b2 * v1) / detT, j22 = (b2 * u1 - b1 * u2) / detT;
        float ax = std::fabs(j11) * kx, ay = std::fabs(j22) * ky;   // real px per texel
        if (ax < 1e-3f || ay < 1e-3f) return (g_shr_why = 10, false);
        if (std::fabs(j12) * g_f > 0.01f * ax || std::fabs(j21) * g_f > 0.01f * ay) return (g_shr_why = 11, false);   // rotated/sheared
        g_shr_jx = ax; g_shr_jy = ay; g_shr_j12 = j12; g_shr_j21 = j21;
        if (!any) { sx = ax; sy = ay; any = true; }
        else if (std::fabs(ax - sx) > 0.01f * sx || std::fabs(ay - sy) > 0.01f * sy) return (g_shr_why = 12, false);   // mixed scales
    }
    if (!any) return (g_shr_why = 13, false);
    // not the plain 1:1 case (3 real px per texel: the 3x shadows handle it), sane range only
    if ((std::fabs(sx - g_f) < 0.02f * g_f && std::fabs(sy - g_f) < 0.02f * g_f) || sx < 1.0f / 16 || sy < 1.0f / 16 || sx > 8 || sy > 8)
        return (g_shr_why = 14, false);
    *rsx = sx; *rsy = sy;
    return true;
}

// Area-average shrink of src (w x h BGRA) to W x H; colour weighted by alpha (straight alpha out).
void shrink_image(const uint32_t *src, int w, int h, uint32_t *dst, int W, int H) {
    const double fx = (double)w / W, fy = (double)h / H;
    std::vector<double> acc((size_t)W * 4);
    for (int Y = 0; Y < H; Y++) {
        std::fill(acc.begin(), acc.end(), 0.0);
        double y0 = Y * fy, y1 = y0 + fy, wsum_y = 0;
        for (int y = (int)y0; y < h && y < y1; y++) {
            double wy = std::min<double>(y + 1, y1) - std::max<double>(y, y0);
            if (wy <= 0) continue;
            wsum_y += wy;
            const uint32_t *r = src + (size_t)y * w;
            for (int X = 0; X < W; X++) {
                double x0 = X * fx, x1 = x0 + fx;
                double a = 0, cr = 0, cg = 0, cb = 0;
                for (int x = (int)x0; x < w && x < x1; x++) {
                    double wx = std::min<double>(x + 1, x1) - std::max<double>(x, x0);
                    if (wx <= 0) continue;
                    uint32_t p = r[x];
                    double pa = (p >> 24) * wx;
                    a += pa;
                    cr += pa * ((p >> 16) & 255); cg += pa * ((p >> 8) & 255); cb += pa * (p & 255);
                }
                double *o = &acc[(size_t)X * 4];
                o[0] += a * wy; o[1] += cr * wy; o[2] += cg * wy; o[3] += cb * wy;
            }
        }
        for (int X = 0; X < W; X++) {
            const double *o = &acc[(size_t)X * 4];
            double area = fx * wsum_y;
            uint32_t A = (uint32_t)std::lround(std::min(255.0, o[0] / (area > 0 ? area : 1)));
            uint32_t R = 0, G = 0, B = 0;
            if (o[0] > 0) {
                R = (uint32_t)std::lround(std::min(255.0, o[1] / o[0]));
                G = (uint32_t)std::lround(std::min(255.0, o[2] / o[0]));
                B = (uint32_t)std::lround(std::min(255.0, o[3] / o[0]));
            }
            dst[(size_t)Y * W + X] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
}

// Lanczos-3 resample (enlarging axis) / area average (shrinking axis), premultiplied alpha, clamped edges.
void resample_axis(const float *src, int n, int m, int stride_in, int count, float *dst, int stride_out, int pitch_in, int pitch_out) {
    // src: count lines of n samples (4 floats each, step stride_in floats); dst: count lines of m samples
    const double sc = (double)m / n;
    const bool up = sc >= 1.0;
    const double support = up ? 3.0 : 0.5 / sc;   // Lanczos-3 lobes / half box width in source px
    std::vector<int> first(m);
    std::vector<std::vector<float>> wts(m);
    for (int X = 0; X < m; X++) {
        double c = (X + 0.5) / sc - 0.5;
        int a = (int)std::floor(c - support) + (up ? 0 : 0), b = (int)std::ceil(c + support);
        first[X] = a;
        double sum = 0;
        for (int x = a; x <= b; x++) {
            double t = x - c, wv;
            if (up) {
                if (std::fabs(t) >= 3.0) wv = 0;
                else if (std::fabs(t) < 1e-9) wv = 1;
                else { double pt = 3.14159265358979 * t; wv = 3.0 * std::sin(pt) * std::sin(pt / 3.0) / (pt * pt); }
            } else {
                // box [c-support, c+support] overlap with texel [x-0.5, x+0.5]
                wv = std::min(c + support, x + 0.5) - std::max(c - support, x - 0.5);
                if (wv < 0) wv = 0;
            }
            wts[X].push_back((float)wv);
            sum += wv;
        }
        for (auto &w : wts[X]) w = (float)(w / (sum != 0 ? sum : 1));
    }
    for (int l = 0; l < count; l++) {
        const float *in = src + (size_t)l * pitch_in;
        float *out = dst + (size_t)l * pitch_out;
        for (int X = 0; X < m; X++) {
            float acc[4] = {0, 0, 0, 0};
            const auto &w = wts[X];
            for (size_t k = 0; k < w.size(); k++) {
                int x = first[X] + (int)k;
                x = x < 0 ? 0 : x >= n ? n - 1 : x;
                const float *p = in + (size_t)x * stride_in;
                for (int c = 0; c < 4; c++) acc[c] += w[k] * p[c];
            }
            float *o = out + (size_t)X * stride_out;
            for (int c = 0; c < 4; c++) o[c] = acc[c];
        }
    }
}
void resample_image(const uint32_t *src, int w, int h, uint32_t *dst, int W, int H) {
    std::vector<float> a((size_t)w * h * 4), b((size_t)W * h * 4), c((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t p = src[i];
        float al = (p >> 24) / 255.0f;
        a[i * 4 + 0] = al;
        a[i * 4 + 1] = ((p >> 16) & 255) * al; a[i * 4 + 2] = ((p >> 8) & 255) * al; a[i * 4 + 3] = (p & 255) * al;
    }
    resample_axis(a.data(), w, W, 4, h, b.data(), 4, w * 4, W * 4);        // rows
    resample_axis(b.data(), h, H, W * 4, W, c.data(), W * 4, 4, 4);        // columns (line = one column)
    for (size_t i = 0; i < (size_t)W * H; i++) {
        float al = std::min(1.0f, std::max(0.0f, c[i * 4]));
        uint32_t A = (uint32_t)std::lround(al * 255), R = 0, G = 0, B = 0;
        if (A > 0) {
            float inv = 1.0f / al;
            R = (uint32_t)std::lround(std::min(255.0f, std::max(0.0f, c[i * 4 + 1] * inv)));
            G = (uint32_t)std::lround(std::min(255.0f, std::max(0.0f, c[i * 4 + 2] * inv)));
            B = (uint32_t)std::lround(std::min(255.0f, std::max(0.0f, c[i * 4 + 3] * inv)));
        }
        dst[i] = (A << 24) | (R << 16) | (G << 8) | B;
    }
}

// The shrunk copy of u at W x H (made or refreshed when the original changed); nullptr = keep the original.
IDirect3DTexture9 *shrink_get_(IDirect3DDevice9 *dev, UpTex *u, UINT W, UINT H) {
    if (!u->real) return nullptr;
    if (u->mini_dirty) {
        D3DLOCKED_RECT lr;
        g_src.resize((size_t)u->w * u->h);   // before LockRect, so a failed allocation never leaves it locked
        if (FAILED(u->real->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return nullptr;
        for (UINT y = 0; y < u->h; y++)
            memcpy(&g_src[(size_t)y * u->w], (const BYTE *)lr.pBits + (size_t)y * lr.Pitch, u->w * 4);
        u->real->UnlockRect(0);
        if (!u->alpha)
            for (auto &p : g_src) p |= 0xff000000u;
        unsigned long long hs = 1469598103934665603ULL;
        for (uint32_t p : g_src) { hs ^= p; hs *= 1099511628211ULL; }
        u->mini_hash = hs;
        u->mini_dirty = false;
    }
    UpTex::Mini *m = nullptr;
    for (auto &k : u->minis)
        if (k.W == W && k.H == H) { m = &k; break; }
    if (m && m->t && m->hash == u->mini_hash) return m->t;
    // enlarged draws only with a 4x cache file (a resampled 1x picture is hardly better than the game's
    // own filtering); shrunk draws always
    bool enlarge = W > u->w || H > u->h;
    if (u->x4_hash != u->mini_hash) { u->x4_hash = u->mini_hash; u->x4_none = false; }
    if (enlarge && (u->x4_none || !g_hdcache)) return nullptr;
    bool big = (unsigned long long)u->w * u->h > 1024ull * 1024;   // copies of pictures above 1024x1024
    if (big && (u->x4_none || !g_hdcache)) return nullptr;            // texels only from a 4x file (scratch memory)
    if (!m) {
        // drawn at many sizes: leave it. Reusing the least recently used entry would not work:
        // a picture drawn at 5+ sizes per frame, or one whose size
        // changes every frame, would then be resampled on almost every draw.
        if (u->minis.size() >= 4) return nullptr;
        u->minis.push_back(UpTex::Mini());
        m = &u->minis.back();
        m->W = W; m->H = H;
    }
    double t0 = now_ms();
    // source pixels: g_src holds them only right after a dirty read; read again otherwise
    {
        D3DLOCKED_RECT lr;
        g_src.resize((size_t)u->w * u->h);   // before LockRect (see above)
        if (FAILED(u->real->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return nullptr;
        for (UINT y = 0; y < u->h; y++)
            memcpy(&g_src[(size_t)y * u->w], (const BYTE *)lr.pBits + (size_t)y * lr.Pitch, u->w * 4);
        u->real->UnlockRect(0);
        if (!u->alpha)
            for (auto &p : g_src) p |= 0xff000000u;
    }
    // optional finer source: uiscale_hd\<hash>_<w>x<h>_x4.raw = the same picture at 4x (BGRA), e.g. the
    // menu logo 512x128 made from the game's own 2048x512 copy of that artwork (registered offline)
    bool x4 = false;
    if (g_hdcache) {
        char b[64];
        snprintf(b, sizeof b, "%016llx_%ux%u_x4.raw", u->mini_hash, u->w, u->h);
        HANDLE f = CreateFileA((g_dir + "uiscale_hd\\" + b).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            size_t n = (size_t)u->w * 4 * u->h * 4;
            try { g_tmp.resize(n); } catch (...) { CloseHandle(f); throw; }   // no handle leak on a failed allocation
            DWORD got = 0;
            x4 = ReadFile(f, g_tmp.data(), (DWORD)(n * 4), &got, NULL) && got == n * 4;
            CloseHandle(f);
            if (!x4) log_text("[shrink] %s has the wrong size, ignored", b);
        }
        static std::set<unsigned long long> x4_logged;   // log each content key once
        if (x4_logged.insert(u->mini_hash ^ ((unsigned long long)u->w << 48) ^ ((unsigned long long)u->h << 32)).second)
            log_text("[shrink] key %016llx %ux%u: 4x file %s (uiscale_hd\\%s)", u->mini_hash, u->w, u->h,
                     x4 ? "found" : f == INVALID_HANDLE_VALUE ? "not found" : "found but wrong size", b);
    }
    if (!x4) u->x4_none = true;
    if (!x4 && enlarge) return nullptr;   // m stays without a texture: the original is drawn
    if (!x4 && big) return nullptr;   // (see above; the exe has a 2 GB address space)
    g_dst.resize((size_t)W * H);      // after the checks above
    if (x4)
        resample_image(g_tmp.data(), (int)u->w * 4, (int)u->h * 4, g_dst.data(), (int)W, (int)H);
    else
        resample_image(g_src.data(), (int)u->w, (int)u->h, g_dst.data(), (int)W, (int)H);
    if (!m->t && (FAILED(dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m->t, nullptr)) || !m->t)) {
        m->t = nullptr;
        log_text("[shrink] CreateTexture %ux%u FAILED: original kept", W, H);
        return nullptr;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(m->t->LockRect(0, &lr, NULL, 0))) return nullptr;
    for (UINT y = 0; y < H; y++)
        memcpy((BYTE *)lr.pBits + (size_t)y * lr.Pitch, &g_dst[(size_t)y * W], W * 4);
    m->t->UnlockRect(0);
    m->hash = u->mini_hash;
    st_shrink_made++;
    log_text("[shrink] tex %p %ux%u (hash %016llx) -> copy %ux%u from %s made in %.1f ms", u->key, u->w, u->h, u->mini_hash, W, H,
             x4 ? "the 4x cache file" : "the original", now_ms() - t0);
    return m->t;
}
// A failed allocation (scratch vectors, the 4x file buffer) never ends the game: the original is
// drawn, this texture gets no further copies and the scratch memory is given back
IDirect3DTexture9 *shrink_get(IDirect3DDevice9 *dev, UpTex *u, UINT W, UINT H) {
    try {
        return shrink_get_(dev, u, W, H);
    } catch (const std::exception &e) {
        u->mini_ok = false;
        std::vector<uint32_t>().swap(g_src);
        std::vector<uint32_t>().swap(g_dst);
        std::vector<uint32_t>().swap(g_tmp);
        log_text("[shrink] tex %p %ux%u -> %ux%u: %s, original kept for good", u->key, u->w, u->h, W, H, e.what());
        return nullptr;
    }
}

void release_minis(UpTex *u) {
    for (auto &k : u->minis) {
        if (!k.t) continue;
        bool bound = false;
        for (int s = 0; s < 8; s++) bound |= g_bound[s] == k.t;
        if (bound) g_comp_orphans.push_back(k.t);   // released at the next Reset with the companions' orphans
        else k.t->Release();
        k.t = nullptr;
    }
    u->minis.clear();
}
}   // namespace

bool up_buf_lock(void *w, int elem, UINT size, UINT off, UINT len, BYTE **data, BYTE *real_ptr) {
    if (!g_mirrors || size > kMirrorMax || !data || !real_ptr || off >= size)
        return false;
    UpLock lk_;
    Mirror *m = mirror_of(w);
    if (!m) {
        m = new Mirror;
        g_mir[w] = m;
    }
    if (m->size != size || m->elem != elem) {
        m->mem.assign(size, 0);
        m->size = size;
        m->elem = elem;
    }
    m->realp = real_ptr;
    m->off = off;
    m->len = len && off + len <= size ? len : size - off;
    *data = m->mem.data() + off;
    return true;
}

void up_buf_unlock(void *w) {
    if (!g_mirrors)
        return;
    UpLock lk_;
    Mirror *m = mirror_of(w);
    if (!m || !m->realp)
        return;
    memcpy(m->realp, m->mem.data() + m->off, m->len);
    m->realp = nullptr;
}

void up_buf_released(void *w) {
    if (!g_mirrors)
        return;
    UpLock lk_;
    auto it = g_mir.find(w);
    if (it == g_mir.end())
        return;
    delete it->second;
    g_mir.erase(it);
}

void up_set_world(const D3DMATRIX *m) {
    if (!m) return;
    g_w11 = m->_11; g_w12 = m->_12; g_w21 = m->_21; g_w22 = m->_22; g_w41 = m->_41; g_w42 = m->_42;
}

// Pixel-alignment class of the current interface draw (read by the scale layer): the game puts
// most interface geometry at -0.5 game px (Direct3D 8 texel alignment), some flat rectangles at whole
// pixels. 1 = first vertex at x.5 (shift by (F-1)/2 real px), 0 = whole pixel (no shift), -1 = unknown.
int g_halfx = -1, g_halfy = -1;
static int half_class(float v) {
    float f = v - std::floor(v);
    return f >= 0.25f && f < 0.75f ? 1 : 0;
}
void up_half_pixel(int *hx, int *hy) { *hx = g_halfx; *hy = g_halfy; }


static DWORD g_rs_src, g_rs_dst, g_rs_ref;   // blend states replaced for one text draw
static bool g_rs_saved = false;
void up_after_draw(IDirect3DDevice9 *dev);
void up_before_draw2(IDirect3DDevice9 *dev, const UpDraw &d) {
    if (!g_on) {
        if (!g_mirrors)
            return;
        // Upscale=0, PixelFix=1: only the half-pixel class of interface draws (read by the scale layer)
        UpLock lk_;
        g_halfx = g_halfy = -1;
        float x, y;
        if (g_ortho && g_sc.rt_is_bb && g_sc.active && first_vertex(d, &x, &y)) {
            g_halfx = half_class(x);
            g_halfy = half_class(y);
        }
        return;
    }
    UpLock lk_;
    g_halfx = g_halfy = -1;
    if (g_ortho && (g_sc.rt_is_bb || g_rt_comp) && g_sc.active) {
        float x, y;
        bool fv = first_vertex(d, &x, &y);
        if (fv) {
            g_halfx = half_class(x);
            g_halfy = half_class(y);
        }
#ifdef UP_HDTEXT
        {
            static int tlog = -1;   // test switch D3D8PROXY_TEXTDRAWLOG=N: first N interface draws of text textures
            if (tlog < 0) { const char *e = getenv("D3D8PROXY_TEXTDRAWLOG"); tlog = e ? atoi(e) : 0; }
            void *k = g_game_w[0];
            static int bx0 = -1, by0, bx1, by1;   // D3D8PROXY_DRAWBOX=x0,y0,x1,y1: log every draw starting in it
            if (bx0 == -1) { const char *e = getenv("D3D8PROXY_DRAWBOX"); bx0 = -2; if (e) sscanf(e, "%d,%d,%d,%d", &bx0, &by0, &bx1, &by1); }
            bool inbox = bx0 >= 0 && fv && x >= bx0 && x <= bx1 && y >= by0 && y <= by1;
            if (tlog > 0 && k && (inbox || (bx0 < 0 && g_hd && hdtext_has_text(k)))) {
                tlog--;
                log_text("[textdraw] f%ld text %d tex %p stage1 %p fvf 0x%lx vb %p up %d first vertex %s %g,%g prims %u pt %d", (long)g_frames, (int)(k && g_hd && hdtext_has_text(k)), k,
                         (void *)g_game_w[1], (unsigned long)d.fvf, d.vb, d.up_vtx != nullptr, fv ? "ok" : "UNKNOWN", fv ? x : 0.f,
                         fv ? y : 0.f, d.prims, (int)d.pt);
            }
        }
#endif
    }
    {
        // test switch D3D8PROXY_RTLOG=N: the first N draws from user memory into a render target
        // that is not the back buffer, and the first N interface draws of a render-target texture
        static int rtlog = -1;
        if (rtlog < 0) { const char *e = getenv("D3D8PROXY_RTLOG"); rtlog = e ? atoi(e) : 0; }
        if (rtlog > 0 && g_sc.active) {
            UpTex *t0 = g_game_w[0] ? info_for(g_game_w[0], g_game_r[0]) : nullptr;
            bool into_rt = !g_sc.rt_is_bb && d.up_vtx;
            bool of_rt = g_sc.rt_is_bb && g_ortho && t0 && t0->rt;
            static long frame_until = -1;   // log every draw of the frame with the first RT-texture draw + 1 more
            if (of_rt && frame_until < 0) frame_until = (long)g_frames + 2;
            bool inframe = frame_until > 0 && (long)g_frames < frame_until;
            if (inframe && !into_rt && !of_rt) {
                float fx = 0, fy = 0;
                bool fv = first_vertex(d, &fx, &fy);
                log_text("[rtlog]  frame f%ld draw: rt_is_bb %d ortho %d tex %p %ux%u rt %d elig %d text %d photo %d | world t %g,%g view %g %g t %g,%g | first %s %g,%g | pt %d prims %u up %d idx %d fvf 0x%lx 1:1 %d",
                         (long)g_frames, (int)g_sc.rt_is_bb, (int)g_ortho, (void *)g_game_w[0], t0 ? t0->w : 0, t0 ? t0->h : 0,
                         t0 ? (int)t0->rt : -1, t0 ? (int)t0->eligible : -1,
#ifdef UP_HDTEXT
                         (int)(g_hd && g_game_w[0] && hdtext_has_text(g_game_w[0])),
#else
                         0,
#endif
                         t0 ? (int)t0->photo : -1, g_w41, g_w42, g_v11, g_v22, g_v41, g_v42, fv ? "at" : "unknown", fx, fy, (int)d.pt, d.prims,
                         (int)(d.up_vtx != nullptr), (int)d.indexed, (unsigned long)d.fvf, t0 ? (int)one_to_one(d, t0->w, t0->h) : -1);
            }
            if (into_rt || of_rt) {
                rtlog--;
                D3DVIEWPORT9 vp = {};
                dev->GetViewport(&vp);
                DWORD sb = 0, db = 0, ab = 0, mag = 0, cop = 0, ca1 = 0, ca2 = 0, aop = 0;
                dev->GetRenderState(D3DRS_SRCBLEND, &sb); dev->GetRenderState(D3DRS_DESTBLEND, &db);
                dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
                dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &mag); dev->GetTextureStageState(0, D3DTSS_COLOROP, &cop);
                dev->GetTextureStageState(0, D3DTSS_COLORARG1, &ca1); dev->GetTextureStageState(0, D3DTSS_COLORARG2, &ca2);
                dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &aop);
                log_text("[rtlog] f%ld %s: tex %p %ux%u rt %d eligible %d text %d | ortho %d proj %g %g %g %g world %g %g %g %g %g %g view %g %g t %g,%g | vp %lu,%lu %lux%lu | blend %lu %lu,%lu mag %lu cop %lu %lu %lu aop %lu | fvf 0x%lx stride %u pt %d prims %u up %d 1:1 %d",
                         (long)g_frames, into_rt ? "UP draw into RT" : "RT texture drawn on back buffer", (void *)g_game_w[0],
                         t0 ? t0->w : 0, t0 ? t0->h : 0, t0 ? (int)t0->rt : -1, t0 ? (int)t0->eligible : -1,
#ifdef UP_HDTEXT
                         (int)(g_hd && g_game_w[0] && hdtext_has_text(g_game_w[0])),
#else
                         0,
#endif
                         (int)g_ortho, g_proj11, g_proj22, 0.f, 0.f, g_w11, g_w12, g_w21, g_w22, g_w41, g_w42, g_v11, g_v22, g_v41, g_v42,
                         (unsigned long)vp.X, (unsigned long)vp.Y, (unsigned long)vp.Width, (unsigned long)vp.Height,
                         (unsigned long)ab, (unsigned long)sb, (unsigned long)db, (unsigned long)mag, (unsigned long)cop,
                         (unsigned long)ca1, (unsigned long)ca2, (unsigned long)aop, (unsigned long)d.fvf, d.stride, (int)d.pt, d.prims,
                         (int)(d.up_vtx != nullptr), t0 ? (int)one_to_one(d, t0->w, t0->h) : -1);
                const BYTE *vb = d.up_vtx;
                if (!vb) {
                    Mirror *m = mirror_of(d.vb);
                    UINT first = d.start;
                    if (d.indexed) {
                        Mirror *mi = mirror_of(d.ib);
                        first = (mi && mi->elem == 2 && (size_t)(d.start + 1) * 2 <= mi->size) ? ((const uint16_t *)mi->mem.data())[d.start] + d.basevtx : 0xffffffffu;
                    }
                    if (m && m->elem == 0 && first != 0xffffffffu && (size_t)(first + 4) * d.stride <= m->size) vb = m->mem.data() + (size_t)first * d.stride;
                }
                if (vb && d.fvf_valid && d.stride >= 20) {
                    UINT uvo = 12 + ((d.fvf & D3DFVF_NORMAL) ? 12 : 0) + ((d.fvf & D3DFVF_DIFFUSE) ? 4 : 0);
                    for (int k = 0; k < 4 && uvo + 8 <= d.stride; k++) {
                        const float *p = (const float *)(vb + (size_t)k * d.stride);
                        const float *uv = (const float *)(vb + (size_t)k * d.stride + uvo);
                        DWORD col = (d.fvf & D3DFVF_DIFFUSE) ? *(const DWORD *)(vb + (size_t)k * d.stride + uvo - 4) : 0;
                        log_text("[rtlog]    v%d (%g,%g,%g) uv (%g,%g) diffuse %08lx", k, p[0], p[1], p[2], uv[0], uv[1], (unsigned long)col);
                    }
                }
            }
        }
    }
    bool ui = g_active && g_ortho && g_sc.rt_is_bb && g_sc.active;
    IDirect3DTexture9 *shrunk = nullptr;   // stage 0 gets this shrunk copy
    if (g_rt_comp && g_ortho && g_sc.active) {
        // a draw into a companion: interface treatment only when 1 model unit = 1 px of the game's
        // render target (view x projection = 2/w, -2/h); else the original texture is drawn (into the
        // companion at 3x: the look of before)
        float sx = g_v11 * g_proj11, sy = g_v22 * g_proj22;
        ui = std::fabs(std::fabs(sx) * g_rt_comp->w * 0.5f - 1) < 1e-3f && std::fabs(std::fabs(sy) * g_rt_comp->h * 0.5f - 1) < 1e-3f &&
             std::fabs(g_w11 - 1) < 1e-4f && std::fabs(g_w22 - 1) < 1e-4f && std::fabs(g_w12) < 1e-4f && std::fabs(g_w21) < 1e-4f;
        static int nlog = 0;
        if (!ui && nlog < 4) { nlog++; log_text("[dragtext] draw into the companion with scale %g,%g (render target %ux%u): original texture", sx, sy, g_rt_comp->w, g_rt_comp->h); }
    }
    if (ui && !d.up_vtx && (d.pt == D3DPT_LINELIST || d.pt == D3DPT_LINESTRIP)) {
        static int seen = 0;   // lines from vertex buffers are not thickened: say so once
        if (seen < 4) { seen++; log_text("[thick] interface line draw from a vertex buffer (pt %d, %u prims): left 1 px", (int)d.pt, d.prims); }
    }
    if (ui) {
        st_draw_ui++;
        // only when stage 0 would get a shadow: check the texel-to-pixel mapping of this draw
        UpTex *u0 = nullptr;
        if (g_game_w[0]) {
            if (!g_stage_looked[0]) {
                g_stage_u[0] = info_for(g_game_w[0], g_game_r[0]);
                g_stage_looked[0] = true;
            }
            u0 = g_stage_u[0];
        }
        static int vlog = -1;   // test switch D3D8PROXY_VLOG=N: log the first vertex of the first N interface draws
        if (vlog < 0) { const char *e = getenv("D3D8PROXY_VLOG"); vlog = e ? atoi(e) : 0; }
        if (vlog > 0) {
            float x, y;
            if (first_vertex(d, &x, &y)) {
                vlog--;
                log_text("[vlog] ui draw fvf=0x%lx world t=(%g,%g) first vertex (%g,%g) -> half x=%d y=%d", (unsigned long)d.fvf,
                         g_w41, g_w42, x, y, g_halfx, g_halfy);
            }
        }
        static int nocheck = -1;   // test switch D3D8PROXY_UPNOCHECK=1: no 1:1 check (diagnosis)
        if (nocheck < 0) { const char *e = getenv("D3D8PROXY_UPNOCHECK"); nocheck = e && *e == '1'; }
        if (u0 && !u0->eligible && u0->w > 1024 && !u0->logged_stretch) {
            static int dall = -1;   // D3D8PROXY_UPDUMPALL=1: is the too-large texture drawn 1:1?
            if (dall < 0) { const char *e = getenv("D3D8PROXY_UPDUMPALL"); dall = e && *e == '1'; }
            if (dall) {
                u0->logged_stretch = true;
                float x = 0, y = 0;
                bool fv = first_vertex(d, &x, &y);
                log_text("[upscale] too-large %ux%u texture %p: first interface draw %s, first vertex %s %g,%g, %u prims",
                         u0->w, u0->h, u0->key, one_to_one(d, u0->w, u0->h) ? "1:1 (or unknown)" : "STRETCHED", fv ? "at" : "unknown", x, y, d.prims);
            }
        }
        if (u0 && g_shrink && !nocheck && !g_rt_comp && g_sc.rt_is_bb && u0->real) {
            // SmoothShrink: art drawn smaller than 1 real px per texel with point sampling gets a
            // copy shrunk to the drawn size (see shrink_scale). Never text, render targets, odd formats.
            if (!u0->mini_checked) {
                u0->mini_checked = true;
                D3DSURFACE_DESC dd;
                u0->mini_ok = SUCCEEDED(u0->real->GetLevelDesc(0, &dd)) &&
                              (dd.Format == D3DFMT_A8R8G8B8 || dd.Format == D3DFMT_X8R8G8B8) && dd.Pool == D3DPOOL_MANAGED &&
                              !(dd.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && dd.Width <= 4096 && dd.Height <= 4096 &&
                              dd.Width >= 64 && dd.Height >= 64 && dd.Width * dd.Height >= 32768;   // pictures, never fills/bars/icons
            }
            bool text = false;
#ifdef UP_HDTEXT
            text = g_hd && hdtext_has_text(u0->key);
#endif
            float sx, sy;
            g_shr_why = 0;
            bool shr = u0->mini_ok && !text && shrink_scale(dev, d, u0->w, u0->h, &sx, &sy);
            {
                static int sdiag = -1;   // test switch D3D8PROXY_SHRINKDIAG=N: why the first N draws of textures >= 512 wide were not shrunk
                if (sdiag < 0) { const char *e = getenv("D3D8PROXY_SHRINKDIAG"); sdiag = e ? atoi(e) : 0; }
                static unsigned sdn = 0;
                if (sdiag > 0 && !shr && u0->w >= 2048 && u0->h == 512 && (sdn++ % 200) == 0) {
                    sdiag--;
                    log_text("[shrinkdiag] tex %p %ux%u ok %d text %d why %d last j %g %g (%g %g) fvf 0x%lx valid %d pt %d prims %u up %d idx %d", u0->key, u0->w, u0->h,
                             (int)u0->mini_ok, (int)text, g_shr_why, g_shr_jx, g_shr_jy, g_shr_j12, g_shr_j21, (unsigned long)d.fvf, (int)d.fvf_valid, (int)d.pt, d.prims, (int)(d.up_vtx != nullptr), (int)d.indexed);
                    DWORD ttf = 0, tci = 0;
                    dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
                    dev->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &tci);
                    Mirror *m = mirror_of(d.vb), *mi = mirror_of(d.ib);
                    log_text("[shrinkdiag]   vp %u %u ttf %lu tci %lu world %g %g %g %g t %g %g view %g %g proj %g %g gw %u gh %u", g_shr_vpw, g_shr_vph, (unsigned long)ttf, (unsigned long)tci,
                             g_w11, g_w12, g_w21, g_w22, g_w41, g_w42, g_v11, g_v22, g_proj11, g_proj22, g_sc.gw, g_sc.gh);
                    if (m && mi && mi->elem == 2 && d.stride == 36)
                        for (UINT k = 0; k < 6 && (d.start + k + 1) * 2 <= mi->size; k++) {
                            UINT v = ((const uint16_t *)mi->mem.data())[d.start + k] + d.basevtx;
                            if ((size_t)(v + 1) * 36 > m->size) break;
                            const float *p = (const float *)(m->mem.data() + (size_t)v * 36);
                            log_text("[shrinkdiag]   v%u pos %g %g %g uv %g %g", v, p[0], p[1], p[2], p[7], p[8]);
                        }
                }
            }
            if (shr) {
                DWORD minf = 0, magf = 0, mipf = 0;
                dev->GetSamplerState(0, D3DSAMP_MINFILTER, &minf);
                dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &magf);
                dev->GetSamplerState(0, D3DSAMP_MIPFILTER, &mipf);
                UINT W = (UINT)std::lround(u0->w * sx), H = (UINT)std::lround(u0->h * sy);
                // point or bilinear without mip levels: a copy at the exact drawn size samples 1:1 either way
                bool point = (minf == D3DTEXF_POINT || minf == D3DTEXF_NONE || minf == D3DTEXF_LINEAR || minf == D3DTEXF_ANISOTROPIC) &&
                             (magf == D3DTEXF_POINT || magf == D3DTEXF_LINEAR);
                bool fits = W >= 2 && H >= 2 && W <= 4096 && H <= 4096 && (unsigned long long)W * H <= 8u * 1024 * 1024;
                IDirect3DTexture9 *t = (point && fits) ? shrink_get(dev, u0, W, H) : nullptr;
                if (u0->shrink_logW != W || u0->shrink_logH != H) {
                    u0->shrink_logW = W; u0->shrink_logH = H;
                    float x = 0, y = 0;
                    bool fv = first_vertex(d, &x, &y);
                    log_text("[shrink] f%ld tex %p %ux%u drawn at %ux%u real px (%.4f,%.4f real px per texel), first vertex %s %g,%g, %u prims, view %g %g proj %g %g, filter min %lu mag %lu mip %lu -> %s",
                             (long)g_frames, u0->key, u0->w, u0->h, W, H, sx, sy, fv ? "at" : "unknown", x, y, d.prims, g_v11, g_v22, g_proj11, g_proj22,
                             (unsigned long)minf, (unsigned long)magf, (unsigned long)mipf, t ? "RESAMPLED COPY" : !point ? "original (not point sampled)" : !fits ? "original (copy would be too big)" : "original (enlarged, no 4x file)");
                }
                if (t) {
                    for (auto &k : u0->minis) if (k.t == t) k.last_use = GetTickCount();
                    st_draw_shrunk++;
                    ui = false;   // no 3x shadows for this draw
                    shrunk = t;
                }
            }
        }
        if (u0 && u0->eligible && !nocheck) {
            float box[4] = {1e30f, 1e30f, -1e30f, -1e30f};
            bool ok11 = one_to_one(d, u0->w, u0->h, box);
            if (ok11 && g_tileedges && u0->rstate != 2) {
                // remember the texel rectangle of single-quad 1:1 draws (whole texels only); a
                // second, different rectangle or any other kind of use = the old whole-texture copy
                auto whole = [](float v) { return std::fabs(v - std::round(v)) < 0.02f; };
                bool good = d.prims <= 2 && (d.pt == D3DPT_TRIANGLELIST || d.pt == D3DPT_TRIANGLESTRIP || d.pt == D3DPT_TRIANGLEFAN) &&
                            box[0] <= box[2] && whole(box[0]) && whole(box[1]) && whole(box[2]) && whole(box[3]);
                int x0 = good ? (int)std::lround(box[0]) : 0, y0 = good ? (int)std::lround(box[1]) : 0;
                int x1 = good ? (int)std::lround(box[2]) : 0, y1 = good ? (int)std::lround(box[3]) : 0;
                good = good && x0 >= 0 && y0 >= 0 && x1 <= (int)u0->w && y1 <= (int)u0->h && x1 > x0 && y1 > y0;
                int ns = !good ? 2 : u0->rstate == 0 ? 1 : (x0 == u0->rx0 && y0 == u0->ry0 && x1 == u0->rx1 && y1 == u0->ry1) ? 1 : 2;
                if (ns != u0->rstate) {
                    if (ns == 1) { u0->rx0 = x0; u0->ry0 = y0; u0->rx1 = x1; u0->ry1 = y1; }
                    if (u0->rstate == 1 || u0->shadow) { u0->dirty = true; u0->hash = 0; }   // rebuild the copy
                    u0->rstate = ns;
                }
            }
            if (!ok11) {
                ui = false;
                st_draw_stretched++;
                if (!u0->logged_stretch) {
                    u0->logged_stretch = true;
                    log_text("[upscale] %ux%u texture %p drawn stretched/scaled: original kept (hash %016llx)", u0->w, u0->h,
                             u0->key, u0->hash);
                    dump_stretched(u0);
                }
            } else
                st_draw_shadow++;
        }
    }
    DWORD now = 0;
    for (DWORD s = 0; s < 4; s++) {
        IDirect3DBaseTexture9 *want = g_game_r[s];
        if (g_comp_n > 0 && g_game_w[s]) {
            // a game render target with a filled companion: bind the companion for every draw
            if (!g_stage_looked[s]) {
                g_stage_u[s] = info_for(g_game_w[s], g_game_r[s]);
                g_stage_looked[s] = true;
            }
            UpTex *u = g_stage_u[s];
            if (u->comp && u->comp_valid) {
                want = u->comp;   // the picture lives only there
                static int cdump = -1;   // test switch D3D8PROXY_COMPDUMP=N: N-th use of a companion -> <game>\comp_<w>x<h>.raw
                if (cdump < 0) { const char *e = getenv("D3D8PROXY_COMPDUMP"); cdump = e ? atoi(e) : 0; }
                if (cdump > 0 && --cdump == 0) {
                    UINT W = u->w * g_f, H = u->h * g_f;
                    IDirect3DSurface9 *img = nullptr;
                    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &img, nullptr)) && img) {
                        HRESULT cr = dev->GetRenderTargetData(u->comp_s, img);
                        D3DLOCKED_RECT lr;
                        if (SUCCEEDED(cr) && SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                            char b[64];
                            snprintf(b, sizeof b, "comp_%ux%u.raw", W, H);
                            HANDLE f = CreateFileA((g_dir + b).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                            if (f != INVALID_HANDLE_VALUE) {
                                DWORD wr;
                                for (UINT y = 0; y < H; y++) WriteFile(f, (const BYTE *)lr.pBits + (size_t)y * lr.Pitch, W * 4, &wr, NULL);
                                CloseHandle(f);
                            }
                            img->UnlockRect();
                        }
                        log_text("[dragtext] companion dump: CopyRects -> 0x%lx", (unsigned long)cr);
                        img->Release();
                    }
                }
            }
        }
        if (s == 0 && shrunk) want = shrunk;
        if (ui && g_game_w[s]) {
            if (!g_stage_looked[s]) {
                g_stage_u[s] = info_for(g_game_w[s], g_game_r[s]);
                g_stage_looked[s] = true;
            }
            UpTex *u = g_stage_u[s];
            if (u->eligible) {
                if (u->dirty)
                    refresh(dev, u);
                if (u->shadow && (!u->photo || g_photosmooth)) {
                    want = u->shadow;
                    if (!now) now = GetTickCount();
                    u->last_use = now;
                }
            }
        }
        if (want != g_bound[s]) {
            dev->SetTexture(s, want);
            g_bound[s] = want;
        }
    }
#ifdef UP_HDTEXT
    // text the game draws with alpha test and no blending (mono-font lines such as the mission
    // countdown) would cut the smooth edge of the sharp 3x letters at alpha 128 (hard stair steps);
    // for a 3x text copy drawn that way, blend instead (states restored by up_after_draw)
    if (g_rs_saved) up_after_draw(dev);   // never stack two saves
    if (ui && g_stage_u[0] && g_stage_u[0]->shadow && g_bound[0] == g_stage_u[0]->shadow && g_hd &&
        hdtext_has_text(g_stage_u[0]->key)) {
        DWORD ab = 1, at = 0;
        dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
        dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
        if (!ab && at) {
            dev->GetRenderState(D3DRS_SRCBLEND, &g_rs_src);
            dev->GetRenderState(D3DRS_DESTBLEND, &g_rs_dst);
            dev->GetRenderState(D3DRS_ALPHAREF, &g_rs_ref);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->SetRenderState(D3DRS_ALPHAREF, 0);
            g_rs_saved = true;
            static void *seen[64];
            static int nlog = 0;
            bool known = false;
            for (int i = 0; i < nlog; i++) known |= seen[i] == g_stage_u[0]->key;
            if (!known && nlog < 64) { seen[nlog++] = g_stage_u[0]->key; float fx = 0, fy = 0; bool fv = first_vertex(d, &fx, &fy); log_text("[h48] text copy %p %ux%u drawn with alpha test, no blend (ref %lu): blended; first vertex %s %g,%g", g_stage_u[0]->key, g_stage_u[0]->w, g_stage_u[0]->h, (unsigned long)g_rs_ref, fv ? "at" : "unknown", fx, fy); }
        }
    }
#endif
}

void up_after_draw(IDirect3DDevice9 *dev) {
    if (!g_rs_saved) return;
    g_rs_saved = false;
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, g_rs_src);
    dev->SetRenderState(D3DRS_DESTBLEND, g_rs_dst);
    dev->SetRenderState(D3DRS_ALPHAREF, g_rs_ref);
}

// ---- thick interface lines (ThickLines=1) --------------------------------------------------------
// Direct3D lines are 1 real pixel wide whatever the scale, so the mission drag-select rectangle (two
// line strips, white + grey, 1 game px apart) shows at a third of its designed thickness. In the
// interface pass (orthographic projection on the back buffer, scaled layer active) a line draw from
// user memory is redrawn as quads 1 game px wide (= F real px), same vertices otherwise. Each segment
// covers [start - 0.5, end - 0.5] along its direction: corners are covered exactly once (no double
// blending) and, like a Direct3D line, the last pixel of an open strip is left out. 3D-world lines
// (perspective projection) and render-texture draws are never touched.
namespace { std::vector<BYTE> g_thickbuf; unsigned long long st_thick = 0; }

void up_set_view(const D3DMATRIX *m) {
    if (!m) return;
    g_v11 = m->_11; g_v22 = m->_22; g_v41 = m->_41; g_v42 = m->_42; g_v12 = m->_12; g_v21 = m->_21;
    g_view_unit = std::fabs(std::fabs(m->_11) - 1) < 1e-3f && std::fabs(std::fabs(m->_22) - 1) < 1e-3f &&
                  std::fabs(m->_12) < 1e-3f && std::fabs(m->_21) < 1e-3f;
}

bool up_thick_lines(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE pt, UINT n, const void *data, UINT stride, DWORD fvf,
                    bool fvf_valid, HRESULT *hr) {
    if (!g_thick || !data || !n || (pt != D3DPT_LINELIST && pt != D3DPT_LINESTRIP))
        return false;
    if (!fvf_valid || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || stride < 12)
        return false;
    UpLock lk_;
    if (!(g_ortho && g_sc.rt_is_bb && g_sc.active && g_view_unit))
        return false;
    // the half-width offsets assume 1 model unit = 1 game px, i.e. the orthographic
    // projection maps the game's back-buffer width/height onto clip space (2/W, 2/H); any other
    // projection scale: draw the line as before
    if (!g_sc.gw || !g_sc.gh || std::fabs(std::fabs(g_proj11) * g_sc.gw * 0.5f - 1) > 1e-3f ||
        std::fabs(std::fabs(g_proj22) * g_sc.gh * 0.5f - 1) > 1e-3f) {
        static int nlog = 0;
        if (nlog < 4) { nlog++; log_text("[thick] projection scale %g,%g is not 1 unit = 1 game px (%ux%u): line left 1 px", g_proj11, g_proj22, g_sc.gw, g_sc.gh); }
        return false;
    }
    float det = g_w11 * g_w22 - g_w12 * g_w21;
    if (std::fabs(det) < 1e-6f || n > 4096 || stride > 256)
        return false;
    {
        static int seen = 0;
        if (seen < 8) {
            seen++;
            const float *p = (const float *)data;
            log_text("[thick] interface %s, %u segments, fvf 0x%lx, first vertex (%g,%g) -> drawn as quads",
                     pt == D3DPT_LINELIST ? "LINELIST" : "LINESTRIP", n, (unsigned long)fvf, p[0], p[1]);
        }
    }
    const BYTE *v = (const BYTE *)data;
    g_thickbuf.resize((size_t)n * 6 * stride);
    BYTE *o = g_thickbuf.data();
    UINT tris = 0;
    // screen (game px) offset -> model offset: inverse of the world 2x2 (row vectors: s = m * W)
    auto to_model = [&](float sx, float sy, float *mx, float *my) {
        *mx = (sx * g_w22 - sy * g_w21) / det;
        *my = (sy * g_w11 - sx * g_w12) / det;
    };
    for (UINT i = 0; i < n; i++) {
        const BYTE *a = pt == D3DPT_LINELIST ? v + (size_t)(2 * i) * stride : v + (size_t)i * stride;
        const BYTE *b = a + stride;
        const float *pa = (const float *)a, *pb = (const float *)b;
        float ax = pa[0] * g_w11 + pa[1] * g_w21, ay = pa[0] * g_w12 + pa[1] * g_w22;
        float bx = pb[0] * g_w11 + pb[1] * g_w21, by = pb[0] * g_w12 + pb[1] * g_w22;
        float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f)
            continue;
        dx /= len; dy /= len;
        float nx = -dy * 0.5f, ny = dx * 0.5f;     // half width
        float sx = -dx * 0.5f, sy = -dy * 0.5f;    // both ends moved back by half a pixel
        float c[4][2] = {{sx + nx, sy + ny}, {sx - nx, sy - ny}, {sx + nx, sy + ny}, {sx - nx, sy - ny}};
        const BYTE *src[4] = {a, a, b, b};
        BYTE tmp[4][256];
        for (int k = 0; k < 4; k++) {
            memcpy(tmp[k], src[k], stride);
            float mx, my;
            to_model(c[k][0], c[k][1], &mx, &my);
            float *p = (float *)tmp[k];
            p[0] += mx;
            p[1] += my;
        }
        const int order[6] = {0, 1, 2, 2, 1, 3};
        for (int k = 0; k < 6; k++)
            memcpy(o + (size_t)(tris * 3 + k) * stride, tmp[order[k]], stride);
        tris += 2;
    }
    if (!tris)
        return false;
    // the quads' winding depends on the line direction: no culling for this draw
    DWORD cull = D3DCULL_CCW;
    bool gotcull = SUCCEEDED(dev->GetRenderState(D3DRS_CULLMODE, &cull));
    if (gotcull && cull != D3DCULL_NONE) dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    *hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, tris, g_thickbuf.data(), stride);
    if (gotcull && cull != D3DCULL_NONE) dev->SetRenderState(D3DRS_CULLMODE, cull);
    st_thick++;
    return true;
}

// ListText: Device::CopyRects with HD glyph records carried from the source texture to the
// destination (the list rows are copied into a list canvas texture). true = copy done here.
bool up_copy_rects(IDirect3DDevice9 *real_dev, IDirect3DSurface9 *ws, IDirect3DSurface9 *rs, const RECT *rects, UINT n,
                   IDirect3DSurface9 *wd, IDirect3DSurface9 *rd, const POINT *pts, HRESULT *hr) {
#ifdef UP_HDTEXT
    if (!g_on || !g_hd || !g_dblfix || !rs || !rd || !ws || !wd || n == 0 || n > 64)
        return false;
    D3DSURFACE_DESC dS, dD;
    if (FAILED(rs->GetDesc(&dS)) || FAILED(rd->GetDesc(&dD)) || dS.Format != D3DFMT_A8R8G8B8 ||
        dD.Format != D3DFMT_A8R8G8B8 || dS.Width > 1024 || dS.Height > 1024)
        return false;
    auto grab = [](IDirect3DSurface9 *s, const RECT &r, std::vector<uint32_t> &out) -> bool {
        D3DLOCKED_RECT lr;
        if (FAILED(s->LockRect(&lr, NULL, D3DLOCK_READONLY))) return false;
        int w = r.right - r.left;
        out.resize((size_t)w * (r.bottom - r.top));
        for (LONG y = r.top; y < r.bottom; y++)
            memcpy(&out[(size_t)(y - r.top) * w], (const BYTE *)lr.pBits + (size_t)y * lr.Pitch + r.left * 4, w * 4);
        s->UnlockRect();
        return true;
    };
    RECT whole = {0, 0, (LONG)dS.Width, (LONG)dS.Height};
    std::vector<uint32_t> src;
    if (!grab(rs, whole, src)) return false;
    struct Part { RECT s, d; std::vector<uint32_t> before; };
    std::vector<Part> parts;
    for (UINT i = 0; i < (rects ? n : 1); i++) {
        Part p;
        p.s = rects ? rects[i] : whole;
        LONG px = pts ? pts[i].x : p.s.left, py = pts ? pts[i].y : p.s.top;
        p.d = {px, py, px + (p.s.right - p.s.left), py + (p.s.bottom - p.s.top)};
        if (p.s.left < 0 || p.s.top < 0 || p.s.right > (LONG)dS.Width || p.s.bottom > (LONG)dS.Height ||
            p.d.left < 0 || p.d.top < 0 || p.d.right > (LONG)dD.Width || p.d.bottom > (LONG)dD.Height ||
            p.s.right <= p.s.left || p.s.bottom <= p.s.top)
            return false;   // let the real call judge odd rectangles
        if (!grab(rd, p.d, p.before)) return false;
        parts.push_back(std::move(p));
    }
    {   // d3d9 has no CopyRects: copy on the CPU (both are lockable 32-bit surfaces here)
        (void)real_dev;
        D3DLOCKED_RECT ld;
        *hr = rd->LockRect(&ld, NULL, 0);
        if (FAILED(*hr)) return true;
        for (Part &p : parts)
            for (LONG y = 0; y < p.s.bottom - p.s.top; y++)
                memcpy((BYTE *)ld.pBits + (size_t)(p.d.top + y) * ld.Pitch + p.d.left * 4,
                       &src[(size_t)(p.s.top + y) * dS.Width + p.s.left], (p.s.right - p.s.left) * 4);
        rd->UnlockRect();
    }
    if (FAILED(*hr)) return true;
    for (Part &p : parts) {
        std::vector<uint32_t> after;
        if (!grab(rd, p.d, after)) continue;
        int c = hdtext_carry_copy(ws, src.data(), (int)dS.Width * 4, (int)dS.Width, (int)dS.Height, wd, p.before.data(),
                                  after.data(), p.d.left, p.d.top, p.d.right - p.d.left, p.d.bottom - p.d.top, p.s.left,
                                  p.s.top, p.s.right, p.s.bottom, p.d.left - p.s.left, p.d.top - p.s.top);
        static int logged = 0;
        if (c > 0 && logged < 10) { logged++; log_text("[upscale] CopyRects %ux%u -> %ux%u: %d glyph records carried", dS.Width, dS.Height, dD.Width, dD.Height, c); }
    }
    return true;
#else
    return false;
#endif
}

void up_present() {
    if (!g_on)
        return;
    UpLock lk_;
    long f = g_frames;
    if (g_toggle_file && f % 30 == 0) {
        bool off = GetFileAttributesA((g_dir + "upscale.off").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (off == g_active) {
            g_active = !off;
            log_text("[upscale] runtime toggle -> %s", g_active ? "on" : "off");
        }
    }
    if (f % 600 == 0) {
        // evict shadows not used by the interface for 60 s (released textures, old strings)
        DWORD now = GetTickCount();
        for (auto &kv : g_tex) {
            UpTex *u = kv.second;
            if (u->shadow && now - u->last_use > 60000) {
                bool bound = false;
                for (int s = 0; s < 8; s++) bound |= g_bound[s] == u->shadow;
                if (bound) continue;
                u->shadow->Release();
                u->shadow = nullptr;
                u->dirty = true;
                st_evicted++;
            }
            for (auto &k : u->minis) {   // shrunk copies unused for 60 s
                if (!k.t || now - k.last_use <= 60000) continue;
                bool bound = false;
                for (int s = 0; s < 8; s++) bound |= g_bound[s] == k.t;
                if (bound) continue;
                k.t->Release();
                k.t = nullptr;
                k.hash = 0;
            }
        }
    }
    if (g_stats && GetTickCount() - w_t0 >= 5000)
        write_stats();
}

void up_texture_created(void *w) {
    if (!g_on)
        return;
#ifdef UP_HDTEXT
    if (g_hd)
        hdtext_forget(w);   // glyph records of the texture that lived behind this wrapper before
#endif
    UpLock lk_;
    auto it = g_tex.find(w);
    if (it != g_tex.end()) {
        // The wrapper is reused for a new real texture at the same address (the game re-creates its
        // text textures often). Keep the shadow object when the size still fits, else drop it.
        UpTex *u = it->second;
        if (u->comp) {   // the companion belonged to the texture that lived here before
            if (g_rt_comp == u) g_rt_comp = nullptr;
            if (u->comp_s) u->comp_s->Release();
            bool bound = false;
            for (int s = 0; s < 8; s++) bound |= g_bound[s] == u->comp;
            if (bound) g_comp_orphans.push_back(u->comp);
            else u->comp->Release();
            u->comp = nullptr; u->comp_s = nullptr;
            g_comp_n--;
        }
        release_minis(u);   // shrunk copies of the texture that lived here before
        IDirect3DTexture9 *sh = u->shadow;
        UINT ow = u->w, oh = u->h;
        IDirect3DTexture9 *real = u->real;
        delete u;
        g_tex.erase(it);
        if (sh) {
            UpTex *n = real ? info_for((IDirect3DBaseTexture9 *)w, real) : nullptr;
            if (n && n->eligible && n->w == ow && n->h == oh) {
                n->shadow = sh;   // reused; dirty + hash 0 force a refill at the next interface use
                st_reused++;
            } else {
                bool bound = false;
                for (int s = 0; s < 8; s++) bound |= g_bound[s] == sh;
                if (!bound) sh->Release();   // (a still-bound one is left alone: rare, small leak)
            }
        }
    }
    for (int s = 0; s < 8; s++) g_stage_looked[s] = false;
}

// The game released its last reference on this texture: drop the per-texture record and every
// copy made for it (same clean-up as up_texture_created does for a reused wrapper, without the reuse).
void up_texture_released(void *w) {
    if (!g_on)
        return;
#ifdef UP_HDTEXT
    if (g_hd)
        hdtext_forget(w);
#endif
    UpLock lk_;
    for (auto it = g_surf0.begin(); it != g_surf0.end();)
        it = it->second == w ? g_surf0.erase(it) : std::next(it);
    auto it = g_tex.find(w);
    if (it == g_tex.end())
        return;
    UpTex *u = it->second;
    auto drop = [](IDirect3DTexture9 *t) {   // a copy still bound in the real device waits in the orphan list
        bool bound = false;
        for (int s = 0; s < 8; s++) bound |= g_bound[s] == t;
        if (bound) g_comp_orphans.push_back(t);
        else t->Release();
    };
    if (u->comp) {
        if (g_rt_comp == u) g_rt_comp = nullptr;
        if (u->comp_s) u->comp_s->Release();
        drop(u->comp);
        g_comp_n--;
    }
    release_minis(u);
    if (u->shadow) drop(u->shadow);
    for (int s = 0; s < 8; s++) {
        if (g_stage_u[s] == u) g_stage_u[s] = nullptr;
        g_stage_looked[s] = false;
    }
    delete u;
    g_tex.erase(it);
}

void up_surface_level(void *w, UINT level, IDirect3DSurface9 *rs) {
    if (g_on && level == 0 && rs) {
        UpLock lk_;
        g_surf0[rs] = w;
    }
}

void up_surface_written(IDirect3DSurface9 *rs) {
    if (!g_on || !rs)
        return;
    UpLock lk_;
    auto it = g_surf0.find(rs);
    if (it == g_surf0.end())
        return;
    auto jt = g_tex.find(it->second);
    if (jt != g_tex.end())
        jt->second->dirty = jt->second->mini_dirty = true;
}

void up_texture_written(void *w, UINT level) {
    if (!g_on || level != 0)
        return;
    UpLock lk_;
    auto jt = g_tex.find(w);
    if (jt != g_tex.end())
        jt->second->dirty = jt->second->mini_dirty = true;
}

void up_texture_written_real(IDirect3DBaseTexture9 *) {}

// ---- render-target companions (drag sprite), see g_rt_comp ---------------------------------------
IDirect3DSurface9 *up_set_render_target(IDirect3DDevice9 *dev, IDirect3DSurface9 *rr, IDirect3DSurface9 *rz) {
    if (!g_on || !rr)
        return rr;
    UpLock lk_;
    if (!g_comp_orphans.empty()) {
        for (auto *t : g_comp_orphans) {
            bool bound = false;
            for (int s = 0; s < 8; s++) bound |= g_bound[s] == t;
            if (!bound) t->Release();
        }
        g_comp_orphans.erase(std::remove_if(g_comp_orphans.begin(), g_comp_orphans.end(), [](IDirect3DTexture9 *t) {
            for (int s = 0; s < 8; s++) if (g_bound[s] == t) return false;
            return true; }), g_comp_orphans.end());
    }
    g_rt_comp = nullptr;
    g_rt_real_comp = false;
    if (!g_comp_on || !g_sc.active || g_f < 2 || (void *)rr == g_sc.bb)
        return rr;
    auto it = g_surf0.find(rr);
    if (it == g_surf0.end())
        return rr;
    IDirect3DTexture9 *rt = nullptr;
    if (FAILED(rr->GetContainer(kIID_Tex9up, (void **)&rt)) || !rt)
        return rr;
    UpTex *u = info_for((IDirect3DBaseTexture9 *)it->second, rt);
    rt->Release();   // info_for keeps the pointer like for every texture (the game holds it)
    // only the drag sprite target: 256x128 A8R8G8B8 render-target texture, no depth buffer
    if (!u->rt || u->w != 256 || u->h != 128 || !u->alpha || u->comp_off)
        return rr;
    if (rz) {
        u->comp_off = true;
        u->comp_valid = false;
        log_text("[dragtext] %ux%u render target %p set with a depth buffer: left at 1x for good", u->w, u->h, u->key);
        return rr;
    }
    if (!u->comp) {
        if (FAILED(dev->CreateTexture(u->w * g_f, u->h * g_f, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &u->comp, nullptr)) || !u->comp) {
            u->comp = nullptr;
            u->comp_off = true;
            log_text("[dragtext] CreateTexture %ux%u render target FAILED: left at 1x", u->w * g_f, u->h * g_f);
            return rr;
        }
        if (FAILED(u->comp->GetSurfaceLevel(0, &u->comp_s)) || !u->comp_s) {
            u->comp->Release();
            u->comp = nullptr;
            u->comp_s = nullptr;
            u->comp_off = true;
            return rr;
        }
        g_comp_n++;
        log_text("[dragtext] %ux%u render target %p drawn through a %ux%u companion", u->w, u->h, u->key, u->w * g_f, u->h * g_f);
    }
    u->game_s = rr;
    u->comp_valid = true;
    g_rt_comp = u;
    g_rt_real_comp = true;
    return u->comp_s;
}

void up_companion_failed() {
    UpLock lk_;
    if (g_rt_comp) {
        g_rt_comp->comp_off = true;
        g_rt_comp->comp_valid = false;
        log_text("[dragtext] SetRenderTarget on the companion FAILED: render target left at 1x for good");
    }
    g_rt_comp = nullptr;
    g_rt_real_comp = false;
}

IDirect3DSurface9 *up_game_render_target(IDirect3DSurface9 *real) {
    if (!real || g_comp_n == 0)
        return real;
    UpLock lk_;
    for (auto &kv : g_tex) {
        UpTex *u = kv.second;
        if (u->comp_s && u->comp_s == real && u->game_s) {
            u->game_s->AddRef();
            real->Release();
            return u->game_s;
        }
    }
    return real;
}

// g_rt_comp is read once, under the lock (up_texture_created may clear it on another thread)
bool up_rt_companion(UINT *rw, UINT *rh) {
    if (!g_on) return false;
    UpLock lk_;
    UpTex *c = g_rt_comp;
    if (!c) return false;
    *rw = c->w * g_f;
    *rh = c->h * g_f;
    return true;
}

void up_viewport_in(D3DVIEWPORT9 *v) {
    if (!g_on || !v) return;
    UpLock lk_;
    UpTex *c = g_rt_comp;
    if (!c) return;
    UINT W = c->w * g_f, H = c->h * g_f;
    DWORD x0 = v->X * g_f, y0 = v->Y * g_f, x1 = (v->X + v->Width) * g_f, y1 = (v->Y + v->Height) * g_f;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    v->X = x0; v->Y = y0;
    v->Width = x1 > x0 ? x1 - x0 : 0;
    v->Height = y1 > y0 ? y1 - y0 : 0;
}

void up_viewport_out(D3DVIEWPORT9 *v) {
    if (!g_on || !v) return;
    UpLock lk_;
    if (!g_rt_comp) return;
    DWORD x0 = v->X / g_f, y0 = v->Y / g_f, x1 = (v->X + v->Width) / g_f, y1 = (v->Y + v->Height) / g_f;
    v->X = x0; v->Y = y0; v->Width = x1 - x0; v->Height = y1 - y0;
}

const D3DRECT *up_clear_rects(DWORD n, const D3DRECT *rects, D3DRECT *buf, DWORD bufn) {
    if (!g_on || !n || !rects || n > bufn)
        return rects;
    UpLock lk_;
    if (!g_rt_comp)
        return rects;
    for (DWORD i = 0; i < n; i++) {
        buf[i].x1 = rects[i].x1 * g_f; buf[i].y1 = rects[i].y1 * g_f;
        buf[i].x2 = rects[i].x2 * g_f; buf[i].y2 = rects[i].y2 * g_f;
    }
    return buf;
}

void up_release_companions(IDirect3DDevice9 *dev) {
    if (!g_on)
        return;
    UpLock lk_;
    // also when the texture was re-created while its companion was still the real target
    if ((g_rt_comp || g_rt_real_comp || g_comp_n > 0 || !g_comp_orphans.empty()) && g_sc.bb)
        dev->SetRenderTarget(0, (IDirect3DSurface9 *)g_sc.bb);   // never left on a companion at Reset
    g_rt_comp = nullptr;
    g_rt_real_comp = false;
    for (int s = 0; s < 8; s++) {
        bool ours = false;
        for (auto &kv : g_tex) ours |= kv.second->comp && g_bound[s] == kv.second->comp;
        for (auto *t : g_comp_orphans) ours |= g_bound[s] == t;
        if (ours) { dev->SetTexture(s, nullptr); g_bound[s] = nullptr; }
    }
    for (auto &kv : g_tex) {
        UpTex *u = kv.second;
        if (u->comp_s) u->comp_s->Release();
        if (u->comp) { u->comp->Release(); g_comp_n--; }
        u->comp = nullptr; u->comp_s = nullptr; u->comp_valid = false; u->game_s = nullptr;
    }
    for (auto *t : g_comp_orphans) t->Release();
    g_comp_orphans.clear();
    g_comp_n = 0;
}
