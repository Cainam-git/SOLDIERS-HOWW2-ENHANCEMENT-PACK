#!/usr/bin/env python3
# Makes: the bottom-bar metal plate file 8de1dbc92a4ff6cc_256x64.raw (winner lz_usm_r2_100, then art_straighten.py).
# Tiling art: wrap-around padding at the content width, so the 552-px joins in the game carry no seam.
# Called as: ORIG_1X_DIR=... PREV_CACHE_DIR=... OUT_DIR=... python3 -I tools/bottombar/make_candidates.py
"""3x candidates for the bottom-bar plate 8de1dbc92a4ff6cc_256x64 (content 184x56 at 0,0; alpha 255 inside, 0 outside).
All candidates are made from the 184x56 content with WRAP-AROUND padding left/right (the game tiles it every 184 px)
and edge-copy padding top/bottom. Outside the content the bytes of today's file are kept.
usage: ORIG_1X_DIR=... PREV_CACHE_DIR=... OUT_DIR=... python3 -I tools/bottombar/make_candidates.py -> OUT_DIR/cand/*.raw, OUT_DIR/sheet_*.png
(ORIG_1X_DIR = the dll's 1x texture dump, uiscale_hd/todo; PREV_CACHE_DIR = the earlier 3x cache file of the same texture)
"""
import os, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.getcwd()
NAME = '8de1dbc92a4ff6cc_256x64'
ORIG = os.path.join(os.environ.get('ORIG_1X_DIR', 'uiscale_hd/todo'), NAME + '.raw')
TODAY = os.path.join(os.environ.get('PREV_CACHE_DIR', 'uiscale_hd'), NAME + '.raw')
OUT = os.environ.get('OUT_DIR', os.path.join(ROOT, 'out'))
MODELS = '/usr/share/realesrgan-ncnn-vulkan/models'
CW, CH, P = 184, 56, 16

o = np.fromfile(ORIG, np.uint8).reshape(64, 256, 4)[:, :, [2, 1, 0, 3]]
rgb = o[:CH, :CW, :3].copy()
today = np.fromfile(TODAY, np.uint8).reshape(192, 768, 4)[:, :, [2, 1, 0, 3]].copy()


def padw(a, p):
    a = np.pad(a, ((0, 0), (p, p), (0, 0)), mode='wrap')
    return np.pad(a, ((p, p), (0, 0), (0, 0)), mode='edge')


def lanczos3(a):
    pa = padw(a, 8)
    im = Image.fromarray(pa).resize((pa.shape[1] * 3, pa.shape[0] * 3), Image.LANCZOS)
    return np.asarray(im).astype(np.float32)[24:24 + CH * 3, 24:24 + CW * 3]


def esrgan(a, model, tmp):
    pa = padw(a, P)
    src, dst = os.path.join(tmp, 'in.png'), os.path.join(tmp, model + '.png')
    Image.fromarray(pa).save(src)
    s = 3 if model == 'realesr-animevideov3' else 4
    r = subprocess.run(['realesrgan-ncnn-vulkan', '-i', src, '-o', dst, '-m', MODELS, '-n', model, '-s', str(s), '-g', '0'],
                       capture_output=True, text=True, env=dict(os.environ, DISABLE_MANGOHUD='1', MANGOHUD='0'))
    if r.returncode or not os.path.exists(dst):
        sys.exit('realesrgan failed: ' + r.stderr[-800:])
    im = Image.open(dst).convert('RGB')
    if s != 3:   # back to 3x on the padded picture, then crop (keeps the wrap seamless)
        im = im.resize((pa.shape[1] * 3, pa.shape[0] * 3), Image.LANCZOS)
    return np.asarray(im).astype(np.float32)[P * 3:P * 3 + CH * 3, P * 3:P * 3 + CW * 3]


def blur(v, r):
    # wrap-aware Gaussian: pad horizontally by wrap, blur, crop
    pv = np.pad(np.clip(v, 0, 255), ((0, 0), (24, 24), (0, 0)), mode='wrap')
    pv = np.pad(pv, ((24, 24), (0, 0), (0, 0)), mode='edge').astype(np.uint8)
    b = np.asarray(Image.fromarray(pv).filter(ImageFilter.GaussianBlur(r)), np.float32)
    return b[24:-24, 24:-24]


def split(lz, e, r=2.0, gain=1.0):
    return blur(lz, r) + gain * (e - blur(e, r)) + (1 - min(gain, 1)) * 0   # coarse from Lanczos, fine band from model


def usm(lz, r, amt):
    return lz + amt * (lz - blur(lz, r))


def main():
    os.makedirs(os.path.join(OUT, 'cand'), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        lz = lanczos3(rgb)
        x4 = esrgan(rgb, 'realesrgan-x4plus', tmp)
        av3 = esrgan(rgb, 'realesr-animevideov3', tmp)
    c = {}
    c['today'] = today[:CH * 3, :CW * 3, :3].astype(np.float32)
    c['nearest'] = np.repeat(np.repeat(rgb, 3, 0), 3, 1).astype(np.float32)
    c['lanczos'] = lz
    c['av3split_wrap'] = split(lz, av3)                 # today's method, only made seamless
    c['x4split'] = split(lz, x4)
    c['x4split_g15'] = split(lz, x4, 2.0, 1.5)          # stronger fine band
    c['x4split_r3'] = split(lz, x4, 3.0)                # wider fine band
    for p in (25, 50, 75):
        c['x4_%d' % p] = lz * (1 - p / 100) + x4 * p / 100
    c['x4plus'] = x4
    c['lz_usm_r2_100'] = usm(lz, 2.0, 1.0)
    c['lz_usm_r3_60'] = usm(lz, 3.0, 0.6)
    c['lz_usm_r15_150'] = usm(lz, 1.5, 1.5)
    for k, v in c.items():
        full = today.copy()
        full[:CH * 3, :CW * 3, :3] = np.clip(np.round(v), 0, 255).astype(np.uint8)
        full[:CH * 3, :CW * 3, 3] = 255
        full[:, :, [2, 1, 0, 3]].tofile(os.path.join(OUT, 'cand', k + '.raw'))
    # metrics: sharpness (mean |gradient|), join step (|col 551 - col 0|) vs typical neighbour step, mean colour
    lines = ['%-16s %7s %7s %7s  %s' % ('cand', 'grad', 'join', 'typ', 'mean RGB')]
    for k, v in c.items():
        g = np.abs(np.diff(v.mean(2), axis=1)).mean() + np.abs(np.diff(v.mean(2), axis=0)).mean()
        j = np.abs(v[:, -1] - v[:, 0]).mean()
        t = np.abs(np.diff(v, axis=1)).mean()
        lines.append('%-16s %7.2f %7.2f %7.2f  %s' % (k, g, j, t, np.round(v.reshape(-1, 3).mean(0), 1)))
    open(os.path.join(OUT, 'metrics.txt'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    # sheets: each candidate tiled 4x in a row as on screen (2208 x 168 real px), labelled
    def sheet(keys, path, ntile=4):
        W = CW * 3 * ntile
        H = len(keys) * (CH * 3 + 20) + 20
        im = Image.new('RGB', (W, H), (16, 16, 16)); d = ImageDraw.Draw(im)
        d.text((4, 4), NAME + ': each row = candidate tiled %dx as in the game (real 4K px; joins at x=552,1104,1656)' % ntile, fill=(255, 255, 255))
        for i, k in enumerate(keys):
            y = 20 + i * (CH * 3 + 20)
            d.text((4, y + 2), k, fill=(255, 220, 0))
            row = np.tile(np.clip(np.round(c[k]), 0, 255).astype(np.uint8), (1, ntile, 1))
            im.paste(Image.fromarray(row), (0, y + 18))
        im.save(path)
    sheet(['today', 'av3split_wrap', 'lanczos', 'x4split', 'x4split_g15', 'x4split_r3', 'x4_25'], os.path.join(OUT, 'sheet_A.png'))
    sheet(['today', 'x4_50', 'x4_75', 'x4plus', 'lz_usm_r2_100', 'lz_usm_r3_60', 'lz_usm_r15_150'], os.path.join(OUT, 'sheet_B.png'))
    # close-up crop around the first join (x 400..700), 3 candidates per row, real px
    keys = list(c)
    tiles = [Image.fromarray(np.tile(np.clip(np.round(c[k]), 0, 255).astype(np.uint8), (1, 2, 1))[:, 402:702]) for k in keys]
    cols = 5; im = Image.new('RGB', (cols * 310, ((len(keys) + cols - 1) // cols) * 190), (16, 16, 16)); d = ImageDraw.Draw(im)
    for i, k in enumerate(keys):
        x, y = (i % cols) * 310, (i // cols) * 190
        im.paste(tiles[i], (x, y + 18)); d.text((x + 2, y + 2), k + ' (join at 150)', fill=(255, 220, 0))
    im.save(os.path.join(OUT, 'sheet_join_px.png'))


if __name__ == '__main__':
    main()
