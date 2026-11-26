// hdtext: sharp interface text for Soldiers: Heroes of World War II.
// The game paints text glyph by glyph on the CPU into 1x textures (eFontMono / eFontHQ vtable
// slot 11). hdtext hooks those two slots in place, lets the original painter run (the game
// texture stays byte-exact) and records every glyph. Whoever builds the 3x copy of a texture
// (the proxy's upscaler) calls hdtext_overlay() afterwards: every glyph whose 1x pixels are
// still exactly what the painter left is redrawn into the 3x copy from the HD glyph pack
// (hdtext_fonts.bin, made by tools/fontdump.py). Stale text is left alone.
#pragma once
#include <cstdint>

extern "C" {
// Install the two slot hooks and load the pack. pack_path = hdfonts.bin (NULL: "hdtext_fonts.bin"
// next to the module that contains this code). Returns number of fonts loaded, <0 on error
// (then nothing is hooked). Safe to call once; later calls return the first result.
int hdtext_install(const char *pack_path);

// key   = the IDirect3DTexture9* the GAME holds (the proxy's texture wrapper; = container of the
//         surface at eSurface+0x18).
// src1x = level 0 of that texture as the game wrote it (A8R8G8B8, w x h, pitch in bytes)
// dst3x = the 3x copy (A8R8G8B8, 3w x 3h, pitch in bytes), already filled by the upscaler.
// Returns the number of glyphs drawn (0 = this texture holds no live text).
int hdtext_overlay(void *key, const uint32_t *src1x, int pitch1x, uint32_t *dst3x, int pitch3x,
                   int w, int h);

// Best quality with a smoothing upscaler (xBRZ): copy level 0, hdtext_strip() the copy (puts the
// background back where live glyphs are), upscale the stripped copy, then hdtext_overlay_text()
// with the ORIGINAL level 0 as src1x (draws only the HD glyphs, the background stays smooth).
int hdtext_strip(void *key, uint32_t *img1x, int pitch, int w, int h);
int hdtext_overlay_text(void *key, const uint32_t *src1x, int pitch1x, uint32_t *dst3x, int pitch3x,
                        int w, int h);

// True if the texture has glyph records (cheap; lets the caller skip textures without text).
int hdtext_has_text(void *key);

// Drop every record of a texture (the game re-created the texture behind this key).
void hdtext_forget(void *key);

// Before hdtext_install: 0 = the built-in plain-font list (main_hq_11) is not used (the caller
// handles the draw-time doubles of that font). hdtext_plain.txt next to the pack still wins.
void hdtext_set_builtin_plain(int on);
// 1 = a glyph repeated 1 px away by a newer record (emboss/shadow pass) is drawn 1 real px away (not 3)
void hdtext_set_close_copies(int on);
// Carry live glyph records across a Device::CopyRects (surfaces = the game-held wrappers; src1x =
// whole source level wS x hS; before/after = destination region X0,Y0 RW x RH around the copy;
// source rect sx0..sx1, sy0..sy1 lands at +dx,+dy). Returns records carried.
int hdtext_carry_copy(void *src_surface, const uint32_t *src1x, int pitchS, int wS, int hS, void *dst_surface,
                      const uint32_t *before, const uint32_t *after, int X0, int Y0, int RW, int RH, int sx0,
                      int sy0, int sx1, int sy1, int dx, int dy);
}
