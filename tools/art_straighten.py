#!/usr/bin/env python3
# Makes: final art files: hardens straight tile edges of 3x cache files (second step after hdcache.py).
# Input = 3x cache folder + the folders holding the 1x originals; output folder is written fresh.
# Called as: python3 -I tools/art_straighten.py SRC_CACHE_DIR DST_CACHE_DIR GAME_DIR/uiscale_hd/todo
"""art_straighten.py SRC_CACHE_DIR DST_CACHE_DIR [DIR_WITH_1X_ORIGINALS ...] : make straight content edges of 3x cache files hard again.

The enlargers leave a soft transparency ramp (255, 178, 77, 0) across the edge of a piece of art.
Where that edge is a straight line on the 1x pixel grid (art cut off at a tile border, e.g. the wood
weapon bar), the half-transparent column lets the background shine through as a light line at the
seam with the next tile (seen in game on the wood weapon bar). For every boundary on a multiple of 3 real pixels
that is straight for at least 9 pixels (opaque two pixels inside, empty one pixel outside), the
inner pixel becomes fully opaque and the outer one fully transparent. Curved outlines are untouched.
Files are <hash>_<w>x<h>.raw = (3w)x(3h) BGRA. Prints the number of changed pixels per file."""
import os, sys, numpy as np

def runs(mask, n=9):
    """keep only True runs of length >= n along axis 0"""
    out = np.zeros_like(mask)
    for x in range(mask.shape[1]):
        col = mask[:, x]; y = 0; H = len(col)
        while y < H:
            if col[y]:
                e = y
                while e < H and col[e]: e += 1
                if e - y >= n: out[y:e, x] = True
                y = e
            else:
                y += 1
    return out

def straighten(al):
    """al: alpha plane (H, W), edges along columns (vertical edges). Returns number of changes."""
    H, W = al.shape; ch = 0
    for x in range(3, W - 2, 3):          # boundary between column x-1 and x
        for inner, outer, far_in, far_out in ((x - 1, x, x - 2, x + 1), (x, x - 1, x + 1, x - 2)):
            m = (al[:, far_in] >= 250) & (al[:, far_out] <= 5) & ((al[:, inner] < 250) | (al[:, outer] > 5))
            straight = (al[:, far_in] >= 250) & (al[:, far_out] <= 5)
            keep = runs(straight[:, None])[:, 0] & m
            ch += int(keep.sum())
            al[keep, inner] = 255; al[keep, outer] = 0
    return ch

def solid_edges(a, a1):
    """Second pass, needs the 1x original (alpha plane a1): every fully opaque 1x pixel that has an empty
    direct neighbour AND at least one neighbour along that edge in the same situation (locally straight
    edge) gets the outermost 3x line facing the empty side made fully opaque. Corners and curves keep
    their soft ramp. Covers short straight runs between notches that the run-length pass skips."""
    al = a[:, :, 3]; op = a1 == 255; tr = a1 < 16; n = 0
    for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        nb = np.zeros_like(op)
        if dx == 1: nb[:, :-1] = tr[:, 1:]
        if dx == -1: nb[:, 1:] = tr[:, :-1]
        if dy == 1: nb[:-1, :] = tr[1:, :]
        if dy == -1: nb[1:, :] = tr[:-1, :]
        e = op & nb
        st = e & (np.roll(e, 1, 0) | np.roll(e, -1, 0)) if dx != 0 else e & (np.roll(e, 1, 1) | np.roll(e, -1, 1))
        for y, x in zip(*np.where(st)):
            if dx == 1: blk = al[3*y:3*y+3, 3*x+2]
            elif dx == -1: blk = al[3*y:3*y+3, 3*x]
            elif dy == 1: blk = al[3*y+2, 3*x:3*x+3]
            else: blk = al[3*y, 3*x:3*x+3]
            m = blk < 255; n += int(m.sum()); blk[m] = 255
    return n

src, dst = sys.argv[1], sys.argv[2]
src1x = sys.argv[3:]      # optional: folders with the 1x originals (<same name>.raw, w x h BGRA) for the second pass
os.makedirs(dst, exist_ok=True)
total = 0
for name in sorted(os.listdir(src)):
    if not name.endswith('.raw'): continue
    w, h = (int(v) for v in name[:-4].split('_')[-1].split('x'))
    a = np.fromfile(os.path.join(src, name), dtype=np.uint8).reshape(h * 3, w * 3, 4).copy()
    al = a[:, :, 3]
    c = straighten(al)                    # vertical edges
    alt = np.ascontiguousarray(al.T); c += straighten(alt); a[:, :, 3] = alt.T   # horizontal edges
    c2 = 0
    s1 = next((os.path.join(d, name) for d in src1x if os.path.exists(os.path.join(d, name)) and os.path.getsize(os.path.join(d, name)) == w * h * 4), None)
    if s1:
        c2 = solid_edges(a, np.fromfile(s1, dtype=np.uint8).reshape(h, w, 4)[:, :, 3])
    elif src1x:
        print('%-34s no 1x original found: second pass skipped' % name)
    a.tofile(os.path.join(dst, name)); total += c + c2
    if c or c2: print('%-34s %6d + %4d pixels' % (name, c, c2))
print('total changed pixels:', total)
