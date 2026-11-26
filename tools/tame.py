#!/usr/bin/env python3
# Makes: step 2 of uiscale_hd/458cc397969621dc_512x1024.raw (inventory figure sheet) and 51367eb28aed7817_512x512.raw (bag).
# Needs: SRC_DIR with the hdcache.py output <name>.raw and the 1x dump todo/<name>.raw; numpy + Pillow.
# Called as: TAME_K=1.0 python3 -I tools/tame.py SRC_DIR OUT_DIR 458cc397969621dc_512x1024 51367eb28aed7817_512x512
"""tame.py SRC_DIR OUT_DIR HASH_W_H [HASH_W_H ...]

Damps detail that a 3x cache file adds where the 1x source has none (Real-ESRGAN "weave" invented
on the flat canvas of the inventory figure and in some bag cells), keeps it where the source has
edges. out = low(hd) + gain * (hd - low(hd)), gain = clip(K * E_src / E_hd, 0, 1) per neighbourhood,
E = local mean gradient (E_src measured on a smooth 3x copy of the 1x source). Alpha unchanged.
SRC_DIR holds <name>.raw (3x BGRA) and todo/<name>.raw (1x BGRA)."""
import os, sys
import numpy as np
from PIL import Image

K = float(os.environ.get("TAME_K", "1.6"))


def box(a, r):
    # separable box blur, radius r, edge-clamped, applied twice (close to a Gaussian)
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


src_dir, out_dir = sys.argv[1], sys.argv[2]
os.makedirs(out_dir, exist_ok=True)
for name in sys.argv[3:]:
    w, h = map(int, name.split('_')[1].split('x'))
    s = np.fromfile(os.path.join(src_dir, 'todo', name + '.raw'), np.uint8).reshape(h, w, 4)
    hd = np.fromfile(os.path.join(src_dir, name + '.raw'), np.uint8).reshape(3 * h, 3 * w, 4).astype(np.float64)
    sm = np.asarray(Image.fromarray(s[..., :3].copy()).resize((3 * w, 3 * h), Image.LANCZOS), np.float64)
    es = box(grad(sm), 6)
    eh = box(grad(hd[..., :3]), 6)
    gain = np.clip(K * (es + 1) / (eh + 1), 0, 1)[..., None]
    low = box(hd[..., :3], 2)
    out = hd.copy()
    out[..., :3] = low + gain * (hd[..., :3] - low)
    np.clip(out, 0, 255).astype(np.uint8).tofile(os.path.join(out_dir, name + '.raw'))
    print(name, 'mean gain %.2f, damped (<0.7) share %.0f%%' % (gain.mean(), (gain < 0.7).mean() * 100))
