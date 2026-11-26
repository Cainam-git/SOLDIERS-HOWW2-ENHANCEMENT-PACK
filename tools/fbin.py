"""Decoder for the game's .fbin fonts (format worked out from the game's font loader).

load(path_fbin) -> dict(name, type 'mono'|'hq', height, widths[256], glyph(ch) -> 2D list of alpha 0..255 (h rows x width))
"""
import os, re, struct


def load(fbin):
    d = open(fbin, 'rb').read()
    desc = open(fbin[:-5] + '.font', 'rb').read().decode('latin1')
    h = int(re.search(r'\{height (\d+)\}', desc).group(1))
    size = int(re.search(r'\{size (\d+)\}', desc).group(1))
    style = int(re.search(r'\{style (\d+)\}', desc).group(1))
    hq = desc.lstrip().startswith('{font_hq')
    widths = list(d[:256])
    f = dict(name=os.path.basename(fbin)[:-5], type='hq' if hq else 'mono', height=h,
             size=size, style=style, widths=widths)
    if hq:
        offs = struct.unpack('<256I', d[0x100:0x500])
        f['hqoff'] = d[0x504]
        data = d[0x505:]

        def glyph(c):
            w = widths[c]
            o = offs[c]
            return [list(data[o + y * w:o + (y + 1) * w]) for y in range(h)]
    else:
        f['hqoff'] = 0
        slots = struct.unpack('<256H', d[0x100:0x300])
        stride = struct.unpack('<H', d[0x300:0x302])[0]
        bm = d[0x302:]

        def glyph(c):
            w = widths[c]
            x0 = 32 * slots[c]
            rows = []
            for y in range(h):
                row = []
                for x in range(x0, x0 + w):
                    byte = bm[y * stride * 4 + x // 8]
                    row.append(255 if (byte >> (x % 8)) & 1 else 0)
                rows.append(row)
            return rows
    f['glyph'] = glyph
    return f
