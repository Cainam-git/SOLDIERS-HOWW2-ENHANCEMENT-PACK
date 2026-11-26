#!/usr/bin/env python3
"""Add the line "ENHANCED EDITION" to the 4x menu logo art (uiscale_hd/8d8731b9c852d2d2_512x128_x4.raw).

  python3 -I make_logo_line.py <logo_x4.raw (the 1.0 art file)> <steam logo.png> <out_dir>

The game draws the WHOLE 512x128 logo texture (interface/logo_small.tga, 350x104 at the top-left of a
512x128 texture) into one rectangle; the art ends with the subtitle at texture row ~98 (4x row 391), so
rows 392..511 of the 4x art are empty and inside the drawn rectangle. The new line goes there.

Geometry: taken from Steam's official logo.png (640x243): "ENHANCED EDITION" relative to
"HEROES OF WORLD WAR II" (gap, cap height, width, centre, the x place of every letter), scaled by the
size of our subtitle relative to Steam's subtitle (horizontal and vertical separately, so the relation
of the two lines is the official one in the texture AND on screen, where the game squashes the whole
logo by the same factors).
Typeface: Nimbus Roman Bold (URW, the Times Bold clone of the PostScript base fonts; AGPL/OFL-style
free licence, /usr/share/fonts/gsfonts) = best shape match to Steam's line among the bold serifs on
this machine (mean shape error per letter: Nimbus Roman Bold 0.080, Liberation Serif Bold 0.115,
Noto Serif Bold 0.122, Noto Serif Display Bold 0.135, DejaVu Serif Bold 0.139).
Finish: the subtitle's own finish measured on the art: white fill with a darker 1.5 px inner edge,
1.25 px dark outline (grey 20), soft black shadow offset (+3,+4) 4x px, blur sigma 1.5.
Output: <out_dir>/8d8731b9c852d2d2_512x128_x4.raw (same name: the key is the game's unchanged 1x
texture) + previews.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgops import blur, dist_to, shift  # noqa: E402

FONT = '/usr/share/fonts/gsfonts/NimbusRoman-Bold.otf'
TEXT = 'ENHANCED EDITION'
SS = 4                 # supersampling of the 4x grid while drawing
NAME = '8d8731b9c852d2d2_512x128_x4.raw'


def coverage_edges(prof):
    p = prof / np.percentile(prof[prof > 0], 90)
    idx = np.nonzero(p > 0.05)[0]
    a, b = idx[0], idx[-1]
    return a + 1 - min(p[a], 1), b + min(p[b], 1)


def steam_geometry(path):
    s = np.asarray(Image.open(path).convert('RGBA')).astype(np.float64)
    ink = s[..., 3] / 255 * (1 - s[..., :3].mean(2) / 255)
    out = {}
    for nm, (r0, r1) in (('sub', (160, 195)), ('ee', (195, 240))):
        band = ink[r0:r1]
        y0, y1 = coverage_edges(band.sum(1))
        cols = np.nonzero(band.max(0) > 0.5)[0]
        out[nm] = dict(y0=y0 + r0, y1=y1 + r0, x0=cols[0], x1=cols[-1] + 1)
    # letters of the EE line: column groups (threshold 0.3)
    band = ink[195:240]
    cols = np.nonzero(band.max(0) > 0.3)[0]
    groups, st, pv = [], cols[0], cols[0]
    for c in cols[1:]:
        if c > pv + 1:
            groups.append((st, pv + 1))
            st = c
        pv = c
    groups.append((st, pv + 1))
    letters = [ch for ch in TEXT if ch != ' ']
    assert len(groups) == len(letters), (len(groups), groups)
    out['letters'] = list(zip(letters, groups))
    return out


def our_subtitle(a):
    f = (a[..., 3] / 255) * np.clip((a[..., :3].mean(2) - 60) / 150, 0, 1)
    band = f[330:400]
    y0, y1 = coverage_edges(band.sum(1))
    cols = np.nonzero(band.max(0) > 0.5)[0]
    return dict(y0=y0 + 330, y1=y1 + 330, x0=cols[0], x1=cols[-1] + 1)


def finish(F, ow=1.25, sdx=3, sdy=4, ssig=1.5, sk=0.75, edge_lum=150, fill_lum=255, out_lum=20, ss=1):
    """F = fill coverage (0..1) on a grid ss times finer than the 4x art. Returns (lum, alpha), straight alpha."""
    Fb = F > 0.5
    dout = dist_to(Fb, int(np.ceil((ow + 1) * ss)) + 1) / ss
    din = dist_to(~Fb, int(3 * ss) + 1) / ss
    B = np.maximum(np.clip(ow + 0.5 - dout, 0, 1), F)            # fill + outline
    S = np.clip(sk * blur(shift(B, int(round(sdy * ss)), int(round(sdx * ss))), ssig * ss), 0, 1)
    t = np.clip(din - 0.5, 0, 1.5) / 1.5
    fl = edge_lum + (fill_lum - edge_lum) * t
    lum = out_lum + (fl - out_lum) * np.clip(F, 0, 1)
    A = B + S * (1 - B)
    L = np.where(A > 0, lum * B / np.maximum(A, 1e-6), 0)
    return L.astype(np.float32), A.astype(np.float32)


def main():
    src, steam, outdir = sys.argv[1:4]
    a = np.fromfile(src, np.uint8).reshape(512, 2048, 4)
    g = steam_geometry(steam)
    o = our_subtitle(a.astype(np.float64))
    sx = (o['x1'] - o['x0']) / (g['sub']['x1'] - g['sub']['x0'])
    sy = (o['y1'] - o['y0']) / (g['sub']['y1'] - g['sub']['y0'])
    cx_s = (g['sub']['x0'] + g['sub']['x1']) / 2
    cx_o = (o['x0'] + o['x1']) / 2

    def X(xs):
        return cx_o + (xs - cx_s) * sx

    top = o['y1'] + (g['ee']['y0'] - g['sub']['y1']) * sy
    cap = (g['ee']['y1'] - g['ee']['y0']) * sy
    print('subtitle Steam %s | ours %s | scale x %.4f y %.4f' % (g['sub'], o, sx, sy))
    print('new line: top %.2f cap %.2f bottom %.2f, x %.1f..%.1f (width %.1f)' % (
        top, cap, top + cap, X(g['ee']['x0']), X(g['ee']['x1']), X(g['ee']['x1']) - X(g['ee']['x0'])))

    # glyphs: cap height from 'E' at a big size
    big = 1000
    fnt = ImageFont.truetype(FONT, big)
    eb = fnt.getbbox('E', anchor='ls')
    capbig = -eb[1]
    size = cap * SS / capbig * big            # font size on the supersampled grid
    fnt = ImageFont.truetype(FONT, int(round(size)))
    k = cap * SS / -fnt.getbbox('E', anchor='ls')[1]     # residual from rounding the size
    # uniform horizontal factor: target letter widths / glyph ink widths (mean)
    ratios = []
    for ch, (c0, c1) in g['letters']:
        bb = fnt.getbbox(ch, anchor='ls')
        ratios.append((X(c1) - X(c0)) * SS / ((bb[2] - bb[0]) * k))
    hs = float(np.median(ratios))
    print('horizontal factor vs the font: %.3f (letters %.3f..%.3f)' % (hs, min(ratios), max(ratios)))

    # band on the supersampled grid: 4x rows 392..512, columns 0..2048
    R0, R1 = 392, 512
    H, W = (R1 - R0) * SS, 2048 * SS
    F = np.zeros((H, W), np.float32)
    base = (top + cap - R0) * SS               # baseline on the band grid
    for ch, (c0, c1) in g['letters']:
        bb = fnt.getbbox(ch, anchor='ls')
        gw, gh = bb[2] - bb[0] + 8, bb[3] - bb[1] + 8
        im = Image.new('L', (gw, gh))
        ImageDraw.Draw(im).text((4 - bb[0], 4 - bb[1]), ch, font=fnt, fill=255, anchor='ls')
        nw, nh = max(1, round(gw * hs * k)), max(1, round(gh * k))
        im = im.resize((nw, nh), Image.LANCZOS)
        gl = np.asarray(im).astype(np.float32) / 255
        # place: ink centre x on the letter's target centre; baseline on the line's baseline
        cxt = (X(c0) + X(c1)) / 2 * SS
        inkx = np.nonzero(gl.max(0) > 0.5)[0]
        gcx = (inkx[0] + inkx[-1] + 1) / 2
        x0 = int(round(cxt - gcx))
        y0 = int(round(base - (4 - bb[1]) * k))   # glyph origin row 4-bb[1] (baseline) scaled
        F[y0:y0 + nh, x0:x0 + nw] = np.maximum(F[y0:y0 + nh, x0:x0 + nw], gl[:H - y0, :W - x0][:nh, :nw])
    F = np.clip(F, 0, 1)
    L, A = finish(F, ss=SS)
    # box down to the 4x grid (alpha-weighted colour)
    def box(m):
        return m.reshape(H // SS, SS, W // SS, SS).mean((1, 3))
    A4 = box(A)
    L4 = np.where(A4 > 0, box(L * A) / np.maximum(A4, 1e-6), 0)
    out = a.copy()
    band = out[R0:R1]
    assert band[..., 3].max() <= 3, 'rows 392..511 of the art are not empty'
    nb = np.zeros_like(band)
    lum8 = np.clip(L4 + 0.5, 0, 255).astype(np.uint8)
    nb[..., 0] = nb[..., 1] = nb[..., 2] = lum8
    nb[..., 3] = np.clip(A4 * 255 + 0.5, 0, 255).astype(np.uint8)
    nb[nb[..., 3] == 0] = 0
    out[R0:R1] = nb
    ys = np.nonzero(nb[..., 3].max(1) > 2)[0]
    print('new pixels in 4x rows %d..%d (texture rows %.1f..%.1f of 128)' % (R0 + ys[0], R0 + ys[-1], (R0 + ys[0]) / 4, (R0 + ys[-1] + 1) / 4))
    os.makedirs(outdir, exist_ok=True)
    out.tofile(os.path.join(outdir, NAME))
    rgba = out[..., [2, 1, 0, 3]]
    Image.fromarray(rgba).save(os.path.join(outdir, 'logo_x4_with_line.png'))
    for nm, col in (('blue', (40, 60, 90)), ('sky', (200, 205, 210))):
        bg = Image.new('RGBA', (2048, 512), col + (255,))
        bg.alpha_composite(Image.fromarray(rgba))
        bg.crop((0, 0, 1500, 512)).save(os.path.join(outdir, 'preview_on_%s.png' % nm))


if __name__ == '__main__':
    main()
