#!/usr/bin/env python3
# Makes: 3x candidate files + comparison sheets for one texture; imported by hdcache.py for its enlargers.
# The "xbrz" column needs tools/xbrz3, built from tools/xbrz3.cpp (see its header).
# Called as: python3 -I tools/art_candidates.py OUTDIR uiscale_hd/todo/<hash>_<w>x<h>.raw ...
"""art_candidates.py OUTDIR RAW... : make 3x candidates of interface textures (<hash>_<w>x<h>.raw, BGRA)
and one comparison sheet per texture at real 4K pixel size (what the player sees, 3x).

Columns: now (nearest = the game's POINT filter on the original), lanczos, bicubic, xbrz (the dll's
scaler), esr_x4p (realesrgan-x4plus, 4x then Lanczos to 3x), esr_anime (realesrgan-x4plus-anime),
esr_av3 (realesr-animevideov3 at 3x), plus blends.
Alpha: colours are bled into transparent pixels before scaling (no dark fringes); alpha itself is
scaled with Lanczos (smooth edge) for every candidate except now/xbrz. Padding: 16 px edge replicate
(atlases: cells are separate; tiling textures get --wrap).
Writes OUTDIR/<name>/<cand>.raw (3x BGRA, cache format) and OUTDIR/<name>/sheet.png.
"""
import argparse, os, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
SPLIT = 2.0   # Gaussian radius (4K px) of the frequency split

MODELS = '/usr/share/realesrgan-ncnn-vulkan/models'
XBRZ = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xbrz3')   # built from tools/xbrz3.cpp, see its header
PAD = 16


def load(p):
    n = os.path.basename(p)[:-4]
    w, h = map(int, n.split('_')[1].split('x'))
    bgra = np.fromfile(p, dtype=np.uint8).reshape(h, w, 4)
    return bgra[:, :, [2, 1, 0, 3]].copy(), n


def bleed(rgba):
    """fill RGB of (nearly) transparent pixels from opaque neighbours, so scalers see no black halo"""
    rgb = rgba[:, :, :3].astype(np.float32)
    known = rgba[:, :, 3] >= 8
    if known.all() or not known.any():
        return rgba[:, :, :3].copy()
    out = rgb * known[..., None]
    k = known.astype(np.float32)
    for _ in range(64):
        if k.all():
            break
        s = np.zeros_like(out); c = np.zeros_like(k)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
            s += np.roll(np.roll(out * k[..., None], dy, 0), dx, 1)
            c += np.roll(np.roll(k, dy, 0), dx, 1)
        new = (k == 0) & (c > 0)
        out[new] = s[new] / c[new][:, None]
        k = np.where(new, 1.0, k)
    return np.clip(out, 0, 255).astype(np.uint8)


def pil_up(arr, f, method):
    mode = 'RGB' if arr.ndim == 3 else 'L'
    im = Image.fromarray(arr, mode)
    return np.asarray(im.resize((im.width * f, im.height * f), method))


def esrgan(rgb, model, tmp, wrap):
    h, w = rgb.shape[:2]
    pad = np.pad(rgb, ((PAD, PAD), (PAD, PAD), (0, 0)), mode='wrap' if wrap else 'edge')
    src, dst = os.path.join(tmp, 'in.png'), os.path.join(tmp, 'out.png')
    if os.path.exists(dst):
        os.replace(dst, dst + '.old')
    Image.fromarray(pad, 'RGB').save(src)
    s = 3 if model == 'realesr-animevideov3' else 4
    r = subprocess.run(['realesrgan-ncnn-vulkan', '-i', src, '-o', dst, '-m', MODELS, '-n', model, '-s', str(s), '-g', '0'],
                       capture_output=True, text=True, env=dict(os.environ, DISABLE_MANGOHUD='1', MANGOHUD='0'))
    if r.returncode or not os.path.exists(dst):
        sys.exit('realesrgan failed: ' + r.stderr[-1500:])
    out = Image.open(dst).convert('RGB').crop((PAD * s, PAD * s, PAD * s + w * s, PAD * s + h * s))
    if s != 3:
        out = out.resize((w * 3, h * 3), Image.LANCZOS)
    return np.asarray(out)


def xbrz(rgba, tmp):
    h, w = rgba.shape[:2]
    a, b = os.path.join(tmp, 'x.raw'), os.path.join(tmp, 'y.raw')
    rgba[:, :, [2, 1, 0, 3]].tofile(a)
    subprocess.run([XBRZ, a, b, str(w), str(h)], check=True)
    return np.fromfile(b, dtype=np.uint8).reshape(h * 3, w * 3, 4)[:, :, [2, 1, 0, 3]].copy()


def candidates(rgba, tmp, wrap=False):
    h, w = rgba.shape[:2]
    rgb = bleed(rgba)
    a3 = pil_up(rgba[:, :, 3].copy(), 3, Image.LANCZOS)
    c = {}
    c['now'] = np.repeat(np.repeat(rgba, 3, 0), 3, 1)
    lz = pil_up(rgb, 3, Image.LANCZOS)
    c['lanczos'] = np.dstack([lz, a3])
    c['bicubic'] = np.dstack([pil_up(rgb, 3, Image.BICUBIC), pil_up(rgba[:, :, 3].copy(), 3, Image.BICUBIC)])
    c['xbrz'] = xbrz(rgba, tmp)
    for k, m in (('esr_x4p', 'realesrgan-x4plus'), ('esr_anime', 'realesrgan-x4plus-anime'), ('esr_av3', 'realesr-animevideov3')):
        c[k] = np.dstack([esrgan(rgb, m, tmp, wrap), a3])
    # 50/50 blend of the Real-ESRGAN result with Lanczos: keeps edges clean, tames invented texture
    c['x4p_lz50'] = np.dstack([((c['esr_x4p'][:, :, :3].astype(np.uint16) + lz) // 2).astype(np.uint8), a3])
    # frequency split: coarse picture (shapes, faint symbols, colours) from Lanczos = faithful,
    # fine edges from the model; the model can then sharpen but not erase or repaint
    for k, src in (('fs_x4p', 'esr_x4p'), ('fs_av3', 'esr_av3')):
        e = c[src][:, :, :3].astype(np.float32)
        lo = lambda v: np.asarray(Image.fromarray(np.clip(v, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(SPLIT)), np.float32)
        c[k] = np.dstack([np.clip(lo(lz.astype(np.float32)) + e - lo(e), 0, 255).astype(np.uint8), a3])
    usm = Image.fromarray(lz).filter(ImageFilter.UnsharpMask(radius=2, percent=80, threshold=2))
    c['lz_usm'] = np.dstack([np.asarray(usm), a3])
    return c


def over(rgba, bg):
    a = rgba[:, :, 3:4].astype(np.float32) / 255
    return (rgba[:, :, :3] * a + bg * (1 - a)).astype(np.uint8)


def sheet(c, path, title, maxw=1100):
    names = list(c)
    h3, w3 = c['now'].shape[:2]
    # show at most maxw px of width per column (real pixels, no zoom)
    cw = min(w3, maxw)
    bg = np.zeros((h3, cw, 3), np.uint8); bg[:] = (74, 82, 52)   # game-like olive backdrop
    cols = [over(c[n][:, :cw], bg) for n in names]
    per_row = max(1, 3840 // (cw + 8))
    rows = [names[i:i + per_row] for i in range(0, len(names), per_row)]
    W = min(len(names), per_row) * (cw + 8)
    H = 24 + len(rows) * (h3 + 22)
    out = Image.new('RGB', (W, H), (16, 16, 16))
    d = ImageDraw.Draw(out)
    d.text((4, 4), title + '  (real 4K pixels, 3x, over olive)', fill=(255, 255, 255))
    for r, row in enumerate(rows):
        for i, n in enumerate(row):
            x, y = i * (cw + 8), 24 + r * (h3 + 22)
            d.text((x + 2, y + 2), n, fill=(255, 220, 0))
            out.paste(Image.fromarray(cols[names.index(n)]), (x, y + 18))
    out.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('raws', nargs='+')
    ap.add_argument('--wrap', action='store_true')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        for p in a.raws:
            rgba, n = load(p)
            d = os.path.join(a.outdir, n)
            os.makedirs(d, exist_ok=True)
            c = candidates(rgba, tmp, a.wrap or rgba.shape[1] >= 256)
            for k, v in c.items():
                v[:, :, [2, 1, 0, 3]].astype(np.uint8).tofile(os.path.join(d, k + '.raw'))
            sheet(c, os.path.join(d, 'sheet.png'), n)
            print('done', n)


if __name__ == '__main__':
    main()
