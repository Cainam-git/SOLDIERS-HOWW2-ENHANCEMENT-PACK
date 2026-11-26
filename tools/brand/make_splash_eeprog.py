#!/usr/bin/env python3
"""4K splash for the ENHANCED EDITION PROGRAM (their resource/interface/splash.bmp: same logo as GOG, but nine
legal lines), with the line "ENHANCED EDITION": their nine lines (LINES/LINE_TOP), the legal block moved down by
LEGAL_DY rows (the new line would touch it otherwise) and the whole picture moved up by half of that (stays centred).
Regions may lie outside the 2400x1800 frame (the d3d9 painter, src/ee/splash4k.cpp, paints the whole window).

  python3 -I make_splash_eeprog.py <their splash.bmp> <logo_2048x512.png> <outdir> <steam logo.png>

How the picture is made: a crisp 3x version of the start-up splash (resource/interface/splash.bmp, 800x600,
drawn centred in the 1280x720 game window and stretched 3x by the pack).

- Logo + "HEROES OF WORLD WAR II": the game's own 2048x512 logo texture (dumped by the dll with
  D3D8PROXY_UPDUMPALL=1, saved as an RGBA PNG), composited on black and resampled (Lanczos) to
  the splash's size and place. Scale/offset were fitted against splash.bmp (see FIT below).
- The five copyright lines: re-set in Liberation Serif (metric-compatible with Times New Roman, SIL OFL
  1.1) at 42 px = 3x the original 14 px, white on black, slightly emboldened to the original's weight. Every WORD is placed where the original word
  is (fitted against splash.bmp at 1/3-pixel steps), so the layout is the original's.
- Output: <outdir>/splash_3840x2160.bin (format in splash_hd.cpp) + preview PNGs.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

F = 3
FONT = '/usr/share/fonts/liberation/LiberationSerif-Regular.ttf'
FONT_PX = 14 * F
# FIT (search over scale and offset, mean abs error vs splash.bmp on 0..255): logo rows 0..380 of the 2048x512
# texture at scale 0.5140 (x3 = 1.542), top-left at 3x (-1,451): err 2.32; subtitle rows 380..512 at
# 0.5135 (x3 = 1.5405), top-left at 3x (1,1037): err 1.93.
LOGO = dict(rows=(0, 380), s3=0.5140 * 3, at=(-1, 451))
SUB = dict(rows=(380, 512), s3=0.5135 * 3, at=(1, 1037))
LINES = [
    '\u00a9 2004 Best Way Company, 1C Company and The Codemasters Software Company Limited ("Codemasters"). All rights reserved.',
    '"Codemasters"\u00ae and the Codemasters logo are registered trademarks owned by Codemasters.',
    '"Soldiers"\u2122, "Heroes of World War II"\u2122 and "GENIUS AT PLAY"\u2122 are trademarks of Codemasters.',
    'All other copyrights or trademarks are the property of their respective owners.',
    'Developed by Best Way Company. Published by Codemasters.',
    'This product contains software technology licensed from GameSpy Industries, Inc.  (c) 1999-2003 GameSpy Industries Inc.',
    'GameSpy and the "Powered by GameSpy" design are trademarks of GameSpy Industries, Inc.  All rights reserved.',
    'Unauthorised copying, adaptation, rental, lending, re-sale, arcade use,charging for use, broadcast, cable transmission, public performance,',
    'distribution or extraction of this product or any trademark or copyright work that forms part of this product is prohibited.',
]
LINE_TOP = [429, 446, 463, 479, 496, 513, 530, 547, 563]   # first ink row of each line in their splash.bmp
LEGAL_DY = 50      # 1x rows the legal block moves down (gap to the new line = 40 rows)
LIFT = LEGAL_DY * 3 // 2   # 3x rows the whole picture moves up (stays centred)
PAD = 3 * LEGAL_DY       # extra canvas rows below the frame


def paste_logo(can, Hc, p):
    r0, r1 = p['rows']
    sub = Image.fromarray(Hc[r0:r1])
    w, h = round(sub.width * p['s3']), round(sub.height * p['s3'])
    r = np.asarray(sub.resize((w, h), Image.LANCZOS)).astype(np.float32)
    X, Y = p['at']
    x0, y0, x1, y1 = max(0, X), max(0, Y), min(can.shape[1], X + w), min(can.shape[0], Y + h)
    can[y0:y1, x0:x1] = np.maximum(can[y0:y1, x0:x1], r[y0 - Y:y1 - Y, x0 - X:x1 - X])


def word_img(font, word):
    """word rendered at 3x: float array, origin = (pad, baseline at pad + asc)"""
    asc, desc = font.getmetrics()
    pad = 6
    w = int(font.getlength(word)) + 2 * pad + 6
    im = Image.new('L', (w, asc + desc + 2 * pad))
    ImageDraw.Draw(im).text((pad, pad + asc), word, fill=255, font=font, anchor='ls')
    return np.asarray(im).astype(np.float32), pad, pad + asc


def fit_text(S, font):
    """per word: best 3x (x of origin, y of baseline) against the 1x original"""
    out = []
    for li, (line, top) in enumerate(zip(LINES, LINE_TOP)):
        words = line.split()
        band = S[top - 3: top + 16].astype(np.float32)
        if li + 1 < len(LINE_TOP):
            band = S[top - 3: min(top + 16, LINE_TOP[li + 1] - 1)].astype(np.float32)   # 1x rows top-3 .. top+15 (neighbour lines start 4 rows away)
        placed = []
        # ink columns of the original line
        ink = np.nonzero(band.max(0) > 30)[0]
        x_pred = 3 * ink[0]
        space = font.getlength(' ')
        for wi, word in enumerate(words):
            wim, ox, oy = word_img(font, word)
            best = None
            for dy3 in range(3):
                for dx3 in range(3):
                    # shift by (dx3,dy3) then box-downsample by 3
                    hh = (wim.shape[0] + dy3 + 2) // 3 * 3
                    ww = (wim.shape[1] + dx3 + 2) // 3 * 3
                    big = np.zeros((hh, ww), np.float32)
                    big[dy3:dy3 + wim.shape[0], dx3:dx3 + wim.shape[1]] = wim
                    small = big.reshape(hh // 3, 3, ww // 3, 3).mean((1, 3))
                    ys, xs = np.nonzero(small > 8)
                    if len(xs) == 0:
                        continue
                    sy0, sy1, sx0, sx1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
                    patch = small[sy0:sy1, sx0:sx1]
                    # candidate 1x placement of the patch: origin x (3x) = x3 -> patch left 1x = (x3 - ox + dx3)/3 + sx0
                    for X1 in range(int(x_pred / 3) - 8 - sx0, int(x_pred / 3) + 8 - sx0 + 1):
                        for Y1 in range(-7, 3):
                            px, py = X1 + sx0, Y1 + sy0
                            if px < 0 or px + patch.shape[1] > 800 or py < 0 or py + patch.shape[0] > band.shape[0]:
                                continue
                            ref = band[py:py + patch.shape[0], px:px + patch.shape[1]]
                            e = float(((ref - patch) ** 2).mean())
                            if best is None or e < best[0]:
                                # 3x origin: big origin at (ox+dx3, oy+dy3) in big coords; big top-left at 3*X1
                                best = (e, 3 * X1 + ox + dx3, 3 * (top - 3 + Y1) + oy + dy3)
            placed.append((word, best[1], best[2], best[0]))
            x_pred = best[1] + font.getlength(word) + space
        # one baseline per line: the median of the words
        by = int(np.median([p[2] for p in placed]))
        out.append([(w, x, by, e) for (w, x, _, e) in placed])
    return out


EE_FONT = '/usr/share/fonts/gsfonts/NimbusRoman-Bold.otf'   # same face as the menu logo line
EE_TEXT = 'ENHANCED EDITION'


def add_ee_line(out, steam_png, SS=4):
    """The line under "HEROES OF WORLD WAR II", flat white like the splash's subtitle. Geometry from
    Steam's logo.png (gap, cap height, width, centre and every letter's place, relative to the subtitle),
    scaled by our subtitle's size against Steam's (x and y separately), exactly as for the menu logo."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from make_logo_line import steam_geometry, coverage_edges
    g = steam_geometry(steam_png)
    grey = out.max(2).astype(np.float64) / 255
    band = grey[1040:1160]                     # the subtitle (region y 1062..1130)
    y0, y1 = coverage_edges(band.sum(1))
    cols = np.nonzero(band.max(0) > 0.5)[0]
    o = dict(y0=y0 + 1040, y1=y1 + 1040, x0=cols[0], x1=cols[-1] + 1)
    sx = (o['x1'] - o['x0']) / (g['sub']['x1'] - g['sub']['x0'])
    sy = (o['y1'] - o['y0']) / (g['sub']['y1'] - g['sub']['y0'])
    cxs, cxo = (g['sub']['x0'] + g['sub']['x1']) / 2, (o['x0'] + o['x1']) / 2
    X = lambda xs: cxo + (xs - cxs) * sx
    top = o['y1'] + (g['ee']['y0'] - g['sub']['y1']) * sy
    cap = (g['ee']['y1'] - g['ee']['y0']) * sy
    print('splash subtitle %s, scale x %.4f y %.4f; new line top %.1f cap %.1f bottom %.1f x %.1f..%.1f' % (
        o, sx, sy, top, cap, top + cap, X(g['ee']['x0']), X(g['ee']['x1'])))
    big = ImageFont.truetype(EE_FONT, 1000)
    size = cap * SS / -big.getbbox('E', anchor='ls')[1] * 1000
    fnt = ImageFont.truetype(EE_FONT, int(round(size)))
    k = cap * SS / -fnt.getbbox('E', anchor='ls')[1]
    ratios = []
    for ch, (c0, c1) in g['letters']:
        bb = fnt.getbbox(ch, anchor='ls')
        ratios.append((X(c1) - X(c0)) * SS / ((bb[2] - bb[0]) * k))
    hs = float(np.median(ratios))
    R0, R1 = int(top) - 16, int(top + cap) + 24
    H, W = (R1 - R0) * SS, out.shape[1] * SS
    F = np.zeros((H, W), np.float32)
    base = (top + cap - R0) * SS
    for ch, (c0, c1) in g['letters']:
        bb = fnt.getbbox(ch, anchor='ls')
        gw, gh = bb[2] - bb[0] + 8, bb[3] - bb[1] + 8
        im = Image.new('L', (gw, gh))
        ImageDraw.Draw(im).text((4 - bb[0], 4 - bb[1]), ch, font=fnt, fill=255, anchor='ls')
        nw, nh = max(1, round(gw * hs * k)), max(1, round(gh * k))
        gl = np.asarray(im.resize((nw, nh), Image.LANCZOS)).astype(np.float32) / 255
        inkx = np.nonzero(gl.max(0) > 0.5)[0]
        x0 = int(round((X(c0) + X(c1)) / 2 * SS - (inkx[0] + inkx[-1] + 1) / 2))
        y0 = int(round(base - (4 - bb[1]) * k))
        F[y0:y0 + nh, x0:x0 + nw] = np.maximum(F[y0:y0 + nh, x0:x0 + nw], gl)
    F = np.clip(F, 0, 1).reshape(H // SS, SS, W // SS, SS).mean((1, 3))
    # the subtitle's own white (brightest level of its letters)
    white = float(np.percentile(out[int(o['y0']):int(o['y1']), o['x0']:o['x1']].max(2), 99.5))
    seg = out[R0:R1].astype(np.float32)
    assert seg.max() <= 2, 'the rows under the subtitle are not empty'
    out[R0:R1] = np.clip(np.maximum(seg, F[..., None] * white) + 0.5, 0, 255).astype(np.uint8)
    return out


def main():
    bmp, logo, outdir = sys.argv[1:4]
    S = np.asarray(Image.open(bmp).convert('RGB'))
    assert S.shape == (600, 800, 3)
    H = np.asarray(Image.open(logo).convert('RGBA')).astype(np.float32)
    Hc = np.clip(H[..., :3] * (H[..., 3:] / 255.0) + 0.5, 0, 255).astype(np.uint8)
    can = np.zeros((600 * F + PAD, 800 * F, 3), np.float32)
    paste_logo(can, Hc, LOGO)
    paste_logo(can, Hc, SUB)
    # text (grey on the 3x canvas)
    font = ImageFont.truetype(FONT, FONT_PX)
    Sg = S.max(2)
    fit = fit_text(Sg, font)
    # the original letters are heavier than plain Times at 14 px (ink sum 1.42x of a plain render):
    # render 4x larger with a 2 px outline (= 0.5 px per side at 3x) and box-shrink: ink 1.003x
    SS, SW = 4, 2
    fbig = ImageFont.truetype(FONT, FONT_PX * SS)
    tim = Image.new('L', (800 * F * SS, (600 * F + PAD) * SS))
    d = ImageDraw.Draw(tim)
    report = []
    for li, words in enumerate(fit):
        for (w, x, by, e) in words:
            d.text((x * SS, (by + 3 * LEGAL_DY) * SS), w, fill=255, font=fbig, anchor='ls', stroke_width=SW, stroke_fill=255)
    t = np.asarray(tim.resize((800 * F, 600 * F + PAD), Image.BOX)).astype(np.float32)
    can = np.maximum(can, t[..., None])
    out = np.clip(can + 0.5, 0, 255).astype(np.uint8)
    if len(sys.argv) > 4:
        out = add_ee_line(out, sys.argv[4])
    Image.fromarray(out).save(f'{outdir}/splash_eeprog_hd.png')
    # line widths (ink) original x3 vs new
    for li, top in enumerate(LINE_TOP):
        o = np.nonzero(Sg[top - 2:top + 16].max(0) > 30)[0]
        n = np.nonzero(out[3 * (top - 2 + LEGAL_DY):3 * (top + 16 + LEGAL_DY)].max(2).max(0) > 30)[0]
        report.append((li + 1, 3 * o[0], 3 * (o[-1] + 1), 3 * (o[-1] + 1 - o[0]), n[0], n[-1] + 1, n[-1] + 1 - n[0]))
    for r in report:
        print('line %d: original x3 %d..%d width %d | new %d..%d width %d (diff %+d)' % (r + (r[6] - r[3],)))
    # regions: full rows of non-black content, tightened to their ink columns
    g = out.max(2)
    rows = np.nonzero(g.max(1) > 2)[0]
    bands = []
    st = pv = rows[0]
    for r in rows[1:]:
        if r > pv + 24:       # merge bands closer than 24 rows (text lines -> one block)
            bands.append((st, pv))
            st = r
        pv = r
    bands.append((st, pv))
    regs = []
    for (a, b) in bands:
        cols = np.nonzero(g[a:b + 1].max(0) > 2)[0]
        x0, x1 = cols[0], cols[-1] + 1
        x0 -= x0 % 4
        x1 += (-x1) % 4
        x1 = min(x1, 2400)
        blk = out[a:b + 1, x0:x1]
        grey = bool((blk.max(2) - blk.min(2)).max() <= 2)
        regs.append((x0, a, x1 - x0, b + 1 - a, grey, blk))
    with open(f'{outdir}/splash4k_ee.bin', 'wb') as f:
        # header: magic, version, factor, source picture size (1x), region count
        f.write(struct.pack('<4sIIIII', b'SPHD', 1, F, 800, 600, len(regs)))
        for (x, y, w, h, grey, blk) in regs:
            f.write(struct.pack('<iiiiI', x, y - LIFT, w, h, 1 if grey else 3))   # whole picture lifted
        for (x, y, w, h, grey, blk) in regs:
            if grey:
                f.write(np.ascontiguousarray(blk[..., 0]).tobytes())
            else:
                f.write(np.ascontiguousarray(blk[..., ::-1]).tobytes())   # BGR, top-down rows
    for (x, y, w, h, grey, blk) in regs:
        print('region x %d y %d w %d h %d %s' % (x, y - LIFT, w, h, 'grey' if grey else 'BGR'))
    # the whole stored picture (black elsewhere) must equal the canvas
    chk = np.zeros_like(out)
    for (x, y, w, h, grey, blk) in regs:
        chk[y:y + h, x:x + w] = blk
    print('stored regions vs canvas: max difference %d (pixels <= 2 outside the regions are left black)' % int(np.abs(chk.astype(int) - out).max()))


if __name__ == '__main__':
    main()
