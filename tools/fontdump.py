#!/usr/bin/env python3
# Makes: the letter pack hdtext_fonts.bin (written as build/fonts_shape2/hdfonts.bin with the call below).
# Needs the game fonts (*.fbin) unpacked by hand into build/fonts/src, and tools/fbin.py.
# Called as: FONTDUMP_PICK=shape FONTDUMP_OUT=build/fonts_shape2 python3 -I tools/fontdump.py
"""Font review sheets + HD glyph pack for the hdtext module.

  python3 -I tools/fontdump.py              # all fonts in pak_extract/interface/font

For every .fbin font it picks the best matching outline font (candidates per
family below, scored by ink overlap with the original glyphs at 1x), and
writes:
  build/fonts/sheet_<font>.png   per glyph: original 1x | outline 1x |
                                 original 3x (nearest) | HD glyph 3x (as packed)
                                 red frame = glyph uses the fallback (smooth
                                 enlargement of the original bitmap)
  build/fonts/fit.txt            chosen outline font, size, score per font
  build/hdtext/hdfonts.bin       the pack the game-side module loads

HD glyph rule: cell = exactly 3*advance x 3*height (so layout cannot move);
baseline = 3x the original baseline (bottom of 'H'); outline glyph is centred
on 3x the original ink centre and squeezed horizontally if wider than the cell.

Pack format (little endian): "HDTX" u32 ver=2, u32 nfonts; per font:
u8 type (0 mono, 1 hq), u8 height, u8 hqoff, u8 scale(3), 32 B name (v2), 256 B widths,
then for c in 0..255: (3w*3h) alpha bytes (w = widths[c]).
"""
import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import math, struct
import fbin
from PIL import Image, ImageDraw, ImageFont, ImageFilter

S = 3
MINSCORE = 0.30  # below this the font uses the fallback for every glyph
# fonts as the game loads them: game.pak overridden by Resource/Patch/*.pat (3.pat replaces the four
# bookman fonts and adds main_hq_11); extracted by hand from game.pak and Resource/Patch/*.pat into build/fonts/src
FONTDIR = os.path.join(ROOT, 'build', 'fonts', 'src')
LIB = '/usr/share/fonts/liberation/'
GS = '/usr/share/fonts/gsfonts/'
TTF = '/usr/share/fonts/TTF/'
CANDS = {
    'arial': [LIB + 'LiberationSans-Regular.ttf', LIB + 'LiberationSans-Bold.ttf'],
    'arial_hq': [LIB + 'LiberationSans-Regular.ttf', LIB + 'LiberationSans-Bold.ttf'],
    'courier': [LIB + 'LiberationMono-Regular.ttf', LIB + 'LiberationMono-Bold.ttf'],
    'bookman': [GS + 'URWBookman-Light.otf', GS + 'URWBookman-Demi.otf'],
    'bookman_hq': [GS + 'URWBookman-Light.otf', GS + 'URWBookman-Demi.otf'],
    'main': [TTF + 'DejaVuSans.ttf', TTF + 'DejaVuSans-Bold.ttf', TTF + 'DejaVuSansCondensed.ttf',
             TTF + 'DejaVuSansCondensed-Bold.ttf', LIB + 'LiberationSans-Regular.ttf',
             LIB + 'LiberationSans-Bold.ttf'],
    'main_hq': None,  # filled below = 'main'
    'bookman_hq_shadow': [],  # glyph + baked shadow: fallback only
}
CANDS['main_hq'] = CANDS['main']
SAMPLE = [ord(c) for c in 'ABCDEFGHKMNORSTWXYZabcdefghkmnoprstuwxyz0123456789']


def family(name):
    base = name.rsplit('_', 1)[0]
    return base


def ink_box(g):
    xs = [x for row in g for x, v in enumerate(row) if v > 96]
    ys = [y for y, row in enumerate(g) if any(v > 96 for v in row)]
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def orig_img(g, w, h):
    im = Image.new('L', (max(w, 1), h), 0)
    for y, row in enumerate(g):
        for x, v in enumerate(row):
            if v:
                im.putpixel((x, y), v)
    return im


def metrics(f):
    gH = f['glyph'](72)
    b = ink_box(gH)
    capH = b[3] - b[1] + 1
    baseline = b[3] + 1
    return capH, baseline


import functools


@functools.lru_cache(maxsize=256)
def tt(fontfile, size):
    return ImageFont.truetype(fontfile, size, layout_engine=ImageFont.Layout.BASIC)


def render(fontfile, size, c, w, h, baseline, scale, f):
    """Outline glyph for byte c into a (w*scale x h*scale) cell; None if no ink/glyph."""
    try:
        ch = bytes([c]).decode('cp1252')
    except UnicodeDecodeError:
        return None
    fo = tt(fontfile, size)
    if fo.getmask(ch).getbbox() is None:
        return None
    ob = ink_box(f['glyph'](c))
    if ob is None:
        return None
    W, H = w * scale, h * scale
    big = Image.new('L', (W * 3 + 64, H), 0)
    ImageDraw.Draw(big).text((W + 32, baseline * scale), ch, font=fo, fill=255, anchor='ls')
    bb = big.getbbox()
    if bb is None:
        return None
    glyph = big.crop((bb[0], 0, bb[2], H))
    gw = glyph.width
    if gw > W:  # squeeze into the cell
        glyph = glyph.resize((W, H), Image.LANCZOS)
        gw = W
    cx = (ob[0] + ob[2] + 1) / 2.0 * scale
    x0 = int(round(cx - gw / 2.0))
    x0 = max(0, min(W - gw, x0))
    out = Image.new('L', (W, H), 0)
    out.paste(glyph, (x0, 0))
    return out


def ink_ratio(f, ff, size, baseline):
    """HD ink (coverage, in 1x pixel units) / original ink over the sample glyphs."""
    o = h = 0.0
    for c in SAMPLE:
        w = f['widths'][c]
        if not w:
            continue
        o += sum(v / 255.0 for row in f['glyph'](c) for v in row)
        r = render(ff, size, c, w, f['height'], baseline, S, f)
        if r is not None:
            h += sum(r.tobytes()) / 255.0 / (S * S)
    return h / o if o else 1.0


def fit(f):
    """Shape by ink overlap (IoU) at 1x; weight by ink amount: among the faces whose shape score is
    within 0.08 of the best, take the one whose total ink is closest to the original's (so bold
    game fonts get a bold face only when the original strokes really carry that much ink)."""
    capH, baseline = metrics(f)
    per_face = []
    for ff in CANDS.get(family(f['name']), []):
        fo = tt(ff, 100)
        cap100 = -fo.getbbox('H', anchor='ls')[1]
        best = None
        for k in (-0.10, -0.05, 0.0, 0.05, 0.10):
            size1 = capH * 100.0 / cap100 * (1 + k)
            score = n = 0
            for c in SAMPLE:
                w = f['widths'][c]
                if not w:
                    continue
                o = f['glyph'](c)
                r = render(ff, size1 * S, c, w, f['height'], baseline, S, f)
                if r is None:
                    continue
                r1 = r.resize((w, f['height']), Image.BOX)
                inter = uni = 0
                for y in range(f['height']):
                    for x in range(w):
                        a = o[y][x] > 96
                        b = r1.getpixel((x, y)) > 110
                        inter += a and b
                        uni += a or b
                score += inter / uni if uni else 1
                n += 1
            score = score / max(n, 1)
            if best is None or score > best[0]:
                best = (score, ff, size1 * S)
        if best:
            per_face.append(best + (ink_ratio(f, best[1], best[2], baseline),))
    if not per_face:
        return None, baseline
    top = max(p[0] for p in per_face)
    near = [p for p in per_face if p[0] >= top - 0.08]
    ink = lambda p: abs(math.log(max(p[3], 1e-3)))
    pick = min(near, key=ink)
    if os.environ.get('FONTDUMP_PICK') == 'shape' and family(f['name']) in ('main', 'main_hq'):
        # variant: the ink amount only decides between faces whose ink really
        # differs (regular against bold); otherwise the best SHAPE wins. The plain rule let a
        # tie in ink pick Liberation Sans for the Verdana-like "main" fonts (narrow "r").
        best_shape = max(near, key=lambda p: p[0])
        if ink(best_shape) - ink(pick) < 0.15:
            pick = best_shape
    return pick, baseline


def scale3x(im):
    """EPX/Scale3x on a binary mask, then light smoothing (fallback for 1-bit glyphs)."""
    w, h = im.size
    p = im.load()
    out = Image.new('L', (w * 3, h * 3), 0)
    q = out.load()

    def P(x, y):
        return p[x, y] if 0 <= x < w and 0 <= y < h else 0
    for y in range(h):
        for x in range(w):
            A, B, C = P(x-1, y-1), P(x, y-1), P(x+1, y-1)
            D, E, F = P(x-1, y), P(x, y), P(x+1, y)
            G, H, I = P(x-1, y+1), P(x, y+1), P(x+1, y+1)
            e = [E] * 9
            if B != H and D != F:
                e[0] = D if D == B else E
                e[1] = B if (D == B and E != C) or (B == F and E != A) else E
                e[2] = F if B == F else E
                e[3] = D if (D == B and E != G) or (D == H and E != A) else E
                e[5] = F if (B == F and E != I) or (H == F and E != C) else E
                e[6] = D if D == H else E
                e[7] = H if (D == H and E != I) or (H == F and E != G) else E
                e[8] = F if H == F else E
            for i in range(9):
                q[x * 3 + i % 3, y * 3 + i // 3] = e[i]
    return out.filter(ImageFilter.GaussianBlur(0.55))


def fallback(f, c):
    w, h = f['widths'][c], f['height']
    o = orig_img(f['glyph'](c), w, h)
    if f['type'] == 'mono':
        return scale3x(o.point(lambda v: 255 if v > 0 else 0))
    return o.resize((w * S, h * S), Image.BICUBIC)


def main():
    out_fonts = os.path.join(ROOT, 'build', 'fonts')
    out_pack = os.path.join(ROOT, 'build', 'hdtext')
    if os.environ.get('FONTDUMP_OUT'):   # variant builds go to their own folder
        out_fonts = out_pack = os.environ['FONTDUMP_OUT']
    os.makedirs(out_fonts, exist_ok=True)
    os.makedirs(out_pack, exist_ok=True)
    names = sorted(n[:-5] for n in os.listdir(FONTDIR) if n.endswith('.fbin'))
    if len(sys.argv) > 1:
        names = [n for n in names if n in sys.argv[1:]]
    pack = [b'HDTX', struct.pack('<II', 2, len(names))]
    fitlog = []
    for name in names:
        f = fbin.load(os.path.join(FONTDIR, name + '.fbin'))
        h = f['height']
        best, baseline = fit(f)
        nfb = 0
        tiles = []
        pack.append(struct.pack('<BBBB', 0 if f['type'] == 'mono' else 1, h, f['hqoff'], S))
        pack.append(name.encode()[:31].ljust(32, b'\0'))
        pack.append(bytes(f['widths']))
        for c in range(256):
            w = f['widths'][c]
            if not w:
                tiles.append(None)
                continue
            hd = None
            if best is not None and best[0] > MINSCORE:
                hd = render(best[1], best[2], c, w, h, baseline, S, f)
            used_fb = False
            if hd is None:
                if ink_box(f['glyph'](c)) is not None:
                    hd = fallback(f, c)
                    used_fb = True
                    nfb += 1
                else:
                    hd = Image.new('L', (w * S, h * S), 0)
            assert hd.size == (w * S, h * S)
            pack.append(hd.tobytes())
            if c >= 32:
                o = orig_img(f['glyph'](c), w, h)
                tiles.append((c, o, hd, used_fb))
        # sheet
        cellw = max(f['widths']) * (2 + 2 * S) + 12
        cellh = h * S + 6
        cols = 16
        live = [t for t in tiles if t]
        rows = (len(live) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * cellw, rows * cellh + 16), (60, 60, 60))
        d = ImageDraw.Draw(sheet)
        d.text((2, 2), '%s: %s' % (name, ('%s px %.1f score %.2f' % (os.path.basename(best[1]), best[2], best[0])) if best else 'fallback only'), fill=(255, 255, 0))
        for i, (c, o, hd, fb) in enumerate(live):
            x = (i % cols) * cellw + 2
            y = (i // cols) * cellh + 18
            w = o.width
            ol = None
            if best is not None and not fb:
                r = render(best[1], best[2], c, w, h, baseline, S, f)
                ol = r.resize((w, h), Image.BOX) if r else None
            for k, im in enumerate((o, ol if ol else Image.new('L', (w, h), 0))):
                sheet.paste(Image.merge('RGB', (im, im, im)), (x + k * (w + 1), y))
            xx = x + 2 * (w + 1) + 2
            big = o.resize((w * S, h * S), Image.NEAREST)
            sheet.paste(Image.merge('RGB', (big, big, big)), (xx, y))
            sheet.paste(Image.merge('RGB', (hd, hd, hd)), (xx + w * S + 2, y))
            if fb:
                d.rectangle((xx + w * S + 1, y - 1, xx + 2 * w * S + 2, y + h * S), outline=(255, 0, 0))
        sheet.save(os.path.join(out_fonts, 'sheet_%s.png' % name))
        line = '%-24s %-5s h=%2d %s fallback_glyphs=%d' % (
            name, f['type'], h,
            ('%s px=%.2f score=%.3f ink=%.2f' % (os.path.basename(best[1]), best[2], best[0], best[3])) if best else 'NO OUTLINE (fallback only)', nfb)
        print(line, flush=True)
        fitlog.append(line)
    open(os.path.join(out_pack, 'hdfonts.bin'), 'wb').write(b''.join(pack))
    open(os.path.join(out_fonts, 'fit.txt'), 'w').write('\n'.join(fitlog) + '\n')


if __name__ == '__main__':
    main()
