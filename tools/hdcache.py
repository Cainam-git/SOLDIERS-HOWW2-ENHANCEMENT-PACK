#!/usr/bin/env python3
# Makes: the 3x interface art files uiscale_hd/<hash>_<w>x<h>.raw from the 1x textures the dll dumps.
# Needs realesrgan-ncnn-vulkan + models (default method) and tools/art_candidates.py next to it.
# Called as: python3 -I tools/hdcache.py GAME_DIR [--model split] ; then tools/art_straighten.py on the result
"""hdcache.py GAME_DIR [--model split|split_av3|x4plus|anime3|lanczos] [--force] : fill the proxy's offline 3x cache.

The scaler dll (D3D8PROXY_UPDUMP=1) dumps photographic interface textures it does not smooth
itself (xBRZ makes noisy art blotchy) as
    GAME_DIR/uiscale_hd/todo/<hash>_<w>x<h>.raw      (w x h BGRA bytes, level 0 as the game wrote it)
This script enlarges each one 3x and writes
    GAME_DIR/uiscale_hd/<hash>_<w>x<h>.raw           ((3w) x (3h) BGRA bytes)
which the dll loads at the next game start (uiscale.ini HDCache=1, default) and binds instead of the
original for interface draws drawn 1:1. Files already done are skipped unless --force.

Why not plain Real-ESRGAN (comparison sheets were made, not included): the game draws its interface with
POINT filtering, so "original kept" means a nearest-neighbour 3x copy (3x3 blocks). Plain
realesrgan-x4plus removes the blocks but erases the faint symbols of DISABLED command buttons,
flattens grain and invents glitter on stone. Default method "split" = frequency split: the coarse
picture (shapes, colours, faint symbols) comes from Lanczos 3x, only the fine detail above a 2 px
Gaussian comes from realesrgan-x4plus (4x, Lanczos down to 3x). It sharpens, it cannot repaint.
Textures listed in PER_HASH use the method given there (stone slab: realesr-animevideov3 for the
fine band, x4plus glitters on it).
Alpha: colours are bled into transparent pixels first (no dark fringes); alpha = Lanczos 3x.
Padding: 16 px, wrap-around for textures 256 px or wider (tiling strips), edge copy otherwise.
realesrgan-ncnn-vulkan crashes when the MangoHud layer is loaded: it is started with DISABLE_MANGOHUD=1.

The cache holds pictures derived from the game's art: keep it out of any public package (ship this
script, let each user run it on his own copy).
"""
import argparse, os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image, ImageFilter
from art_candidates import bleed, pil_up, esrgan, SPLIT   # noqa: E402

# hash -> method, for textures where the default is not the best (judged on in-game screenshots)
PER_HASH = {
    '8de1dbc92a4ff6cc': 'split_av3',   # 256x64 stone slab under the stance icons
    'aa45042b00597710': 'split_soft',  # 256x64 tank porthole button: full split clashes with the soft stone strip beside it
}


def lo(v):
    return np.asarray(Image.fromarray(np.clip(v, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(SPLIT)), np.float32)


def enlarge(rgba, model, tmp):
    h, w = rgba.shape[:2]
    wrap = w >= 256
    rgb = bleed(rgba)
    a3 = pil_up(rgba[:, :, 3].copy(), 3, Image.LANCZOS)
    lz = pil_up(rgb, 3, Image.LANCZOS)
    if model == 'lanczos':
        out = lz
    elif model in ('x4plus', 'anime3'):
        out = esrgan(rgb, 'realesrgan-x4plus' if model == 'x4plus' else 'realesr-animevideov3', tmp, wrap)
    else:
        e = esrgan(rgb, 'realesr-animevideov3' if model == 'split_av3' else 'realesrgan-x4plus', tmp, wrap).astype(np.float32)
        gain = 0.5 if model == 'split_soft' else 1.0   # half-strength fine band (textured metal next to soft art)
        low = lo(lz.astype(np.float32))
        if gain < 1:   # keep Lanczos' own fine band for the other half
            low = low + (1 - gain) * (lz.astype(np.float32) - low)
        out = np.clip(low + gain * (e - lo(e)), 0, 255).astype(np.uint8)
    return np.dstack([out, a3])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('--model', default='split', choices=['split', 'split_av3', 'split_soft', 'x4plus', 'anime3', 'lanczos'])
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    base = os.path.join(a.game, 'uiscale_hd')
    todo = os.path.join(base, 'todo')
    names = sorted(n for n in os.listdir(todo) if n.endswith('.raw')) if os.path.isdir(todo) else []
    done = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n in names:
            out = os.path.join(base, n)
            if os.path.exists(out) and not a.force:
                continue
            w, h = map(int, n[:-4].split('_')[1].split('x'))
            raw = np.fromfile(os.path.join(todo, n), dtype=np.uint8)
            if raw.size != w * h * 4:
                print('skip (size mismatch):', n)
                continue
            rgba = raw.reshape(h, w, 4)[:, :, [2, 1, 0, 3]].copy()
            model = PER_HASH.get(n.split('_')[0], a.model) if a.model == 'split' else a.model
            big = enlarge(rgba, model, tmp)
            big[:, :, [2, 1, 0, 3]].astype(np.uint8).tofile(out + '.tmp')
            os.replace(out + '.tmp', out)
            done += 1
            print(f'{n}: {w}x{h} -> {w * 3}x{h * 3} ({model})')
    print(f'{done} enlarged, {len(names)} in todo, cache: {base}')


if __name__ == '__main__':
    main()
