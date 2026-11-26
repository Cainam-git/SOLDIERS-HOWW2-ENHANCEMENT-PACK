#!/usr/bin/env python3
"""Taller interface/logo.tga (the big logo shown right after login) with the line "ENHANCED EDITION".

  python3 -I make_biglogo_tga.py <original logo.tga 1557x470> <steam logo.png> <out_dir>

The big logo is placed by 0x7aed60 (read from 1.28.3E): scale = page width x const / picture width, height =
picture height x the same scale, so a taller picture gives a taller box downwards (the text item under it
moves down by the extra height). 1557x470 sits in a 2048x512 texture; the line needs ~100 more rows, so
the new picture (1557x576) lives in a 2048x1024 texture. No 4x copy (it would be 8192x4096): the game
draws this picture with its own filtering, exactly as it draws the original big logo today (the pack has
no sharp copy for it either), and the picture itself is already at about the 4x level of the menu logo.
Geometry and finish: as make_logo_line.py, with the finish sizes scaled from the menu 4x art to this
picture by the ratio of the two subtitles' widths.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_logo_line import FONT, coverage_edges, finish, steam_geometry  # noqa: E402

SS = 4
MENU_SUB_W = 945.0      # width of the subtitle fill in the menu 4x art (make_logo_line.py prints it)
NEW_H = 576


def main():
    src, steam, outdir = sys.argv[1:4]
    d = open(src, 'rb').read()
    w, h, bpp, desc = struct.unpack_from('<HHBB', d, 12)
    assert (d[2], w, h, bpp, desc) == (2, 1557, 470, 32, 8)
    pix = np.frombuffer(d, np.uint8, w * h * 4, 18).reshape(h, w, 4)[::-1]
    footer = d[18 + w * h * 4:]
    assert len(footer) == 26 and footer.endswith(b'TRUEVISION-XFILE.\0')
    a = pix.astype(np.float64)
    f = (a[..., 3] / 255) * np.clip((a[..., :3].min(2) - 60) / 150, 0, 1)   # white letters only (not the red)
    band = f[390:445]
    y0, y1 = coverage_edges(band.sum(1))
    cols = np.nonzero(band.max(0) > 0.5)[0]
    o = dict(y0=y0 + 390, y1=y1 + 390, x0=cols[0], x1=cols[-1] + 1)
    g = steam_geometry(steam)
    sx = (o['x1'] - o['x0']) / (g['sub']['x1'] - g['sub']['x0'])
    sy = (o['y1'] - o['y0']) / (g['sub']['y1'] - g['sub']['y0'])
    cxs, cxo = (g['sub']['x0'] + g['sub']['x1']) / 2, (o['x0'] + o['x1']) / 2
    X = lambda xs: cxo + (xs - cxs) * sx
    top = o['y1'] + (g['ee']['y0'] - g['sub']['y1']) * sy
    cap = (g['ee']['y1'] - g['ee']['y0']) * sy
    fs = (o['x1'] - o['x0']) / MENU_SUB_W
    print('big logo subtitle %s scale x %.4f y %.4f; line top %.1f cap %.1f bottom %.1f; finish x%.3f' % (
        o, sx, sy, top, cap, top + cap, fs))
    big = ImageFont.truetype(FONT, 1000)
    size = cap * SS / -big.getbbox('E', anchor='ls')[1] * 1000
    fnt = ImageFont.truetype(FONT, int(round(size)))
    k = cap * SS / -fnt.getbbox('E', anchor='ls')[1]
    ratios = []
    for ch, (c0, c1) in g['letters']:
        bb = fnt.getbbox(ch, anchor='ls')
        ratios.append((X(c1) - X(c0)) * SS / ((bb[2] - bb[0]) * k))
    hs = float(np.median(ratios))
    R0 = 446
    H, W = (NEW_H - R0) * SS, (w + 3) // 4 * 4 * SS
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
        yy = int(round(base - (4 - bb[1]) * k))
        F[yy:yy + nh, x0:x0 + nw] = np.maximum(F[yy:yy + nh, x0:x0 + nw], gl)
    L, A = finish(np.clip(F, 0, 1), ow=1.25 * fs, sdx=3 * fs, sdy=4 * fs, ssig=1.5 * fs, ss=SS)
    box = lambda m: m.reshape(H // SS, SS, W // SS, SS).mean((1, 3))
    A1 = box(A)
    L1 = np.where(A1 > 0, box(L * A) / np.maximum(A1, 1e-6), 0)
    out = np.zeros((NEW_H, w, 4), np.uint8)
    out[:h] = pix
    assert pix[R0:].max() <= 3 or pix[R0:, :, 3].max() <= 3, 'rows under the subtitle are not empty'
    seg = np.zeros((NEW_H - R0, w, 4), np.uint8)
    lum = np.clip(L1[:, :w] + 0.5, 0, 255).astype(np.uint8)
    seg[..., 0] = seg[..., 1] = seg[..., 2] = lum
    seg[..., 3] = np.clip(A1[:, :w] * 255 + 0.5, 0, 255).astype(np.uint8)
    seg[seg[..., 3] == 0] = 0
    keep = out[R0:].copy()
    out[R0:] = np.where(seg[..., 3:4] > keep[..., 3:4], seg, keep)
    assert (out[:R0] == pix[:R0]).all()
    ys = np.nonzero(seg[..., 3].max(1) > 2)[0]
    print('line pixels in rows %d..%d of %d' % (R0 + ys[0], R0 + ys[-1], NEW_H))
    hdr = bytearray(d[:18])
    struct.pack_into('<H', hdr, 14, NEW_H)
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'logo.tga'), 'wb') as fo:
        fo.write(bytes(hdr))
        fo.write(np.ascontiguousarray(out[::-1]).tobytes())
        fo.write(footer)
    Image.fromarray(out[..., [2, 1, 0, 3]]).save(os.path.join(outdir, 'logo_big_preview.png'))
    bg = Image.new('RGBA', (w, NEW_H), (200, 205, 210, 255))
    bg.alpha_composite(Image.fromarray(out[..., [2, 1, 0, 3]]))
    bg.save(os.path.join(outdir, 'logo_big_preview_on_sky.png'))


if __name__ == '__main__':
    main()
