# Checks (makes nothing): lists straight art edges whose outer 3x line is see-through (a light seam).
# Target for every shipped art file: 0 hits.
# Called as: python3 -I tools/cache_seams.py uiscale_hd GAME_DIR/uiscale_hd/todo [more 1x folders]
# straight alpha edges (>= 8 opaque 1x pixels in a line whose outside neighbour is transparent) whose outermost
# 3x sub-pixel line is see-through in the cache file (= a light seam where tiles butt against each other)
import sys, os, glob, numpy as np
cache_dir, src_dirs = sys.argv[1], sys.argv[2:]
for f in sorted(glob.glob(os.path.join(cache_dir, '*.raw'))):
    name = os.path.basename(f); hs, wh = name[:-4].split('_'); w, h = map(int, wh.split('x'))
    src = next((os.path.join(d, name) for d in src_dirs if os.path.exists(os.path.join(d, name)) and os.path.getsize(os.path.join(d, name)) == w*h*4), None)
    if not src: continue
    a1 = np.fromfile(src, np.uint8).reshape(h, w, 4)[:, :, 3].astype(int)
    a3 = np.fromfile(f, np.uint8).reshape(3*h, 3*w, 4)[:, :, 3].astype(int)
    hits = []
    for x in range(w):
        for side, nx, sub in (('R', x + 1, 3*x + 2), ('L', x - 1, 3*x)):
            if not (0 <= nx < w): continue
            edge = (a1[:, x] == 255) & (a1[:, nx] < 16)
            ys = np.where(edge)[0]
            if len(ys) < 8: continue
            seethru = sum(1 for y in ys if a3[3*y:3*y+3, sub].min() < 230)
            if seethru >= 8: hits.append('%s x=%d (%d of %d rows, mean alpha %d)' % (side, x, seethru, len(ys), int(np.mean([a3[3*y:3*y+3, sub].mean() for y in ys]))))
    for y in range(h):
        for side, ny, sub in (('B', y + 1, 3*y + 2), ('T', y - 1, 3*y)):
            if not (0 <= ny < h): continue
            edge = (a1[y, :] == 255) & (a1[ny, :] < 16)
            xs = np.where(edge)[0]
            if len(xs) < 8: continue
            seethru = sum(1 for x in xs if a3[sub, 3*x:3*x+3].min() < 230)
            if seethru >= 8: hits.append('%s y=%d (%d of %d cols)' % (side, y, seethru, len(xs)))
    print(name, len(hits), 'see-through straight edges', '; '.join(hits[:6]))
