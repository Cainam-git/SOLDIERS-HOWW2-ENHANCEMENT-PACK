#!/usr/bin/env python3
# Makes: the wood weapon-bar file c4f4c0dbd0c0e501_256x64.raw (method split, then art_straighten.py).
# Imports make_candidates.py (which runs on import: set its environment variables too).
# Called as: python3 -I tools/bottombar/rewrap.py c4f4c0dbd0c0e501_256x64 split ORIG_DIR PREV_CACHE_DIR OUT_DIR
"""Remake a tiling cache file with the SAME method as hdcache.py, but wrap-around padding at the CONTENT width
(alpha>0 bounding box starting at 0,0) instead of the texture width. Top/bottom: edge copy. Outside the content the
bytes of today's file are kept.
usage: python3 -I tools/bottombar/rewrap.py NAME METHOD ORIG_DIR TODAY_DIR OUT_DIR    METHOD: split | split_av3 | split_soft
Prints the join step (|last col - first col|) vs the typical neighbour step for today and new, and the mean
difference new-vs-today away from the edges (should be ~0 = look unchanged)."""
import os, sys, tempfile
import numpy as np
from PIL import Image, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_candidates import padw, P, MODELS  # noqa
import subprocess

name, method, odir, tdir, outd = sys.argv[1:6]
w, h = map(int, name.split('_')[1].split('x'))
o = np.fromfile(os.path.join(odir, name + '.raw'), np.uint8).reshape(h, w, 4)[:, :, [2, 1, 0, 3]]
ys, xs = np.where(o[:, :, 3] > 0)
CW, CH = xs.max() + 1, ys.max() + 1
assert xs.min() == 0 and ys.min() == 0 and (o[:CH, :CW, 3] == 255).all(), 'content not a full opaque rect at 0,0'
rgb = o[:CH, :CW, :3].copy()
today = np.fromfile(os.path.join(tdir, name + '.raw'), np.uint8).reshape(3 * h, 3 * w, 4)[:, :, [2, 1, 0, 3]].copy()


def lanczos3(a):
    pa = padw(a, 8)
    return np.asarray(Image.fromarray(pa).resize((pa.shape[1] * 3, pa.shape[0] * 3), Image.LANCZOS)).astype(np.float32)[24:24 + CH * 3, 24:24 + CW * 3]


def esrgan(a, model, tmp):
    pa = padw(a, P)
    src, dst = os.path.join(tmp, 'in.png'), os.path.join(tmp, 'out.png')
    Image.fromarray(pa).save(src)
    s = 3 if model == 'realesr-animevideov3' else 4
    r = subprocess.run(['realesrgan-ncnn-vulkan', '-i', src, '-o', dst, '-m', MODELS, '-n', model, '-s', str(s), '-g', '0'],
                       capture_output=True, text=True, env=dict(os.environ, DISABLE_MANGOHUD='1', MANGOHUD='0'))
    if r.returncode or not os.path.exists(dst):
        sys.exit('realesrgan failed: ' + r.stderr[-800:])
    im = Image.open(dst).convert('RGB')
    if s != 3:
        im = im.resize((pa.shape[1] * 3, pa.shape[0] * 3), Image.LANCZOS)
    return np.asarray(im).astype(np.float32)[P * 3:P * 3 + CH * 3, P * 3:P * 3 + CW * 3]


def blur(v, r=2.0):
    pv = np.pad(np.clip(v, 0, 255), ((0, 0), (24, 24), (0, 0)), mode='wrap')
    pv = np.pad(pv, ((24, 24), (0, 0), (0, 0)), mode='edge').astype(np.uint8)
    return np.asarray(Image.fromarray(pv).filter(ImageFilter.GaussianBlur(r)), np.float32)[24:-24, 24:-24]


with tempfile.TemporaryDirectory() as tmp:
    lz = lanczos3(rgb)
    e = esrgan(rgb, 'realesr-animevideov3' if method == 'split_av3' else 'realesrgan-x4plus', tmp)
gain = 0.5 if method == 'split_soft' else 1.0
low = blur(lz)
if gain < 1:
    low = low + (1 - gain) * (lz - low)
new = np.clip(np.round(low + gain * (e - blur(e))), 0, 255).astype(np.uint8)
full = today.copy()
full[:CH * 3, :CW * 3, :3] = new
full[:CH * 3, :CW * 3, 3] = 255
os.makedirs(outd, exist_ok=True)
full[:, :, [2, 1, 0, 3]].tofile(os.path.join(outd, name + '.raw'))
t = today[:CH * 3, :CW * 3, :3].astype(np.float32); n = new.astype(np.float32)
for k, v in (('today', t), ('new', n)):
    print('%s: content %dx%d, %-5s join %.2f typical %.2f grad %.2f mean %s' % (name, CW, CH, k, np.abs(v[:, -1] - v[:, 0]).mean(),
          np.abs(np.diff(v, axis=1)).mean(), np.abs(np.diff(v.mean(2), axis=1)).mean(), np.round(v.reshape(-1, 3).mean(0), 1)))
print('mean |new - today| in the middle (x 48..-48): %.2f' % np.abs(n[:, 48:-48] - t[:, 48:-48]).mean())
