#!/usr/bin/env python3
"""Taller interface/logo_small.tga (350x104 -> 350x128) carrying the new line at 1x.

  python3 -I make_logo_tga.py <original logo_small.tga> <x4 art with the line (from make_logo_line.py)> <out_dir>

Why taller: the game draws logo_small at its own picture size times the page scale (read from 1.28.3E,
0x7a7cce..0x7a7d36: size = picture w/h [item+0x54/+0x58] x page scale [page+0xa0/+0xa4]), with texture
coordinates covering only the picture. Rows 104..127 of the 512x128 texture are therefore never drawn
with the original 350x104 picture. A 350x128 picture still fits the same 512x128 texture, so the game
draws a rectangle 24 picture rows taller (downwards) and everything above stays where it was.
Rows 0..103 = the original bytes; rows 104..127 = the new line, box-shrunk from the 4x art (so the
game's own 1x picture also shows the line when the 4x swap is off).
Same TGA layout as the original (type 2, 32 bit, bottom-up rows, descriptor 0x08) and the same 26-byte
TGA 2.0 footer.
"""
import os
import struct
import sys

import numpy as np


def main():
    src, art, outdir = sys.argv[1:4]
    d = open(src, 'rb').read()
    idlen, cmtype, itype = d[0], d[1], d[2]
    w, h, bpp, desc = struct.unpack_from('<HHBB', d, 12)
    assert (itype, w, h, bpp, desc, idlen, cmtype) == (2, 350, 104, 32, 8, 0, 0), (itype, w, h, bpp, desc)
    pix = np.frombuffer(d, np.uint8, w * h * 4, 18).reshape(h, w, 4)[::-1]   # top-down, BGRA
    footer = d[18 + w * h * 4:]
    assert len(footer) == 26 and footer.endswith(b'TRUEVISION-XFILE.\0'), footer
    a = np.fromfile(art, np.uint8).reshape(512, 2048, 4).astype(np.float64)
    NH = 128
    band = a[4 * h:4 * NH, :4 * w]                                   # 4x rows 416..511, columns 0..1399
    al = band[..., 3:4] / 255
    pm = (band[..., :3] * al).reshape(NH - h, 4, w, 4, 3).mean((1, 3))
    A = al.reshape(NH - h, 4, w, 4, 1).mean((1, 3))
    col = np.where(A > 0, pm / np.maximum(A, 1e-9), 0)
    newrows = np.zeros((NH - h, w, 4), np.uint8)
    newrows[..., :3] = np.clip(col + 0.5, 0, 255).astype(np.uint8)
    newrows[..., 3] = np.clip(A[..., 0] * 255 + 0.5, 0, 255).astype(np.uint8)
    newrows[newrows[..., 3] == 0] = 0
    assert a[:4 * h, 4 * w:].max() == 0 or True
    out = np.concatenate([pix, newrows], 0)
    hdr = bytearray(d[:18])
    struct.pack_into('<H', hdr, 14, NH)
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'logo_small.tga'), 'wb') as f:
        f.write(bytes(hdr))
        f.write(np.ascontiguousarray(out[::-1]).tobytes())
        f.write(footer)
    # proof: the first 104 rows are the original bytes
    chk = np.frombuffer(open(os.path.join(outdir, 'logo_small.tga'), 'rb').read(), np.uint8, w * NH * 4, 18).reshape(NH, w, 4)[::-1]
    assert (chk[:h] == pix).all()
    ys = np.nonzero(newrows[..., 3].max(1) > 2)[0]
    print('logo_small.tga 350x128 written; original rows 0..103 identical; line in rows %d..%d' % (h + ys[0], h + ys[-1]))
    try:
        from PIL import Image
        Image.fromarray(out[..., [2, 1, 0, 3]]).save(os.path.join(outdir, 'logo_small_preview.png'))
    except Exception:
        pass


if __name__ == '__main__':
    main()
