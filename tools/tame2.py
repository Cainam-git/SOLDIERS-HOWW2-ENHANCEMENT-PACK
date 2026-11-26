#!/usr/bin/env python3
# Makes: step 3 of uiscale_hd/458cc397969621dc_512x1024.raw: plain Lanczos on flat canvas, tamed picture elsewhere.
# Needs: SRC_DIR as for tame.py, TAMED_DIR = tame.py output; numpy + Pillow.
# Called as: T0=4.5 T1=10 python3 -I tools/tame2.py SRC_DIR TAMED_DIR OUT_DIR 458cc397969621dc_512x1024
"""tame2.py SRC_DIR TAMED_DIR OUT_DIR NAME

Second step for the inventory figure sheet: on FLAT canvas (low detail in the 1x source) the plain
Lanczos enlargement of the source replaces the cache picture, so no regular pattern the source lacks
is left; where the source has edges (figure, face, hands, frame, folds) the tamed cache picture stays.
mask = smoothstep of the local source detail E_src between T0 and T1 (same measure as tame.py),
only inside the sheet (alpha >= 200 in a 3x3 neighbourhood; edges of the sheet keep the cache)."""
import os, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
T0 = float(os.environ.get('T0', '10'))
T1 = float(os.environ.get('T1', '22'))


def box(a, r):
    for _ in range(2):
        for ax in (0, 1):
            p = np.pad(a, [(r, r) if i == ax else (0, 0) for i in range(a.ndim)], mode='edge')
            c = np.cumsum(p, axis=ax, dtype=np.float64)
            c = np.concatenate([np.zeros_like(np.take(c, [0], axis=ax)), c], axis=ax)
            n = a.shape[ax]
            a = (np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=ax) - np.take(c, np.arange(0, n), axis=ax)) / (2 * r + 1)
    return a


def grad(a):
    g = np.zeros(a.shape[:2])
    g[:, 1:] += np.abs(np.diff(a, axis=1)).max(2)
    g[1:, :] += np.abs(np.diff(a, axis=0)).max(2)
    return g


src_dir, tamed_dir, out_dir, name = sys.argv[1:5]
os.makedirs(out_dir, exist_ok=True)
w, h = map(int, name.split('_')[1].split('x'))
s = np.fromfile(os.path.join(src_dir, 'todo', name + '.raw'), np.uint8).reshape(h, w, 4)
t = np.fromfile(os.path.join(tamed_dir, name + '.raw'), np.uint8).reshape(3 * h, 3 * w, 4).astype(np.float64)
lz = np.asarray(Image.fromarray(s[..., :3].copy()).resize((3 * w, 3 * h), Image.LANCZOS), np.float64)
es = box(grad(lz), 6)
m = np.clip((es - T0) / (T1 - T0), 0, 1)
m = m * m * (3 - 2 * m)                                   # smoothstep: 0 = flat canvas, 1 = detail
op = s[..., 3] >= 200   # the canvas itself is drawn at alpha ~204
op = op & np.roll(op, 1, 0) & np.roll(op, -1, 0) & np.roll(op, 1, 1) & np.roll(op, -1, 1)
op3 = np.repeat(np.repeat(op, 3, 0), 3, 1)
m = np.where(op3, m, 1.0)
# painted fully opaque parts (face, hands, metal frame: alpha >= 240) always keep the cache picture
hi = np.repeat(np.repeat(s[..., 3] >= 240, 3, 0), 3, 1)
m = np.where(hi, 1.0, m)
m = box(m, 2)[..., None]
out = t.copy()
out[..., :3] = lz + m * (t[..., :3] - lz)
np.clip(out, 0, 255).astype(np.uint8).tofile(os.path.join(out_dir, name + '.raw'))
print(name, 'flat share (mask<0.3) %.0f%%' % ((m < 0.3).mean() * 100), 'E_src percentiles 25/50/75/90:',
      np.percentile(es, [25, 50, 75, 90]).round(1))
