// Interface-texture upscaler.
// Every texture bound while the INTERFACE is drawn (orthographic projection, back buffer as render
// target) gets a proxy-owned "shadow" texture F times larger (F = real / game size, normally 3),
// filled from the original's level 0 with an edge-directed scaler (xBRZ). The shadow is bound
// instead of the original for interface draws only; world draws keep the original. The shadow is
// refreshed lazily at the next interface use after the original was written (Unlock/CopyRects/
// UpdateTexture) and only if the content hash changed.
// Off: uiscale.ini [uiscale] Upscale=0, or D3D8PROXY_UPSCALE=0 (overrides the ini).
#pragma once
#include "compat9.h"
#include <cstdint>

void up_init(const char *ini_path, int factor);
bool up_enabled();
bool up_mirrors();   // vertex/index buffer mirrors wanted (Upscale=1, or PixelFix=1 with Upscale=0)
// device hooks (game-side pointers are wrappers, real-side pointers are the next d3d9's objects)
void up_set_texture(DWORD stage, IDirect3DBaseTexture9 *game_wrapper, IDirect3DBaseTexture9 *real);
IDirect3DBaseTexture9 *up_game_texture(DWORD stage, bool *known);   // what the game bound (wrapper)
void up_set_projection(const D3DMATRIX *m);
void up_present();
// Device::Reset: Reset unbinds every texture; release the stage references the upscaler holds.
void up_before_reset();
void up_texture_created(void *tex_wrapper);
void up_texture_released(void *tex_wrapper);   // the game released its last reference
void up_surface_level(void *tex_wrapper, UINT level, IDirect3DSurface9 *real_surface);
void up_surface_written(IDirect3DSurface9 *real_surface);
void up_texture_written(void *tex_wrapper, UINT level);
// Vertex/index buffer mirrors: the game writes into a CPU copy (cached memory), copied to the real
// buffer at Unlock, so interface draws can be checked for 1:1 texel-to-pixel mapping. Stretched,
// scaled or rotated interface draws keep the original texture (a stretched single texel of an xBRZ
// copy shows bands of its sub-texels).
bool up_buf_lock(void *wrapper, int elem /*0 = vertex buffer, 2/4 = index size*/, UINT size, UINT off, UINT len, BYTE **data, BYTE *real_ptr);
void up_buf_unlock(void *wrapper);
void up_buf_released(void *wrapper);           // the buffer is gone: free its mirror
void up_set_world(const D3DMATRIX *m);
struct UpDraw {   // what the device wrapper knows about the current draw
    DWORD fvf;              // current SetVertexShader value (0 or shader handle = unknown layout)
    bool fvf_valid;
    void *vb; UINT stride; // stream 0 wrapper + stride
    void *ib; UINT basevtx;
    const BYTE *up_vtx; const void *up_idx; bool idx32;   // DrawPrimitiveUP / DrawIndexedPrimitiveUP
    D3DPRIMITIVETYPE pt;
    UINT start;            // first index (indexed) or first vertex
    UINT prims;
    bool indexed;
};
void up_before_draw2(IDirect3DDevice9 *real_dev, const UpDraw &d);
void up_after_draw(IDirect3DDevice9 *real_dev);   // puts back states changed for the last draw
// After up_before_draw2 of an interface draw: 1 = first vertex sits at x.5 game px (Direct3D half-pixel
// convention), 0 = whole pixel, -1 = unknown (no vertex mirror / shader / upscaler off).
void up_half_pixel(int *hx, int *hy);
void up_set_view(const D3DMATRIX *m);
bool up_copy_rects(IDirect3DDevice9 *real_dev, IDirect3DSurface9 *ws, IDirect3DSurface9 *rs, const RECT *rects, UINT n,
                   IDirect3DSurface9 *wd, IDirect3DSurface9 *rd, const POINT *pts, HRESULT *hr);   // ThickLines only acts while VIEW has unit scale
// ThickLines: an interface line draw from user memory is drawn as 1-game-px-wide quads instead.
// true = drawn here (*hr = result), false = caller draws as usual.
bool up_thick_lines(IDirect3DDevice9 *real_dev, D3DPRIMITIVETYPE pt, UINT n, const void *data, UINT stride, DWORD fvf,
                    bool fvf_valid, HRESULT *hr);

// The scaler, behind one function so it can be swapped: src w x h ARGB (BGRA bytes), dst (w*f) x (h*f).
void up_scale_image(const uint32_t *src, int w, int h, uint32_t *dst, int f, bool has_alpha);
// Smooth (Lanczos-3) enlargement for photographic art, premultiplied alpha; tiled axes wrap.
void up_smooth_image(const uint32_t *src, int w, int h, uint32_t *dst, int f);

// DragText: render-target companions (see upscale.cpp). up_set_render_target returns the surface to
// really set (a 3x companion for the drag sprite target, else rr); call before the real SetRenderTarget.
IDirect3DSurface9 *up_set_render_target(IDirect3DDevice9 *real_dev, IDirect3DSurface9 *rr, IDirect3DSurface9 *rz);
IDirect3DSurface9 *up_game_render_target(IDirect3DSurface9 *real);   // GetRenderTarget: companion -> game surface (refs moved)
void up_companion_failed();                                          // the real SetRenderTarget refused the companion
bool up_rt_companion(UINT *real_w, UINT *real_h);                     // current target is a companion
void up_viewport_in(D3DVIEWPORT9 *v);
void up_viewport_out(D3DVIEWPORT9 *v);
const D3DRECT *up_clear_rects(DWORD n, const D3DRECT *rects, D3DRECT *buf, DWORD bufn);
void up_release_companions(IDirect3DDevice9 *real_dev);              // Device::Reset, before up_before_reset
