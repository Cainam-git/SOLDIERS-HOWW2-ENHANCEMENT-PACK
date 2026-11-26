// src/ee/d3dhook.cpp: Direct3D 9 hooks of the Enhanced Edition proxy.
//
// Method: the real (DXVK) objects are handed to the game unchanged; selected entries of their
// vtables are replaced in place (one vtable per class: device, texture, surface, vertex buffer,
// index buffer). Installed only when uiscale.ini [EE] D3DLog=1 (or env EE_D3DLOG=1) or when a
// later feature module asks for it.
//
// D3DLog=1 (logging build):
//   - ee.log gets CreateDevice/Reset parameters and the first texture creations.
//   - ONE-FRAME RECORD: create the file "ee_capture.now" beside Soldiers.exe. At the next Present the
//     file is renamed to ee_capture.taken_N and the frame after it is written call by call to
//     ee_frame_N.txt (every draw with vertex format, decoded sample vertices, screen box, bound
//     textures, the state calls in between); textures bound in that frame are dumped once to
//     ee_frame_N/tex_<id>.dds (readable pools only).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <map>
#include <vector>
#include <string>
#include "ee.h"
#include "up/up9.h"
#include "up/hdtext9.h"

// shared with up/up9.cpp (its g_uplk): ONE lock for the hooks and the upscaler (no lock-order problems)
CRITICAL_SECTION g_ee_cs;
volatile LONG g_frames;   // Present count (up9)
ScaleState g_sc;          // what up9 reads of the old scale layer (see up/compat9.h)
std::string fmtname(D3DFORMAT f);

namespace {
__thread int g_inner;     // >0: the call comes from the upscaler itself: hooks pass straight through
struct Inner { Inner() { g_inner++; } ~Inner() { g_inner--; } };
int g_up;                 // UiUpscale (art + sharp text) configured on
int g_upinit;             // up_init done (factor known)
int g_upfactor;
IDirect3DSurface9 *g_bb;  // back buffer (not referenced)
UINT g_bbw, g_bbh;
bool g_fvf_current;       // the last vertex format call was SetFVF (not a declaration)
char g_ini[MAX_PATH];
void drain_releases();

const GUID kIID_Tex9 = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};
#define cs g_ee_cs
struct Lock {
    Lock() { EnterCriticalSection(&cs); }
    ~Lock() { LeaveCriticalSection(&cs); }
};

int g_dlog;                 // D3DLog
int g_texlog_left = 400;    // texture creations logged to ee.log
unsigned g_frame;           // Present count
IDirect3DDevice9 *g_dev;
void *o_TexLockRect, *o_TexUnlockRect;

// ------------------------------------------------------------------ vtable patching
void *vpatch(void **vt, int idx, void *fn) {
    if (vt[idx] == fn)
        return nullptr;
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    void *o = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
    return o;
}
#define VT(obj) (*(void ***)(obj))

// ------------------------------------------------------------------ resource bookkeeping
struct TexInfo {
    int id;
    UINT w, h, levels;
    DWORD usage;
    D3DFORMAT fmt;
    D3DPOOL pool;
    unsigned created_frame, locks, last_lock_frame;
    bool dumped;
};
std::map<void *, TexInfo> g_tex;   // IDirect3DTexture9* -> info
int g_texid;

struct BufInfo {
    UINT len;
    DWORD usage, fvf;
    D3DPOOL pool;
    D3DFORMAT ifmt;
    std::vector<unsigned char> shadow;   // CPU copy of everything written through Lock/Unlock
    void *lockptr;
    UINT lockoff, locklen;
    unsigned locks;
};
std::map<void *, BufInfo> g_vb, g_ib;
std::map<void *, std::vector<D3DVERTEXELEMENT9>> g_decl;

const char *fmtn(D3DFORMAT f) {
    switch ((unsigned)f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_A1R5G5B5: return "A1R5G5B5";
    case D3DFMT_X1R5G5B5: return "X1R5G5B5";
    case D3DFMT_A4R4G4B4: return "A4R4G4B4";
    case D3DFMT_A8: return "A8";
    case D3DFMT_L8: return "L8";
    case D3DFMT_A8L8: return "A8L8";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D32: return "D32";
    case D3DFMT_INDEX16: return "INDEX16";
    case D3DFMT_INDEX32: return "INDEX32";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
    }
    static char b[8][16];
    static int k;
    char *s = b[k++ & 7];
    if ((unsigned)f > 0xffff) {
        unsigned v = f;
        snprintf(s, 16, "%c%c%c%c", v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24);
    } else
        snprintf(s, 16, "fmt%u", (unsigned)f);
    return s;
}
const char *poolname(D3DPOOL p) {
    switch (p) {
    case D3DPOOL_DEFAULT: return "DEFAULT";
    case D3DPOOL_MANAGED: return "MANAGED";
    case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
    case D3DPOOL_SCRATCH: return "SCRATCH";
    default: return "pool?";
    }
}
std::string usagestr(DWORD u) {
    std::string s;
    if (u & D3DUSAGE_RENDERTARGET) s += "RT|";
    if (u & D3DUSAGE_DEPTHSTENCIL) s += "DS|";
    if (u & D3DUSAGE_DYNAMIC) s += "DYNAMIC|";
    if (u & D3DUSAGE_WRITEONLY) s += "WRITEONLY|";
    if (u & D3DUSAGE_AUTOGENMIPMAP) s += "AUTOGENMIP|";
    if (u & ~(DWORD)(D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY |
                     D3DUSAGE_AUTOGENMIPMAP)) {
        char b[16];
        snprintf(b, 16, "0x%lx|", u);
        s += b;
    }
    if (s.empty()) return "0";
    s.pop_back();
    return s;
}
std::string texstr(void *t) {
    if (!t) return "none";
    auto it = g_tex.find(t);
    char b[160];
    if (it == g_tex.end()) {
        snprintf(b, sizeof b, "%p(unknown)", t);
        return b;
    }
    const TexInfo &i = it->second;
    snprintf(b, sizeof b, "T%d %ux%u %s %s lv%u%s%s locks=%u lastlock=f%u", i.id, i.w, i.h, fmtn(i.fmt),
             poolname(i.pool), i.levels, i.usage ? " " : "", i.usage ? usagestr(i.usage).c_str() : "", i.locks,
             i.last_lock_frame);
    return b;
}

// ------------------------------------------------------------------ recording state
FILE *g_rec;           // open while one frame is recorded
int g_capn;            // capture number
int g_capstate;        // 0 idle, 1 start at next Present
char g_capdir[MAX_PATH];
unsigned g_ndraw;
// tracked state (only what is needed to describe a draw)
DWORD g_fvf;
void *g_vdecl;
void *g_vs, *g_ps;
void *g_tex_bound[8];
struct Stream {
    void *vb;
    UINT off, stride;
} g_stream0;
void *g_ibound;
D3DMATRIX g_world, g_view, g_proj;
D3DVIEWPORT9 g_vp;
DWORD g_rs[256];
DWORD g_ss[8][16];
DWORD g_tss[8][33];

void R(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void R(const char *fmt, ...) {
    if (!g_rec) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(g_rec, fmt, a);
    va_end(a);
    fputc('\n', g_rec);
}

std::string mat(const D3DMATRIX &m) {
    char b[400];
    snprintf(b, sizeof b, "[%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", m._11, m._12, m._13, m._14,
             m._21, m._22, m._23, m._24, m._31, m._32, m._33, m._34, m._41, m._42, m._43, m._44);
    return b;
}

// DDS dump of level 0 of a texture (readable pools only)
void dump_tex(IDirect3DTexture9 *t, TexInfo &ti) {
    if (ti.dumped || !g_capdir[0]) return;
    ti.dumped = true;
    if (ti.pool == D3DPOOL_DEFAULT) return;
    D3DLOCKED_RECT lr;
    typedef HRESULT(STDMETHODCALLTYPE * PLock)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
    if (FAILED(((PLock)o_TexLockRect)(t, 0, &lr, nullptr, D3DLOCK_READONLY))) return;
    unsigned w = ti.w, h = ti.h;
    bool dxt = ti.fmt == D3DFMT_DXT1 || ti.fmt == D3DFMT_DXT2 || ti.fmt == D3DFMT_DXT3 || ti.fmt == D3DFMT_DXT4 ||
               ti.fmt == D3DFMT_DXT5;
    unsigned bpp = 4, rbits = 0, gbits = 0, bbits = 0, abits = 0;
    DWORD rm = 0, gm = 0, bm = 0, am = 0;
    switch ((unsigned)ti.fmt) {
    case D3DFMT_A8R8G8B8: rm = 0xff0000; gm = 0xff00; bm = 0xff; am = 0xff000000; break;
    case D3DFMT_X8R8G8B8: rm = 0xff0000; gm = 0xff00; bm = 0xff; break;
    case D3DFMT_R5G6B5: bpp = 2; rm = 0xf800; gm = 0x7e0; bm = 0x1f; break;
    case D3DFMT_A1R5G5B5: bpp = 2; rm = 0x7c00; gm = 0x3e0; bm = 0x1f; am = 0x8000; break;
    case D3DFMT_X1R5G5B5: bpp = 2; rm = 0x7c00; gm = 0x3e0; bm = 0x1f; break;
    case D3DFMT_A4R4G4B4: bpp = 2; rm = 0xf00; gm = 0xf0; bm = 0xf; am = 0xf000; break;
    case D3DFMT_A8: bpp = 1; am = 0xff; break;
    case D3DFMT_L8: bpp = 1; rm = 0xff; break;
    default:
        if (!dxt) { ((HRESULT(STDMETHODCALLTYPE *)(IDirect3DTexture9 *, UINT))o_TexUnlockRect)(t, 0); return; }
    }
    (void)rbits; (void)gbits; (void)bbits; (void)abits;
    char p[MAX_PATH];
    snprintf(p, MAX_PATH, "%s\\tex_%d.dds", g_capdir, ti.id);
    FILE *f = fopen(p, "wb");
    if (f) {
        DWORD hdr[32] = {0};
        hdr[0] = 0x20534444; // "DDS "
        hdr[1] = 124;
        hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (dxt ? 0x80000 : 0x8);
        hdr[3] = h;
        hdr[4] = w;
        unsigned rows = dxt ? (h + 3) / 4 : h;
        unsigned rowbytes = dxt ? ((w + 3) / 4) * (ti.fmt == D3DFMT_DXT1 ? 8 : 16) : w * bpp;
        hdr[5] = dxt ? rowbytes * rows : rowbytes;
        hdr[19] = 32;
        if (dxt) {
            hdr[20] = 0x4;
            hdr[21] = ti.fmt;
        } else {
            hdr[20] = (am ? 0x1 : 0) | (rm || gm || bm ? 0x40 : 0x2);
            hdr[22] = bpp * 8;
            hdr[23] = rm; hdr[24] = gm; hdr[25] = bm; hdr[26] = am;
        }
        hdr[27] = 0x1000;
        fwrite(hdr, 4, 32, f);
        for (unsigned y = 0; y < rows; y++)
            fwrite((const char *)lr.pBits + y * lr.Pitch, 1, rowbytes, f);
        fclose(f);
    }
    ((HRESULT(STDMETHODCALLTYPE *)(IDirect3DTexture9 *, UINT))o_TexUnlockRect)(t, 0);
}

// layout of one vertex: offsets (or -1) of position, colour, uv0
struct Layout {
    int pos = -1, posw = 0;  // posw: 3 = XYZ, 4 = XYZRHW / POSITIONT
    bool rhw = false;
    int col = -1, uv = -1, uvn = 2, ntex = 0;
    std::string desc;
};
Layout layout_fvf(DWORD fvf) {
    Layout L;
    char b[160];
    int o = 0;
    DWORD p = fvf & D3DFVF_POSITION_MASK;
    if (p == D3DFVF_XYZRHW) { L.pos = 0; L.posw = 4; L.rhw = true; o = 16; }
    else if (p == D3DFVF_XYZ) { L.pos = 0; L.posw = 3; o = 12; }
    else if (p) { L.pos = 0; L.posw = 3; o = 12 + 4 * (((p - D3DFVF_XYZB1) >> 1) + 1); }
    if (fvf & D3DFVF_NORMAL) o += 12;
    if (fvf & D3DFVF_PSIZE) o += 4;
    if (fvf & D3DFVF_DIFFUSE) { L.col = o; o += 4; }
    if (fvf & D3DFVF_SPECULAR) o += 4;
    L.ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (L.ntex) {
        L.uv = o;
        int sz = (fvf >> 16) & 3;
        L.uvn = sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1;
    }
    snprintf(b, sizeof b, "FVF 0x%lx (%s%s%s%s tex%d)", fvf, p == D3DFVF_XYZRHW ? "XYZRHW" : p == D3DFVF_XYZ ? "XYZ" : "XYZB",
             fvf & D3DFVF_NORMAL ? "|NORMAL" : "", fvf & D3DFVF_DIFFUSE ? "|DIFFUSE" : "",
             fvf & D3DFVF_SPECULAR ? "|SPECULAR" : "", L.ntex);
    L.desc = b;
    return L;
}
Layout layout_decl(void *decl) {
    Layout L;
    auto it = g_decl.find(decl);
    if (it == g_decl.end()) { L.desc = "decl ?"; return L; }
    std::string d = "decl{";
    for (auto &e : it->second) {
        if (e.Stream == 0xff) break;
        char b[64];
        static const char *U[] = {"POS", "BLENDW", "BLENDI", "NORMAL", "PSIZE", "TEX", "TANGENT", "BINORMAL",
                                  "TESSF", "POST", "COLOR", "FOG", "DEPTH", "SAMPLE"};
        snprintf(b, sizeof b, "s%d+%d %s%d t%d,", e.Stream, e.Offset, e.Usage < 14 ? U[e.Usage] : "?", e.UsageIndex,
                 e.Type);
        d += b;
        if (e.Stream != 0) continue;
        if ((e.Usage == D3DDECLUSAGE_POSITION || e.Usage == D3DDECLUSAGE_POSITIONT) && e.UsageIndex == 0) {
            L.pos = e.Offset;
            L.rhw = e.Usage == D3DDECLUSAGE_POSITIONT;
            L.posw = e.Type == D3DDECLTYPE_FLOAT4 ? 4 : 3;
        } else if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_D3DCOLOR) {
            L.col = e.Offset;
        } else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 0 && e.Type <= D3DDECLTYPE_FLOAT4) {
            L.uv = e.Offset;
            L.uvn = e.Type + 1;
        }
    }
    d += "}";
    L.desc = d;
    return L;
}

void mul(const D3DMATRIX &a, const D3DMATRIX &b, D3DMATRIX &r) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
}

// describe a draw: vertices given as (base pointer, stride, list of vertex indices)
void describe(const char *kind, const unsigned char *vbase, size_t vlen, UINT stride, const std::vector<int> &idx) {
    Layout L = g_vs ? layout_decl(g_vdecl) : (g_fvf ? layout_fvf(g_fvf) : layout_decl(g_vdecl));
    if (g_vs) L.desc += " +VS";
    R("   layout %s stride %u vs=%p ps=%p", L.desc.c_str(), stride, g_vs, g_ps);
    for (int s = 0; s < 2; s++)
        if (g_tex_bound[s])
            R("   tex%d %s  min/mag/mip=%lu/%lu/%lu addr=%lu/%lu", s, texstr(g_tex_bound[s]).c_str(),
              g_ss[s][D3DSAMP_MINFILTER], g_ss[s][D3DSAMP_MAGFILTER], g_ss[s][D3DSAMP_MIPFILTER],
              g_ss[s][D3DSAMP_ADDRESSU], g_ss[s][D3DSAMP_ADDRESSV]);
    R("   rs: zen=%lu zw=%lu ab=%lu src=%lu dst=%lu at=%lu aref=%lu cull=%lu light=%lu fog=%lu  tss0 cop=%lu ca1=%lu ca2=%lu aop=%lu",
      g_rs[D3DRS_ZENABLE], g_rs[D3DRS_ZWRITEENABLE], g_rs[D3DRS_ALPHABLENDENABLE], g_rs[D3DRS_SRCBLEND],
      g_rs[D3DRS_DESTBLEND], g_rs[D3DRS_ALPHATESTENABLE], g_rs[D3DRS_ALPHAREF], g_rs[D3DRS_CULLMODE],
      g_rs[D3DRS_LIGHTING], g_rs[D3DRS_FOGENABLE], g_tss[0][D3DTSS_COLOROP], g_tss[0][D3DTSS_COLORARG1],
      g_tss[0][D3DTSS_COLORARG2], g_tss[0][D3DTSS_ALPHAOP]);
    if (!vbase || L.pos < 0) {
        R("   (no vertex data: %s)", vbase ? "no position" : "buffer not shadowed");
        return;
    }
    D3DMATRIX wv, wvp;
    mul(g_world, g_view, wv);
    mul(wv, g_proj, wvp);
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    int shown = 0;
    for (size_t k = 0; k < idx.size(); k++) {
        size_t o = (size_t)idx[k] * stride;
        if (o + stride > vlen) continue;
        const unsigned char *v = vbase + o;
        const float *p = (const float *)(v + L.pos);
        float sx, sy;
        if (L.rhw) {
            sx = p[0];
            sy = p[1];
        } else {
            float X = p[0] * wvp._11 + p[1] * wvp._21 + p[2] * wvp._31 + wvp._41;
            float Y = p[0] * wvp._12 + p[1] * wvp._22 + p[2] * wvp._32 + wvp._42;
            float W = p[0] * wvp._14 + p[1] * wvp._24 + p[2] * wvp._34 + wvp._44;
            if (fabsf(W) < 1e-6f) W = 1e-6f;
            sx = g_vp.X + (X / W + 1) * 0.5f * g_vp.Width;
            sy = g_vp.Y + (1 - Y / W) * 0.5f * g_vp.Height;
        }
        if (sx < x0) x0 = sx;
        if (sy < y0) y0 = sy;
        if (sx > x1) x1 = sx;
        if (sy > y1) y1 = sy;
        if (shown < 4) {
            char b[200];
            int n = L.posw == 4 ? snprintf(b, sizeof b, "   v%d pos(%g,%g,%g,%g)", idx[k], p[0], p[1], p[2], p[3])
                                : snprintf(b, sizeof b, "   v%d pos(%g,%g,%g)", idx[k], p[0], p[1], p[2]);
            if (L.col >= 0) n += snprintf(b + n, sizeof b - n, " col %08lx", *(const DWORD *)(v + L.col));
            if (L.uv >= 0) {
                const float *t = (const float *)(v + L.uv);
                n += snprintf(b + n, sizeof b - n, " uv(%g,%g)", t[0], t[1]);
            }
            snprintf(b + n, sizeof b - n, " -> screen(%.2f,%.2f)", sx, sy);
            R("%s", b);
            shown++;
        }
    }
    R("   screen box %.1f,%.1f - %.1f,%.1f (%s, %u vertices looked at)%s", x0, y0, x1, y1,
      L.rhw ? "finished screen positions" : g_vs ? "WRONG if VS uses its own matrices" : "positions x world*view*proj",
      (unsigned)idx.size(), kind);
}

void draw_common() {
    for (int s = 0; s < 2; s++)
        if (g_tex_bound[s]) {
            auto it = g_tex.find(g_tex_bound[s]);
            if (it != g_tex.end()) dump_tex((IDirect3DTexture9 *)g_tex_bound[s], it->second);
        }
}

// ------------------------------------------------------------------ hooks
#define DEF(ret, name, ...)                                    \
    typedef ret(STDMETHODCALLTYPE *P_##name)(__VA_ARGS__);     \
    P_##name o_##name;

DEF(HRESULT, CreateDevice, IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **)
DEF(HRESULT, Reset, IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *)
DEF(HRESULT, Present, IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *)
DEF(HRESULT, CreateTexture, IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *)
DEF(HRESULT, CreateVertexBuffer, IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *)
DEF(HRESULT, CreateIndexBuffer, IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *)
DEF(HRESULT, UpdateSurface, IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *)
DEF(HRESULT, UpdateTexture, IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *)
DEF(HRESULT, StretchRect, IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE)
DEF(HRESULT, ColorFill, IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, D3DCOLOR)
DEF(HRESULT, SetRenderTarget, IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *)
DEF(HRESULT, SetDepthStencilSurface, IDirect3DDevice9 *, IDirect3DSurface9 *)
DEF(HRESULT, BeginScene, IDirect3DDevice9 *)
DEF(HRESULT, EndScene, IDirect3DDevice9 *)
DEF(HRESULT, Clear, IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD)
DEF(HRESULT, SetTransform, IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *)
DEF(HRESULT, SetViewport, IDirect3DDevice9 *, const D3DVIEWPORT9 *)
DEF(HRESULT, SetRenderState, IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD)
DEF(HRESULT, SetTexture, IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *)
DEF(HRESULT, SetTextureStageState, IDirect3DDevice9 *, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD)
DEF(HRESULT, SetSamplerState, IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD)
DEF(HRESULT, SetScissorRect, IDirect3DDevice9 *, const RECT *)
DEF(HRESULT, DrawPrimitive, IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT)
DEF(HRESULT, DrawIndexedPrimitive, IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)
DEF(HRESULT, DrawPrimitiveUP, IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT)
DEF(HRESULT, DrawIndexedPrimitiveUP, IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT)
DEF(HRESULT, CreateVertexDeclaration, IDirect3DDevice9 *, const D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **)
DEF(HRESULT, SetVertexDeclaration, IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *)
DEF(HRESULT, SetFVF, IDirect3DDevice9 *, DWORD)
DEF(HRESULT, SetVertexShader, IDirect3DDevice9 *, IDirect3DVertexShader9 *)
DEF(HRESULT, SetVertexShaderConstantF, IDirect3DDevice9 *, UINT, const float *, UINT)
DEF(HRESULT, SetStreamSource, IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 *, UINT, UINT)
DEF(HRESULT, SetIndices, IDirect3DDevice9 *, IDirect3DIndexBuffer9 *)
DEF(HRESULT, SetPixelShader, IDirect3DDevice9 *, IDirect3DPixelShader9 *)
DEF(HRESULT, SetCursorProperties, IDirect3DDevice9 *, UINT, UINT, IDirect3DSurface9 *)
DEF(HRESULT, TexLockRect2, IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD)
DEF(HRESULT, TexUnlockRect2, IDirect3DTexture9 *, UINT)
DEF(HRESULT, SurfLockRect, IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD)
DEF(HRESULT, SurfUnlockRect, IDirect3DSurface9 *)
DEF(HRESULT, VBLock, IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD)
DEF(HRESULT, VBUnlock, IDirect3DVertexBuffer9 *)
DEF(HRESULT, IBLock, IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD)
DEF(HRESULT, IBUnlock, IDirect3DIndexBuffer9 *)


void log_pp(const char *what, const D3DPRESENT_PARAMETERS *pp, HRESULT r) {
    ee_log("%s: back buffer %ux%u %s count %u ms %d swap %d windowed %d autoDS %d(%s) flags 0x%lx refresh %u interval 0x%x -> %08lx",
           what, pp->BackBufferWidth, pp->BackBufferHeight, fmtn(pp->BackBufferFormat), pp->BackBufferCount,
           pp->MultiSampleType, pp->SwapEffect, pp->Windowed, pp->EnableAutoDepthStencil,
           fmtn(pp->AutoDepthStencilFormat), pp->Flags, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval,
           (unsigned long)r);
}

void hook_device(IDirect3DDevice9 *d);

// ---- upscaler glue (art swap + sharp text, src/ee/up/) -------------------------------------------
void bb_refresh(IDirect3DDevice9 *d) {
    IDirect3DSurface9 *bb = nullptr;
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC dd;
        bb->GetDesc(&dd);
        g_bb = bb;
        g_bbw = dd.Width;
        g_bbh = dd.Height;
        bb->Release();
        g_sc.bb = g_bb;
        g_sc.rt_is_bb = true;
        if (g_upfactor) { g_sc.gw = g_bbw / g_upfactor; g_sc.gh = g_bbh / g_upfactor; }
    }
}
// The interface factor is what the game draws with: an orthographic projection on the full back buffer
// maps the layout width W' = 2 / _11 onto the viewport width: factor = viewport width * _11 / 2.
void up_factor_seen(const D3DMATRIX *m) {
    if (!g_up || !m || m->_34 != 0.0f || m->_44 != 1.0f || !g_sc.rt_is_bb) return;
    if (g_vp.X || g_vp.Y || g_vp.Width != g_bbw || g_vp.Height != g_bbh || !g_bbw) return;
    float f = g_vp.Width * m->_11 / 2.0f;
    int F = (int)lroundf(f);
    if (F < 1 || F > 6 || fabsf(f - F) > 0.02f) return;
    if (!g_upinit) {
        g_upinit = 1;
        g_upfactor = F;
        g_sc.active = F > 1;
        g_sc.gw = g_bbw / F;
        g_sc.gh = g_bbh / F;
        Inner in_;
        up_init(g_ini, F);
        ee_log("upscaler: interface factor %d seen (projection _11 %g, viewport %lux%lu, layout %ux%u) -> up_init, %s",
               F, m->_11, g_vp.Width, g_vp.Height, g_sc.gw, g_sc.gh, up_enabled() ? "ON" : "off");
    } else if (F != g_upfactor) {
        static int nlog = 10;
        if (nlog > 0) { nlog--; ee_log("upscaler: interface factor now %d (started with %d): not followed", F, g_upfactor); }
    }
}
UpDraw mk_draw(D3DPRIMITIVETYPE pt, UINT start, UINT prims, bool indexed, INT basevtx) {
    UpDraw u = {};
    u.fvf = g_fvf;
    u.fvf_valid = g_fvf_current && !g_vs && g_fvf && g_stream0.off == 0;
    u.vb = g_stream0.vb;
    u.stride = g_stream0.stride;
    u.ib = g_ibound;
    u.basevtx = (UINT)basevtx;
    u.pt = pt;
    u.start = start;
    u.prims = prims;
    u.indexed = indexed;
    return u;
}

HRESULT STDMETHODCALLTYPE h_CreateDevice(IDirect3D9 *self, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                         D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
    HRESULT r = o_CreateDevice(self, a, t, w, f, pp, out);
    ee_log("CreateDevice(adapter %u, type %d, hwnd %p, behavior 0x%lx)", a, t, (void *)w, f);
    log_pp("  params", pp, r);
    if (SUCCEEDED(r) && out && *out) {
        g_dev = *out;
        hook_device(*out);
        Lock l;
        bb_refresh(*out);
    }
    return r;
}
HRESULT STDMETHODCALLTYPE h_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp) {
    if (g_inner) return o_Reset(d, pp);
    if (g_up) {
        Lock l;
        Inner in_;
        up_release_companions(d);
        up_before_reset();
    }
    HRESULT r = o_Reset(d, pp);
    log_pp("Reset", pp, r);
    if (SUCCEEDED(r)) { Lock l; bb_refresh(d); }
    return r;
}

void check_trigger() {
    char p[MAX_PATH], q[MAX_PATH];
    ee_beside_program("ee_capture.now", p);
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return;
    for (int n = 1; n < 1000; n++) {
        char nm[64];
        snprintf(nm, sizeof nm, "ee_capture.taken_%d", n);
        ee_beside_program(nm, q);
        if (GetFileAttributesA(q) != INVALID_FILE_ATTRIBUTES) continue;
        snprintf(nm, sizeof nm, "ee_frame_%d.txt", n);
        ee_beside_program(nm, q);
        if (GetFileAttributesA(q) != INVALID_FILE_ATTRIBUTES) continue;
        g_capn = n;
        break;
    }
    snprintf(q, MAX_PATH, "%s", p);
    char *s = strrchr(q, '\\');
    snprintf(s + 1, MAX_PATH - (s + 1 - q), "ee_capture.taken_%d", g_capn);
    MoveFileA(p, q);
    g_capstate = 1;
    ee_log("frame capture %d armed at frame %u", g_capn, g_frame);
}

HRESULT STDMETHODCALLTYPE h_Present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *rg) {
    if (g_inner) return o_Present(d, a, b, w, rg);
    {
        Lock l;
        if (g_rec) {
            R("== Present (end of recorded frame %u, %u draws)", g_frame, g_ndraw);
            fclose(g_rec);
            g_rec = nullptr;
            ee_log("frame capture %d written (%u draws)", g_capn, g_ndraw);
        }
    }
    if (g_up) { Lock l; Inner in_; drain_releases(); up_present(); }
    HRESULT r = o_Present(d, a, b, w, rg);
    Lock l;
    g_frame++;
    InterlockedIncrement(&g_frames);
    if (g_capstate == 1) {
        char nm[64];
        snprintf(nm, sizeof nm, "ee_frame_%d.txt", g_capn);
        char p[MAX_PATH];
        g_rec = fopen(ee_beside_program(nm, p), "w");
        snprintf(nm, sizeof nm, "ee_frame_%d", g_capn);
        ee_beside_program(nm, g_capdir);
        CreateDirectoryA(g_capdir, nullptr);
        for (auto &t : g_tex) t.second.dumped = false;
        g_capstate = 0;
        g_ndraw = 0;
        R("== frame %u (capture %d). State at start: fvf 0x%lx decl %p vs %p ps %p viewport %lu,%lu %lux%lu", g_frame,
          g_capn, g_fvf, g_vdecl, g_vs, g_ps, g_vp.X, g_vp.Y, g_vp.Width, g_vp.Height);
        R("   world %s", mat(g_world).c_str());
        R("   view  %s", mat(g_view).c_str());
        R("   proj  %s", mat(g_proj).c_str());
        R("   textures known: %u", (unsigned)g_tex.size());
    } else if ((g_frame & 31) == 0) {
        check_trigger();
    }
    return r;
}

HRESULT STDMETHODCALLTYPE h_TexLockRect2(IDirect3DTexture9 *t, UINT lv, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f) {
    if (g_inner) return o_TexLockRect2(t, lv, lr, rc, f);
    HRESULT r = o_TexLockRect2(t, lv, lr, rc, f);
    if (!g_dlog) return r;
    Lock l;
    auto it = g_tex.find(t);
    if (it != g_tex.end() && lv == 0) {
        it->second.locks++;
        it->second.last_lock_frame = g_frame;
    }
    if (g_rec)
        R("TexLockRect %s lv%u rect %ld,%ld-%ld,%ld flags 0x%lx", texstr(t).c_str(), lv, rc ? rc->left : -1L,
          rc ? rc->top : -1L, rc ? rc->right : -1L, rc ? rc->bottom : -1L, f);
    return r;
}
HRESULT STDMETHODCALLTYPE h_TexUnlockRect2(IDirect3DTexture9 *t, UINT lv) {
    if (g_inner) return o_TexUnlockRect2(t, lv);
    HRESULT r = o_TexUnlockRect2(t, lv);
    if (g_up && SUCCEEDED(r)) up_texture_written(t, lv);
    return r;
}
DEF(ULONG, TexRelease, IDirect3DTexture9 *)
struct RelNode { SLIST_ENTRY e; void *t; };
SLIST_HEADER g_relq;
void drain_releases() {   // with g_ee_cs held
    PSLIST_ENTRY e = InterlockedFlushSList(&g_relq);
    while (e) {
        RelNode *nd = (RelNode *)e;
        e = e->Next;
        up_texture_released(nd->t);
        g_tex.erase(nd->t);
        _aligned_free(nd);
    }
}
DEF(HRESULT, TexGetSurfaceLevel, IDirect3DTexture9 *, UINT, IDirect3DSurface9 **)
DEF(HRESULT, GetTexture, IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 **)
ULONG STDMETHODCALLTYPE h_TexRelease(IDirect3DTexture9 *t) {
    if (g_inner) return o_TexRelease(t);
    ULONG n = o_TexRelease(t);
    if (n == 0 && g_up) {
        // DXVK itself releases textures through this vtable while holding its own device lock: no lock
        // here (lock-order deadlock with the upscaler, which calls DXVK while holding ours). Queued and
        // handled at the next CreateTexture / Present.
        RelNode *nd = (RelNode *)_aligned_malloc(sizeof(RelNode), MEMORY_ALLOCATION_ALIGNMENT);
        if (nd) { nd->t = t; InterlockedPushEntrySList(&g_relq, &nd->e); }
    }
    return n;
}
HRESULT STDMETHODCALLTYPE h_TexGetSurfaceLevel(IDirect3DTexture9 *t, UINT lv, IDirect3DSurface9 **pp) {
    if (g_inner) return o_TexGetSurfaceLevel(t, lv, pp);
    HRESULT r = o_TexGetSurfaceLevel(t, lv, pp);
    if (g_up && SUCCEEDED(r) && pp && *pp) up_surface_level(t, lv, *pp);
    return r;
}
HRESULT STDMETHODCALLTYPE h_GetTexture(IDirect3DDevice9 *d, DWORD st, IDirect3DBaseTexture9 **pp) {
    if (g_inner) return o_GetTexture(d, st, pp);
    if (g_up && pp) {
        bool known = false;
        IDirect3DBaseTexture9 *gt = up_game_texture(st, &known);
        if (known) {   // what the game bound, not the copy bound for an interface draw
            if (gt) gt->AddRef();
            *pp = gt;
            return D3D_OK;
        }
    }
    return o_GetTexture(d, st, pp);
}

HRESULT STDMETHODCALLTYPE h_SurfLockRect(IDirect3DSurface9 *s, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f) {
    if (g_inner) return o_SurfLockRect(s, lr, rc, f);
    HRESULT r = o_SurfLockRect(s, lr, rc, f);
    if (!g_dlog) return r;
    Lock l;
    IDirect3DTexture9 *t = nullptr;
    if (SUCCEEDED(s->GetContainer(kIID_Tex9, (void **)&t)) && t) {
        auto it = g_tex.find(t);
        if (it != g_tex.end()) {
            it->second.locks++;
            it->second.last_lock_frame = g_frame;
        }
        if (g_rec)
            R("SurfLockRect of %s rect %ld,%ld-%ld,%ld flags 0x%lx", texstr(t).c_str(), rc ? rc->left : -1L,
              rc ? rc->top : -1L, rc ? rc->right : -1L, rc ? rc->bottom : -1L, f);
        t->Release();
    } else if (g_rec) {
        D3DSURFACE_DESC dd;
        s->GetDesc(&dd);
        R("SurfLockRect standalone %p %ux%u %s %s flags 0x%lx", (void *)s, dd.Width, dd.Height, fmtn(dd.Format),
          poolname(dd.Pool), f);
    }
    return r;
}
HRESULT STDMETHODCALLTYPE h_SurfUnlockRect(IDirect3DSurface9 *s) {
    if (g_inner) return o_SurfUnlockRect(s);
    HRESULT r = o_SurfUnlockRect(s);
    if (g_up && SUCCEEDED(r)) up_surface_written(s);
    return r;
}

void hook_texture_vt(IDirect3DTexture9 *t) {
    static bool done;
    if (done) return;
    done = true;
    void **vt = VT(t);
    o_TexLockRect2 = (P_TexLockRect2)vpatch(vt, 19, (void *)h_TexLockRect2);
    o_TexUnlockRect2 = (P_TexUnlockRect2)vpatch(vt, 20, (void *)h_TexUnlockRect2);
    o_TexLockRect = (void *)o_TexLockRect2;
    o_TexUnlockRect = (void *)o_TexUnlockRect2;
    if (g_up) {
        o_TexRelease = (P_TexRelease)vpatch(vt, 2, (void *)h_TexRelease);
        o_TexGetSurfaceLevel = (P_TexGetSurfaceLevel)vpatch(vt, 18, (void *)h_TexGetSurfaceLevel);
    }
    IDirect3DSurface9 *s = nullptr;
    if (SUCCEEDED(t->GetSurfaceLevel(0, &s)) && s) {
        void **sv = VT(s);
        o_SurfLockRect = (P_SurfLockRect)vpatch(sv, 13, (void *)h_SurfLockRect);
        o_SurfUnlockRect = (P_SurfUnlockRect)vpatch(sv, 14, (void *)h_SurfUnlockRect);
        s->Release();
    }
    ee_log("texture + surface vtables hooked (tex vt %p)", (void *)vt);
}

HRESULT STDMETHODCALLTYPE h_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT lv, DWORD u, D3DFORMAT f,
                                          D3DPOOL p, IDirect3DTexture9 **out, HANDLE *sh) {
    if (g_inner) return o_CreateTexture(d, w, h, lv, u, f, p, out, sh);
    HRESULT r = o_CreateTexture(d, w, h, lv, u, f, p, out, sh);
    if (SUCCEEDED(r) && out && *out) {
        hook_texture_vt(*out);
        Lock l;
        if (g_up) { drain_releases(); up_texture_created(*out); }
        TexInfo &ti = g_tex[*out];
        ti = TexInfo{++g_texid, w, h, (*out)->GetLevelCount(), u, f, p, g_frame, 0, 0, false};
        if (g_texlog_left > 0) {
            g_texlog_left--;
            ee_log("CreateTexture T%d %ux%u lv%u %s %s %s at frame %u", ti.id, w, h, ti.levels, fmtn(f),
                   poolname(p), usagestr(u).c_str(), g_frame);
        }
        if (g_rec) R("CreateTexture -> %s", texstr(*out).c_str());
    }
    return r;
}

HRESULT STDMETHODCALLTYPE h_VBLock(IDirect3DVertexBuffer9 *b, UINT off, UINT len, void **pp, DWORD f) {
    if (g_inner) return o_VBLock(b, off, len, pp, f);
    HRESULT r = o_VBLock(b, off, len, pp, f);
    if (SUCCEEDED(r) && g_up && pp && *pp && up_mirrors()) {
        Lock l;
        auto it2 = g_vb.find(b);
        if (it2 != g_vb.end()) {
            BYTE *data = (BYTE *)*pp;
            if (up_buf_lock(b, 0, it2->second.len, off, len, &data, (BYTE *)*pp)) *pp = data;   // the game writes into the mirror
        }
    }
    if (SUCCEEDED(r)) {
        Lock l;
        auto it = g_vb.find(b);
        if (it != g_vb.end()) {
            it->second.lockptr = *pp;
            it->second.lockoff = off;
            it->second.locklen = len ? len : it->second.len - off;
            it->second.locks++;
        }
    }
    return r;
}
HRESULT STDMETHODCALLTYPE h_VBUnlock(IDirect3DVertexBuffer9 *b) {
    if (g_inner) return o_VBUnlock(b);
    {
        Lock l;
        auto it = g_vb.find(b);
        if (it != g_vb.end() && it->second.lockptr) {
            BufInfo &bi = it->second;
            if (bi.shadow.size() == bi.len && bi.lockoff + bi.locklen <= bi.len)
                memcpy(&bi.shadow[bi.lockoff], bi.lockptr, bi.locklen);
            bi.lockptr = nullptr;
        }
    }
    if (g_up) { Lock l; up_buf_unlock(b); }
    return o_VBUnlock(b);
}
HRESULT STDMETHODCALLTYPE h_IBLock(IDirect3DIndexBuffer9 *b, UINT off, UINT len, void **pp, DWORD f) {
    if (g_inner) return o_IBLock(b, off, len, pp, f);
    HRESULT r = o_IBLock(b, off, len, pp, f);
    if (SUCCEEDED(r) && g_up && pp && *pp && up_mirrors()) {
        Lock l;
        auto it2 = g_ib.find(b);
        if (it2 != g_ib.end()) {
            BYTE *data = (BYTE *)*pp;
            if (up_buf_lock(b, (it2 != g_ib.end() && it2->second.ifmt == D3DFMT_INDEX32 ? 4 : 2), it2->second.len, off, len, &data, (BYTE *)*pp)) *pp = data;   // the game writes into the mirror
        }
    }
    if (SUCCEEDED(r)) {
        Lock l;
        auto it = g_ib.find(b);
        if (it != g_ib.end()) {
            it->second.lockptr = *pp;
            it->second.lockoff = off;
            it->second.locklen = len ? len : it->second.len - off;
            it->second.locks++;
        }
    }
    return r;
}
HRESULT STDMETHODCALLTYPE h_IBUnlock(IDirect3DIndexBuffer9 *b) {
    if (g_inner) return o_IBUnlock(b);
    {
        Lock l;
        auto it = g_ib.find(b);
        if (it != g_ib.end() && it->second.lockptr) {
            BufInfo &bi = it->second;
            if (bi.shadow.size() == bi.len && bi.lockoff + bi.locklen <= bi.len)
                memcpy(&bi.shadow[bi.lockoff], bi.lockptr, bi.locklen);
            bi.lockptr = nullptr;
        }
    }
    if (g_up) { Lock l; up_buf_unlock(b); }
    return o_IBUnlock(b);
}

const UINT SHADOW_MAX = 8u << 20;
HRESULT STDMETHODCALLTYPE h_CreateVertexBuffer(IDirect3DDevice9 *d, UINT len, DWORD u, DWORD fvf, D3DPOOL p,
                                               IDirect3DVertexBuffer9 **out, HANDLE *sh) {
    if (g_inner) return o_CreateVertexBuffer(d, len, u, fvf, p, out, sh);
    HRESULT r = o_CreateVertexBuffer(d, len, u, fvf, p, out, sh);
    if (SUCCEEDED(r) && out && *out) {
        static bool vt;
        if (!vt) {
            vt = true;
            o_VBLock = (P_VBLock)vpatch(VT(*out), 11, (void *)h_VBLock);
            o_VBUnlock = (P_VBUnlock)vpatch(VT(*out), 12, (void *)h_VBUnlock);
        }
        Lock l;
        BufInfo &bi = g_vb[*out];
        bi = BufInfo();
        bi.len = len; bi.usage = u; bi.fvf = fvf; bi.pool = p;
        if (g_dlog && len <= SHADOW_MAX) bi.shadow.assign(len, 0);
        static int left = 60;
        if (left > 0) { left--; ee_log("CreateVertexBuffer %u bytes %s fvf 0x%lx %s", len, usagestr(u).c_str(), fvf, poolname(p)); }
        if (g_rec) R("CreateVertexBuffer %p %u bytes %s fvf 0x%lx %s", (void *)*out, len, usagestr(u).c_str(), fvf, poolname(p));
    }
    return r;
}
HRESULT STDMETHODCALLTYPE h_CreateIndexBuffer(IDirect3DDevice9 *d, UINT len, DWORD u, D3DFORMAT f, D3DPOOL p,
                                              IDirect3DIndexBuffer9 **out, HANDLE *sh) {
    if (g_inner) return o_CreateIndexBuffer(d, len, u, f, p, out, sh);
    HRESULT r = o_CreateIndexBuffer(d, len, u, f, p, out, sh);
    if (SUCCEEDED(r) && out && *out) {
        static bool vt;
        if (!vt) {
            vt = true;
            o_IBLock = (P_IBLock)vpatch(VT(*out), 11, (void *)h_IBLock);
            o_IBUnlock = (P_IBUnlock)vpatch(VT(*out), 12, (void *)h_IBUnlock);
        }
        Lock l;
        BufInfo &bi = g_ib[*out];
        bi = BufInfo();
        bi.len = len; bi.usage = u; bi.ifmt = f; bi.pool = p;
        if (g_dlog && len <= SHADOW_MAX) bi.shadow.assign(len, 0);
        static int left = 60;
        if (left > 0) { left--; ee_log("CreateIndexBuffer %u bytes %s %s %s", len, usagestr(u).c_str(), fmtn(f), poolname(p)); }
    }
    return r;
}

std::string surfstr(IDirect3DSurface9 *s) {
    if (!s) return "NULL";
    IDirect3DTexture9 *t = nullptr;
    std::string r;
    if (SUCCEEDED(s->GetContainer(kIID_Tex9, (void **)&t)) && t) {
        r = "level of " + texstr(t);
        t->Release();
    } else {
        D3DSURFACE_DESC dd;
        s->GetDesc(&dd);
        char b[120];
        snprintf(b, sizeof b, "surface %p %ux%u %s %s %s", (void *)s, dd.Width, dd.Height, fmtn(dd.Format),
                 poolname(dd.Pool), usagestr(dd.Usage).c_str());
        r = b;
    }
    return r;
}

HRESULT STDMETHODCALLTYPE h_UpdateSurface(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *t, const POINT *pt) {
    if (g_inner) return o_UpdateSurface(d, s, sr, t, pt);
    Lock l;
    if (g_rec) R("UpdateSurface %s -> %s", surfstr(s).c_str(), surfstr(t).c_str());
    return o_UpdateSurface(d, s, sr, t, pt);
}
HRESULT STDMETHODCALLTYPE h_UpdateTexture(IDirect3DDevice9 *d, IDirect3DBaseTexture9 *s, IDirect3DBaseTexture9 *t) {
    if (g_inner) return o_UpdateTexture(d, s, t);
    Lock l;
    if (g_rec) R("UpdateTexture %s -> %s", texstr(s).c_str(), texstr(t).c_str());
    return o_UpdateTexture(d, s, t);
}
HRESULT STDMETHODCALLTYPE h_StretchRect(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *t, const RECT *tr, D3DTEXTUREFILTERTYPE f) {
    if (g_inner) return o_StretchRect(d, s, sr, t, tr, f);
    Lock l;
    if (g_rec) R("StretchRect %s -> %s filter %d", surfstr(s).c_str(), surfstr(t).c_str(), f);
    return o_StretchRect(d, s, sr, t, tr, f);
}
HRESULT STDMETHODCALLTYPE h_ColorFill(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *rc, D3DCOLOR c) {
    if (g_inner) return o_ColorFill(d, s, rc, c);
    Lock l;
    if (g_rec) R("ColorFill %s color %08lx", surfstr(s).c_str(), c);
    return o_ColorFill(d, s, rc, c);
}
HRESULT STDMETHODCALLTYPE h_SetRenderTarget(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s) {
    if (g_inner) return o_SetRenderTarget(d, i, s);
    Lock l;
    if (g_rec) R("SetRenderTarget %lu %s", i, surfstr(s).c_str());
    if (i == 0) g_sc.rt_is_bb = s && s == g_bb;
    return o_SetRenderTarget(d, i, s);
}
HRESULT STDMETHODCALLTYPE h_SetDepthStencilSurface(IDirect3DDevice9 *d, IDirect3DSurface9 *s) {
    if (g_inner) return o_SetDepthStencilSurface(d, s);
    Lock l;
    if (g_rec) R("SetDepthStencilSurface %s", surfstr(s).c_str());
    return o_SetDepthStencilSurface(d, s);
}
HRESULT STDMETHODCALLTYPE h_BeginScene(IDirect3DDevice9 *d) {
    if (g_inner) return o_BeginScene(d); Lock l; if (g_rec) R("BeginScene"); return o_BeginScene(d); }
HRESULT STDMETHODCALLTYPE h_EndScene(IDirect3DDevice9 *d) {
    if (g_inner) return o_EndScene(d); Lock l; if (g_rec) R("EndScene"); return o_EndScene(d); }
HRESULT STDMETHODCALLTYPE h_Clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *rc, DWORD f, D3DCOLOR c, float z, DWORD s) {
    if (g_inner) return o_Clear(d, n, rc, f, c, z, s);
    Lock l;
    if (g_rec) {
        if (n && rc) R("Clear %lu rects first %ld,%ld-%ld,%ld flags 0x%lx color %08lx", n, rc->x1, rc->y1, rc->x2, rc->y2, f, c);
        else R("Clear all flags 0x%lx color %08lx z %g", f, c, z);
    }
    return o_Clear(d, n, rc, f, c, z, s);
}
HRESULT STDMETHODCALLTYPE h_SetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    if (g_inner) return o_SetTransform(d, t, m);
    Lock l;
    if (m) {
        if (t == D3DTS_WORLD) {
            if (g_rec && memcmp(&g_world, m, sizeof *m)) R("SetTransform WORLD %s", mat(*m).c_str());
            g_world = *m;
            if (g_up) up_set_world(m);
        } else if (t == D3DTS_VIEW) {
            if (g_rec) R("SetTransform VIEW %s", mat(*m).c_str());
            g_view = *m;
            if (g_up) up_set_view(m);
        } else if (t == D3DTS_PROJECTION) {
            if (g_rec) R("SetTransform PROJECTION %s", mat(*m).c_str());
            g_proj = *m;
            if (g_up) { up_factor_seen(m); up_set_projection(m); }
        } else if (g_rec)
            R("SetTransform %d %s", t, mat(*m).c_str());
    }
    return o_SetTransform(d, t, m);
}
HRESULT STDMETHODCALLTYPE h_SetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) {
    if (g_inner) return o_SetViewport(d, v);
    Lock l;
    if (v) {
        g_vp = *v;
        if (g_rec) R("SetViewport %lu,%lu %lux%lu z %g..%g", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ);
    }
    return o_SetViewport(d, v);
}
HRESULT STDMETHODCALLTYPE h_SetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    if (g_inner) return o_SetRenderState(d, s, v);
    Lock l;
    if ((unsigned)s < 256) {
        if (g_rec && g_rs[s] != v) R("SetRenderState %d = %lu (0x%lx)", s, v, v);
        g_rs[s] = v;
    }
    return o_SetRenderState(d, s, v);
}
HRESULT STDMETHODCALLTYPE h_SetTexture(IDirect3DDevice9 *d, DWORD st, IDirect3DBaseTexture9 *t) {
    if (g_inner) return o_SetTexture(d, st, t);
    Lock l;
    if (st < 8) g_tex_bound[st] = t;
    HRESULT r = o_SetTexture(d, st, t);
    if (g_up && SUCCEEDED(r)) { Inner in_; up_set_texture(st, t, t); }
    return r;
}
HRESULT STDMETHODCALLTYPE h_SetTextureStageState(IDirect3DDevice9 *d, DWORD st, D3DTEXTURESTAGESTATETYPE s, DWORD v) {
    if (g_inner) return o_SetTextureStageState(d, st, s, v);
    Lock l;
    if (st < 8 && (unsigned)s < 33) {
        if (g_rec && g_tss[st][s] != v) R("SetTextureStageState %lu %d = %lu", st, s, v);
        g_tss[st][s] = v;
    }
    return o_SetTextureStageState(d, st, s, v);
}
HRESULT STDMETHODCALLTYPE h_SetSamplerState(IDirect3DDevice9 *d, DWORD st, D3DSAMPLERSTATETYPE s, DWORD v) {
    if (g_inner) return o_SetSamplerState(d, st, s, v);
    Lock l;
    if (st < 8 && (unsigned)s < 16) {
        if (g_rec && g_ss[st][s] != v) R("SetSamplerState %lu %d = %lu", st, s, v);
        g_ss[st][s] = v;
    }
    return o_SetSamplerState(d, st, s, v);
}
HRESULT STDMETHODCALLTYPE h_SetScissorRect(IDirect3DDevice9 *d, const RECT *r) {
    if (g_inner) return o_SetScissorRect(d, r);
    Lock l;
    if (g_rec && r) R("SetScissorRect %ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom);
    return o_SetScissorRect(d, r);
}

const char *primname(D3DPRIMITIVETYPE p) {
    switch (p) {
    case D3DPT_POINTLIST: return "POINTLIST";
    case D3DPT_LINELIST: return "LINELIST";
    case D3DPT_LINESTRIP: return "LINESTRIP";
    case D3DPT_TRIANGLELIST: return "TRILIST";
    case D3DPT_TRIANGLESTRIP: return "TRISTRIP";
    case D3DPT_TRIANGLEFAN: return "TRIFAN";
    default: return "PRIM?";
    }
}
UINT nverts(D3DPRIMITIVETYPE p, UINT n) {
    switch (p) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return 2 * n;
    case D3DPT_LINESTRIP: return n + 1;
    case D3DPT_TRIANGLELIST: return 3 * n;
    default: return n + 2;
    }
}

HRESULT STDMETHODCALLTYPE h_DrawPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE p, UINT start, UINT n) {
    if (g_inner) return o_DrawPrimitive(d, p, start, n);
    {
        Lock l;
        if (g_rec) {
            g_ndraw++;
            R("#%u DrawPrimitive %s start %u prims %u | vb %p off %u stride %u", g_ndraw, primname(p), start, n,
              g_stream0.vb, g_stream0.off, g_stream0.stride);
            draw_common();
            std::vector<int> idx;
            UINT nv = nverts(p, n);
            for (UINT k = 0; k < nv && k < 4096; k++) idx.push_back(start + k);
            auto it = g_vb.find(g_stream0.vb);
            const unsigned char *base = nullptr;
            size_t len = 0;
            if (it != g_vb.end() && !it->second.shadow.empty()) {
                base = it->second.shadow.data() + g_stream0.off;
                len = it->second.shadow.size() - g_stream0.off;
            }
            describe("", base, len, g_stream0.stride, idx);
        }
    }
    if (!g_up || !g_upinit) return o_DrawPrimitive(d, p, start, n);
    Lock l;
    Inner in_;
    up_before_draw2(d, mk_draw(p, start, n, false, 0));
    HRESULT r = o_DrawPrimitive(d, p, start, n);
    up_after_draw(d);
    return r;
}
HRESULT STDMETHODCALLTYPE h_DrawIndexedPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE p, INT bv, UINT minv, UINT nv,
                                                 UINT si, UINT n) {
    if (g_inner) return o_DrawIndexedPrimitive(d, p, bv, minv, nv, si, n);
    {
        Lock l;
        if (g_rec) {
            g_ndraw++;
            R("#%u DrawIndexedPrimitive %s base %d min %u num %u startidx %u prims %u | vb %p off %u stride %u ib %p",
              g_ndraw, primname(p), bv, minv, nv, si, n, g_stream0.vb, g_stream0.off, g_stream0.stride, g_ibound);
            draw_common();
            std::vector<int> idx;
            auto ii = g_ib.find(g_ibound);
            if (ii != g_ib.end() && !ii->second.shadow.empty()) {
                bool i32 = ii->second.ifmt == D3DFMT_INDEX32;
                UINT ni = nverts(p, n);
                for (UINT k = 0; k < ni && k < 4096; k++) {
                    size_t o = (size_t)(si + k) * (i32 ? 4 : 2);
                    if (o + (i32 ? 4 : 2) > ii->second.shadow.size()) break;
                    int v = i32 ? *(const int *)&ii->second.shadow[o] : *(const unsigned short *)&ii->second.shadow[o];
                    idx.push_back(v + bv);
                }
            }
            auto it = g_vb.find(g_stream0.vb);
            const unsigned char *base = nullptr;
            size_t len = 0;
            if (it != g_vb.end() && !it->second.shadow.empty() && !idx.empty()) {
                base = it->second.shadow.data() + g_stream0.off;
                len = it->second.shadow.size() - g_stream0.off;
            }
            describe("", base, len, g_stream0.stride, idx);
        }
    }
    if (!g_up || !g_upinit) return o_DrawIndexedPrimitive(d, p, bv, minv, nv, si, n);
    Lock l;
    Inner in_;
    up_before_draw2(d, mk_draw(p, si, n, true, bv));
    HRESULT r = o_DrawIndexedPrimitive(d, p, bv, minv, nv, si, n);
    up_after_draw(d);
    return r;
}
HRESULT STDMETHODCALLTYPE h_DrawPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE p, UINT n, const void *v, UINT stride) {
    if (g_inner) return o_DrawPrimitiveUP(d, p, n, v, stride);
    {
        Lock l;
        if (g_rec) {
            g_ndraw++;
            R("#%u DrawPrimitiveUP %s prims %u stride %u", g_ndraw, primname(p), n, stride);
            draw_common();
            std::vector<int> idx;
            UINT nv = nverts(p, n);
            for (UINT k = 0; k < nv && k < 4096; k++) idx.push_back(k);
            describe(" UP", (const unsigned char *)v, (size_t)nv * stride, stride, idx);
        }
    }
    HRESULT r;
    if (g_up && g_upinit) {
        Lock l;
        Inner in_;
        UpDraw u = mk_draw(p, 0, n, false, 0);
        u.up_vtx = (const BYTE *)v;
        u.stride = stride;
        u.fvf_valid = g_fvf_current && !g_vs && g_fvf;
        up_before_draw2(d, u);
        if (!up_thick_lines(d, p, n, v, stride, g_fvf, u.fvf_valid, &r))
            r = o_DrawPrimitiveUP(d, p, n, v, stride);
        up_after_draw(d);
    } else
        r = o_DrawPrimitiveUP(d, p, n, v, stride);
    Lock l;
    g_stream0.vb = nullptr;  // UP draws reset stream 0
    return r;
}
HRESULT STDMETHODCALLTYPE h_DrawIndexedPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE p, UINT minv, UINT nv, UINT n,
                                                   const void *ix, D3DFORMAT f, const void *v, UINT stride) {
    if (g_inner) return o_DrawIndexedPrimitiveUP(d, p, minv, nv, n, ix, f, v, stride);
    {
        Lock l;
        if (g_rec) {
            g_ndraw++;
            R("#%u DrawIndexedPrimitiveUP %s minv %u numv %u prims %u %s stride %u", g_ndraw, primname(p), minv, nv, n,
              fmtn(f), stride);
            draw_common();
            std::vector<int> idx;
            UINT ni = nverts(p, n);
            for (UINT k = 0; k < ni && k < 4096; k++)
                idx.push_back(f == D3DFMT_INDEX32 ? ((const int *)ix)[k] : ((const unsigned short *)ix)[k]);
            describe(" IUP", (const unsigned char *)v, (size_t)(minv + nv) * stride, stride, idx);
        }
    }
    HRESULT r;
    if (g_up && g_upinit) {
        Lock l;
        Inner in_;
        UpDraw u = mk_draw(p, 0, n, true, 0);
        u.up_vtx = (const BYTE *)v;
        u.up_idx = ix;
        u.idx32 = f == D3DFMT_INDEX32;
        u.stride = stride;
        u.fvf_valid = g_fvf_current && !g_vs && g_fvf;
        up_before_draw2(d, u);
        r = o_DrawIndexedPrimitiveUP(d, p, minv, nv, n, ix, f, v, stride);
        up_after_draw(d);
    } else
        r = o_DrawIndexedPrimitiveUP(d, p, minv, nv, n, ix, f, v, stride);
    Lock l;
    g_stream0.vb = nullptr;
    g_ibound = nullptr;
    return r;
}
HRESULT STDMETHODCALLTYPE h_CreateVertexDeclaration(IDirect3DDevice9 *d, const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **out) {
    if (g_inner) return o_CreateVertexDeclaration(d, e, out);
    HRESULT r = o_CreateVertexDeclaration(d, e, out);
    if (SUCCEEDED(r) && out && *out && e) {
        Lock l;
        std::vector<D3DVERTEXELEMENT9> v;
        for (int i = 0; i < 64; i++) {
            v.push_back(e[i]);
            if (e[i].Stream == 0xff) break;
        }
        g_decl[*out] = v;
        Layout L = layout_decl(*out);
        ee_log("CreateVertexDeclaration %p %s", (void *)*out, L.desc.c_str());
    }
    return r;
}
HRESULT STDMETHODCALLTYPE h_SetVertexDeclaration(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v) {
    if (g_inner) return o_SetVertexDeclaration(d, v);
    Lock l;
    if (g_rec && g_vdecl != v) R("SetVertexDeclaration %p", (void *)v);
    g_vdecl = v;
    g_fvf_current = false;
    return o_SetVertexDeclaration(d, v);
}
HRESULT STDMETHODCALLTYPE h_SetFVF(IDirect3DDevice9 *d, DWORD f) {
    if (g_inner) return o_SetFVF(d, f);
    Lock l;
    if (g_rec && g_fvf != f) R("SetFVF 0x%lx", f);
    g_fvf = f;
    g_fvf_current = true;
    return o_SetFVF(d, f);
}
HRESULT STDMETHODCALLTYPE h_SetVertexShader(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) {
    if (g_inner) return o_SetVertexShader(d, s);
    Lock l;
    if (g_rec && g_vs != s) R("SetVertexShader %p", (void *)s);
    g_vs = s;
    return o_SetVertexShader(d, s);
}
HRESULT STDMETHODCALLTYPE h_SetVertexShaderConstantF(IDirect3DDevice9 *d, UINT r0, const float *c, UINT n) {
    if (g_inner) return o_SetVertexShaderConstantF(d, r0, c, n);
    Lock l;
    if (g_rec && c) {
        R("SetVertexShaderConstantF c%u x%u: %g %g %g %g ...", r0, n, c[0], c[1], c[2], c[3]);
    }
    return o_SetVertexShaderConstantF(d, r0, c, n);
}
HRESULT STDMETHODCALLTYPE h_SetStreamSource(IDirect3DDevice9 *d, UINT s, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride) {
    if (g_inner) return o_SetStreamSource(d, s, vb, off, stride);
    Lock l;
    if (s == 0) g_stream0 = Stream{vb, off, stride};
    else if (g_rec) R("SetStreamSource %u %p off %u stride %u", s, (void *)vb, off, stride);
    return o_SetStreamSource(d, s, vb, off, stride);
}
HRESULT STDMETHODCALLTYPE h_SetIndices(IDirect3DDevice9 *d, IDirect3DIndexBuffer9 *ib) {
    if (g_inner) return o_SetIndices(d, ib);
    Lock l;
    g_ibound = ib;
    return o_SetIndices(d, ib);
}
HRESULT STDMETHODCALLTYPE h_SetPixelShader(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s) {
    if (g_inner) return o_SetPixelShader(d, s);
    Lock l;
    if (g_rec && g_ps != s) R("SetPixelShader %p", (void *)s);
    g_ps = s;
    return o_SetPixelShader(d, s);
}
HRESULT STDMETHODCALLTYPE h_SetCursorProperties(IDirect3DDevice9 *d, UINT x, UINT y, IDirect3DSurface9 *s) {
    if (g_inner) return o_SetCursorProperties(d, x, y, s);
    Lock l;
    if (g_rec) R("SetCursorProperties hotspot %u,%u %s", x, y, surfstr(s).c_str());
    return o_SetCursorProperties(d, x, y, s);
}

void hook_device(IDirect3DDevice9 *d) {
    static bool done;
    if (done) return;
    done = true;
    void **vt = VT(d);
#define H(idx, name) o_##name = (P_##name)vpatch(vt, idx, (void *)h_##name)
    H(16, Reset);
    H(17, Present);
    H(23, CreateTexture);
    H(26, CreateVertexBuffer);
    H(27, CreateIndexBuffer);
    H(30, UpdateSurface);
    H(31, UpdateTexture);
    H(34, StretchRect);
    H(35, ColorFill);
    H(37, SetRenderTarget);
    H(39, SetDepthStencilSurface);
    H(41, BeginScene);
    H(42, EndScene);
    H(43, Clear);
    H(44, SetTransform);
    H(47, SetViewport);
    H(57, SetRenderState);
    H(65, SetTexture);
    H(67, SetTextureStageState);
    H(69, SetSamplerState);
    H(75, SetScissorRect);
    H(81, DrawPrimitive);
    H(82, DrawIndexedPrimitive);
    H(83, DrawPrimitiveUP);
    H(84, DrawIndexedPrimitiveUP);
    H(86, CreateVertexDeclaration);
    H(87, SetVertexDeclaration);
    H(89, SetFVF);
    H(92, SetVertexShader);
    H(94, SetVertexShaderConstantF);
    H(100, SetStreamSource);
    H(104, SetIndices);
    H(107, SetPixelShader);
    H(10, SetCursorProperties);
    if (g_up) H(64, GetTexture);
#undef H
    // D3D9 default states the game may rely on without setting them
    D3DMATRIX I = {};
    I._11 = I._22 = I._33 = I._44 = 1;
    g_world = g_view = g_proj = I;
    g_rs[D3DRS_ZENABLE] = 1; g_rs[D3DRS_ZWRITEENABLE] = 1; g_rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    g_rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO; g_rs[D3DRS_CULLMODE] = D3DCULL_CCW; g_rs[D3DRS_LIGHTING] = 1;
    for (int s = 0; s < 8; s++) {
        g_ss[s][D3DSAMP_MINFILTER] = g_ss[s][D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
        g_ss[s][D3DSAMP_ADDRESSU] = g_ss[s][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
    }
    g_tss[0][D3DTSS_COLOROP] = D3DTOP_MODULATE; g_tss[0][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    g_tss[0][D3DTSS_COLORARG2] = D3DTA_CURRENT; g_tss[0][D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
    IDirect3DSurface9 *bb = nullptr;
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC dd;
        bb->GetDesc(&dd);
        g_vp = D3DVIEWPORT9{0, 0, dd.Width, dd.Height, 0, 1};
        bb->Release();
    }
    ee_log("device vtable %p hooked (34 entries)", (void *)vt);
}

} // namespace

extern "C" void ee_d3d_init(void) {
    InitializeCriticalSection(&cs);
    InitializeSListHead(&g_relq);
    const char *e = getenv("EE_D3DLOG");
    g_dlog = (e && *e == '1') || ee_ini_int("D3DLog", 0) == 1;
    ee_beside_program("uiscale.ini", g_ini);
    e = getenv("EE_UPSCALE");
    g_up = e && *e ? *e != '0' : ee_ini_int("UiUpscale", 1) != 0;
    ee_log("d3dhook: D3DLog=%d UiUpscale=%d", g_dlog, g_up);
    if (g_up) {
        // sharp text: hook the font painters now (before any text is painted); the 3x copies use the
        // records once the upscaler knows the interface factor (up_init)
        int hd = ee_ini_int("HDText", 1), lt = ee_ini_int("ListText", 1);
        if ((e = getenv("EE_HDTEXT")) && *e) hd = atoi(e);
        if (hd) {
            hdtext_set_builtin_plain(lt ? 0 : 1);
            hdtext_set_close_copies(lt ? 1 : 0);
            int r = hdtext_install(nullptr);
            ee_log("hdtext_install -> %d", r);
        }
    }
}

extern "C" void ee_d3d_created(void *p, int ex) {
    if (!g_dlog && !g_up) return;
    IDirect3D9 *d = (IDirect3D9 *)p;
    static bool done;
    if (done) return;
    done = true;
    o_CreateDevice = (P_CreateDevice)vpatch(VT(d), 16, (void *)h_CreateDevice);
    ee_log("IDirect3D9%s vtable %p: CreateDevice hooked", ex ? "Ex" : "", (void *)VT(d));
}

std::string fmtname(D3DFORMAT f) { return fmtn(f); }
