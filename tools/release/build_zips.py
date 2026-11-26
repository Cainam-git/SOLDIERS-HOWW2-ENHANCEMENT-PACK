#!/usr/bin/env python3
"""Builds the three release zips. Run it through tools/build_patch.sh; what it does is described there."""
import argparse, hashlib, os, re, sys, zipfile
from concurrent.futures import ProcessPoolExecutor
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
NAME = 'Soldiers-HoWW2-Enhancement-Pack-1.2'
LIMIT = int(1.9 * (1 << 30))
ROOM = 32 << 20
ZIP_TIME = (2026, 11, 25, 0, 0, 0)
MEDIA_EXT = ('.wma', '.wmv')
HD_SUB = '@appdata/mods/hd_project/resource/'
GROUPS = (('pack', 'the pack, the music and videos that play under Proton, the program files'),
          ('ee', 'the game data of the Enhanced Edition'),
          ('hd', 'the HD textures'))
TEXTS = (('README-Enhancement-Pack.txt', os.path.join(HERE, 'README-Enhancement-Pack.txt')),
         ('LICENSE-Enhancement-Pack.txt', os.path.join(ROOT, 'LICENSE')))


def sha(data_or_path):
    if isinstance(data_or_path, bytes):
        return hashlib.sha256(data_or_path).hexdigest()
    h = hashlib.sha256()
    with open(data_or_path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def walk(base):
    out = []
    for dp, dn, fn in os.walk(base):
        for f in fn:
            out.append(os.path.relpath(os.path.join(dp, f), base).replace(os.sep, '/'))
    return sorted(out)


def read_list(p):
    return [tuple(l.rstrip('\n').split('  ', 1)) for l in open(p, encoding='utf-8') if l.strip()]


def path_ci(root, rel):
    cur = root
    for part in rel.split('/'):
        hit = sorted(e for e in os.listdir(cur) if e.lower() == part.lower()) if os.path.isdir(cur) else []
        cur = os.path.join(cur, hit[0] if hit else part)
    return cur


class GogNames:
    """GOG's spelling of every folder and file of a fresh GOG install, per level."""
    def __init__(self, rels):
        self.kids = {}
        for r in rels:
            parts = r.split('/')
            for i in range(len(parts)):
                self.kids.setdefault('/'.join(parts[:i]).lower(), {})[parts[i].lower()] = parts[i]

    def spell(self, rel):
        out, key = [], ''
        parts = rel.split('/')
        for i, p in enumerate(parts):
            q = self.kids.get(key, {}).get(p.lower())
            if q is None:
                return '/'.join(out + parts[i:])
            out.append(q)
            key = '/'.join(out).lower()
        return '/'.join(out)


def mount_path(rel):
    """The game's own path of a file: 'resource/x' and 'resource.<lang>[.intro]/x' -> 'x'; else None."""
    m = re.match(r'^resource(\.[a-z]+)*/(.+)$', rel, re.I)
    return m.group(2).lower() if m else None


def collect(conf):
    """[(group, zip path, source path or bytes, sha256)] and the build notes."""
    gog = read_list(os.path.join(HERE, 'gog_1.28.3E_18993.sha256'))
    names = GogNames([r for _, r in gog])
    files, notes, problems = {}, [], []

    def add(group, rel, src, h):
        zp = names.spell(rel)
        k = zp.lower()
        if k in files:
            problems.append(f'  {zp}: would be in two parts ({files[k][0]} and {group})')
            return
        files[k] = (group, zp, src, h)

    # part 1: the pack (with its music and videos) and the program files
    pack = conf['pack']
    for need in ('d3d9.dll', 'uiscale.ini', 'hdtext_fonts.bin', 'uiscale_hd'):
        if not os.path.exists(os.path.join(pack, need)):
            sys.exit(f'STOPPED: --pack {pack} has no {need}')
    for rel in walk(pack):
        if ('/' not in rel and rel.lower().endswith('.txt')) or rel.lower().startswith('keys-enhancement-pack/'):
            continue
        p = os.path.join(pack, rel)
        add('pack', rel, p, sha(p))
    prog = conf['program']
    for rel in walk(prog):
        p = os.path.join(prog, rel)
        add('pack', rel, p, sha(p))
    for name, p in TEXTS:
        add('pack', name, p, sha(p))
    # part 2: the Enhanced Edition data, each file checked against the list
    for h, rel in read_list(os.path.join(HERE, 'layer1_ee_R1.28.8.sha256')):
        p = path_ci(conf['ee'], rel)
        if not os.path.isfile(p):
            problems.append(f'  {rel}: not found in the Enhanced Edition folder (--ee)')
        elif sha(p) != h:
            problems.append(f'  {rel}: in the Enhanced Edition folder it is not the R1.28.8 file (checksum differs)')
        else:
            add('ee', rel, p, h)
    # part 3: the HD textures as loose files under resource/
    for h, rel in read_list(os.path.join(HERE, 'layer4_hd_project_0.51.sha256')):
        if not rel.startswith(HD_SUB):
            continue
        p = path_ci(conf['hd'], 'resource/' + rel[len(HD_SUB):])
        if not os.path.isfile(p):
            problems.append(f'  {rel}: not found in the HD Project folder (--hd)')
        elif sha(p) != h:
            problems.append(f'  {rel}: in the HD Project folder it is not the 0.51 file (checksum differs)')
        else:
            add('hd', 'resource/' + rel[len(HD_SUB):], p, h)
    if problems:
        sys.exit('STOPPED, nothing was written:\n' + '\n'.join(problems[:40]))

    # GOG files that stay behind: those in the loose Resource/ folder are seen by the game under their game path
    by_mount = {}
    for k, (group, zp, src, h) in files.items():
        mp = mount_path(zp)
        if mp is not None:
            by_mount.setdefault(mp, (group, src, h))
    pak = zipfile.ZipFile(path_ci(conf['ee'], 'resource.pak'))
    in_pak = {n.lower(): n for n in pak.namelist()}
    stays = []
    for h, rel in gog:
        if rel.lower() in files:
            continue
        mp = mount_path(rel)
        if mp is None:
            stays.append(rel)
            continue
        if mp in by_mount:
            group, src, h2 = by_mount[mp]
            add(group, rel, src, h2)
            notes.append(f'covered: {rel} (seen by the game as {mp}) gets our copy of that file')
        elif mp in in_pak:
            data = pak.read(in_pak[mp])
            if sha(data) != h:
                add('ee', rel, data, sha(data))
                notes.append(f'covered: {rel} differs from the Enhanced Edition\'s {mp}: their copy from resource.pak')
            else:
                notes.append(f'stays: {rel} (the same bytes as the Enhanced Edition\'s {mp})')
        else:
            stays.append(rel)
            notes.append(f'stays: {rel} (seen as {mp}, a name the Enhanced Edition never asks for)')
    notes.append(f'stays, not read by the game (outside its resource folders): {len([s for s in stays if mount_path(s) is None])} '
                 'files: ' + ', '.join(s for s in stays if mount_path(s) is None))
    if problems:
        sys.exit('STOPPED, nothing was written:\n' + '\n'.join(problems[:40]))
    low = {}
    for k, (group, zp, src, h) in files.items():
        if zp.lower() in low and low[zp.lower()] != zp:
            sys.exit(f'STOPPED: two paths differ only in case: {zp}')
        low[zp.lower()] = zp
    return sorted(files.values(), key=lambda x: x[1]), notes


def size_of(src):
    return len(src) if isinstance(src, bytes) else os.path.getsize(src)


def _zip(job):
    zpath, items = job
    tmp = zpath + '.part'
    with zipfile.ZipFile(tmp, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
        for zp, src in sorted(items):
            zi = zipfile.ZipInfo(zp, ZIP_TIME)
            zi.create_system = 3
            zi.external_attr = 0o100644 << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            if isinstance(src, bytes):
                z.writestr(zi, src, compresslevel=6)
                continue
            zi.file_size = os.path.getsize(src)
            with open(src, 'rb') as fi, z.open(zi, 'w') as fo:
                for b in iter(lambda: fi.read(1 << 22), b''):
                    fo.write(b)
    os.replace(tmp, zpath)
    return zpath, os.path.getsize(zpath), sha(zpath)


def build(conf, out):
    files, notes = collect(conf)
    print('Every input file checked against the lists.')
    groups, trials = [], {}
    for g, what in GROUPS:
        items = [(zp, src, h) for grp, zp, src, h in files if grp == g]
        total = sum(size_of(s) for _, s, _ in items)
        n = max(1, -(-total // (LIMIT - ROOM)))
        if n > 1:                       # try it whole first: compressed it may still fit, and then it IS that part
            t = os.path.join(out, f'.whole_{g}.zip')
            _, zs, zh = _zip((t, [(zp, s) for zp, s, _ in items]))
            if zs < LIMIT:
                n = 1
                trials[g] = (t, zs, zh)
            else:
                os.makedirs(os.path.join(out, 'previous'), exist_ok=True)
                os.rename(t, unique(os.path.join(out, 'previous', os.path.basename(t))))
        target = -(-total // n)
        chunks, cur, cs = [], [], 0
        for it in items:
            z = size_of(it[1])
            if cur and n > 1 and len(chunks) < n - 1 and (cs >= target or cs + z > LIMIT - ROOM):
                chunks.append(cur)
                cur, cs = [], 0
            cur.append(it)
            cs += z
        chunks.append(cur)
        groups += [(g, what + (f' ({i} of {len(chunks)})' if len(chunks) > 1 else ''), c)
                   for i, c in enumerate(chunks, 1)]
    n = len(groups)
    earlier = sorted(f for f in os.listdir(out) if f.startswith(NAME + '-part') or f in ('PARTS.sha256', 'FILES.sha256'))
    if earlier:
        keep = unique(os.path.join(out, 'previous', 'zips'))
        os.makedirs(keep)
        for f in earlier:
            os.rename(os.path.join(out, f), os.path.join(keep, f))
        print(f'Earlier zips moved to {keep}')
    res, jobs = {}, []
    for i, (g, w, c) in enumerate(groups, 1):
        final = os.path.join(out, f'{NAME}-part{i}of{n}.zip')
        if g in trials and sum(1 for x in groups if x[0] == g) == 1:
            t, zs, zh = trials.pop(g)
            os.rename(t, final)         # the whole-group zip made above: same files, same bytes
            res[i] = (final, zs, zh)
        else:
            jobs.append((i, (final, [(zp, s) for zp, s, _ in c])))
    print(f'Writing {len(jobs)} more zips side by side ...')
    with ProcessPoolExecutor(max_workers=min(4, max(1, len(jobs)))) as ex:
        for (i, _), r in zip(jobs, ex.map(_zip, [j for _, j in jobs])):
            res[i] = r
    res = [res[i] for i in sorted(res)]
    for (zpath, zs, h), (g, w, c) in zip(res, groups):
        if zs >= LIMIT:
            sys.exit(f'STOPPED: {os.path.basename(zpath)} is {zs} bytes, over the limit')
        print(f'  {os.path.basename(zpath)}  {zs / (1 << 30):.2f} GiB ({zs} bytes), {len(c)} files: {w}')
    with open(os.path.join(out, 'PARTS.sha256'), 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(f'{h}  {os.path.basename(z)}\n' for z, _, h in res)
    with open(os.path.join(out, 'FILES.sha256'), 'w', encoding='utf-8', newline='\n') as f:
        for i, (g, w, c) in enumerate(groups, 1):
            f.writelines(f'{h}  part{i}of{n}  {zp}\n' for zp, s, h in c)
    with open(os.path.join(out, 'BUILD-NOTES.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(notes) + '\n')
    print('\n'.join('  ' + x for x in notes if not x.startswith('stays, not read')))
    print(f'PARTS.sha256, FILES.sha256 and BUILD-NOTES.txt written in {out}')


def unique(p):
    if not os.path.lexists(p):
        return p
    i = 2
    while os.path.lexists(f'{p}.{i}'):
        i += 1
    return f'{p}.{i}'


def verify(game, out):
    """Every file of FILES.sha256 is in the game folder with its exact spelling and bytes."""
    bad = []
    rows = [l.rstrip('\n').split('  ', 2) for l in open(os.path.join(out, 'FILES.sha256'), encoding='utf-8') if l.strip()]
    for h, part, zp in rows:
        p = os.path.join(game, *zp.split('/'))
        if not os.path.isfile(p):
            bad.append(f'  missing ({part}): {zp}')
        elif sha(p) != h:
            bad.append(f'  differs ({part}): {zp}')
    twins = {}
    for dp, dn, fn in os.walk(game):
        for e in dn + fn:
            twins.setdefault((dp, e.lower()), []).append(e)
    tw = [os.path.join(d, '|'.join(v)) for (d, k), v in twins.items() if len(v) > 1]
    print(f'{len(rows)} files of the zips checked in {game}: ' + ('ALL PRESENT AND IDENTICAL.' if not bad else
                                                                f'{len(bad)} problems:\n' + '\n'.join(bad[:40])))
    print('Names that differ only in case: ' + ('none.' if not tw else '\n  ' + '\n  '.join(tw[:20])))
    return not bad and not tw


def main():
    ap = argparse.ArgumentParser(description='Builds the three Enhancement Pack zips (see tools/build_patch.sh).')
    for k in ('pack', 'ee', 'hd', 'program', 'out', 'config', 'verify'):
        ap.add_argument('--' + k)
    a = ap.parse_args()
    conf_path = a.config or os.path.join(ROOT, 'disc', 'inputs.conf')
    conf = {}
    if os.path.isfile(conf_path):
        for line in open(conf_path, encoding='utf-8'):
            line = line.strip()
            if line and not line.startswith('#') and '=' in line:
                k, v = line.split('=', 1)
                conf[k.strip()] = v.strip()
    out = os.path.abspath(a.out or conf.get('out') or os.path.join(ROOT, 'disc'))
    if a.verify:
        sys.exit(0 if verify(os.path.abspath(a.verify), out) else 1)
    given = False
    for k in ('pack', 'ee', 'hd', 'program'):
        v = getattr(a, k)
        if v:
            conf[k] = os.path.abspath(v)
            given = True
        if not conf.get(k) or not os.path.isdir(conf[k]):
            sys.exit(f'--{k}: no folder {conf.get(k) or "(not given)"}  (inputs are read from {conf_path})')
    os.makedirs(out, exist_ok=True)
    build(conf, out)
    if given:
        tmp = conf_path + '.new'
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write('# Inputs of tools/build_patch.sh (local, never committed); see its header.\n')
            f.writelines(f'{k} = {conf[k]}\n' for k in ('pack', 'ee', 'hd', 'program'))
        os.replace(tmp, conf_path)
        print(f'Inputs saved in {conf_path}: next time run the command without arguments.')


if __name__ == '__main__':
    main()
