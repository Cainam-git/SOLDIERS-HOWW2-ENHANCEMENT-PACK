#!/usr/bin/env python3
"""Read an ee.log of the Enhancement Pack (Enhanced Edition program, d3d9.dll) and say in plain words what worked.

  python3 -I tools/eelog.py <path to ee.log> [--last]

--last: only the last start in the file (the file is appended to at every start; a start begins with "---- ee.log opened").
Output: one line per pack piece (ON / OFF / NOT SEEN, with the reason), then "Looks like trouble", then a detail
section (addresses, found against expected bytes). Read only; needs nothing but Python 3.
Written against the log lines of src/ee (main.cpp, scale3.cpp, small.cpp, d3dhook.cpp, up/, brand.cpp, splash4k.cpp).
"""
import re
import sys

LINE = re.compile(r'^(\d\d:\d\d:\d\d\.\d{3}) \[(\d+)\] (.*)$')


def starts(lines):
    """split the file into starts"""
    out, cur = [], []
    for l in lines:
        if l.startswith('---- ee.log opened'):
            if cur:
                out.append(cur)
            cur = [l]
        else:
            cur.append(l)
    if cur:
        out.append(cur)
    return out


def analyse(block):
    msgs = []                     # (time, text)
    for l in block:
        m = LINE.match(l.rstrip('\r\n'))
        if m:
            msgs.append((m.group(1), m.group(3)))
    text = [t for _, t in msgs]

    def has(pat):
        return any(re.search(pat, t) for t in text)

    def first(pat):
        for t in text:
            m = re.search(pat, t)
            if m:
                return m
        return None

    def last(pat):
        r = None
        for t in text:
            m = re.search(pat, t)
            if m:
                r = m
        return r

    pieces, trouble, detail = [], [], []

    def piece(state, name, why=''):
        pieces.append((state, name, why))

    # ---- patches through ee_patch: last outcome per (name, address)
    patch = {}
    for t in text:
        m = re.match(r'patch (.+?) @([0-9a-f]{8})(?: \((\d+) bytes\))?: (.*)$', t)
        if m:
            patch[(m.group(1), m.group(2))] = m.group(4)
    bad_patches = {k: v for k, v in patch.items() if not (v.startswith('applied') or v.startswith('already'))}
    for (name, addr), v in sorted(bad_patches.items(), key=lambda x: x[0][1]):
        detail.append('change "%s" at %s: %s' % (name, addr, v))

    def patches_ok(prefix):
        ks = [k for k in patch if k[0].startswith(prefix)]
        if not ks:
            return None
        return all(k not in bad_patches for k in ks)

    # ---- program
    m = first(r'program check: .* -> (.*)$')
    if not m:
        piece('NOT SEEN', 'Pack loaded into the game', 'no "program check" line: the pack did not start (d3d9.dll not used? launch options line missing?)')
    elif m.group(1).startswith('KNOWN'):
        piece('ON', 'Pack recognised the game', 'exactly the R1.28.8 program')
    elif m.group(1).startswith('VARIANT'):
        piece('ON', 'Pack recognised the game', 'same R1.28.8 version, slightly different copy; every change checks itself')
    else:
        piece('OFF', 'Pack recognised the game', 'unknown program version: the pack changes NOTHING')
        trouble.append('The game program is not the R1.28.8 version the pack knows: nothing of the pack is active.')
    if not has(r'^init done'):
        trouble.append('The pack did not finish starting up (no "init done").')

    # ---- interface x3
    if has(r'^scale3: UiFactor=\d+ -> off'):
        piece('OFF', 'Bigger interface (3x)', 'switched off in uiscale.ini (UiFactor)')
    elif has(r'^scale3: unknown program'):
        piece('OFF', 'Bigger interface (3x)', 'unknown program')
    else:
        m = first(r'^scale3: site (.+?) @([0-9a-f]{8}) has unexpected bytes: NOTHING patched(.*)$')
        if m:
            piece('OFF', 'Bigger interface (3x)', 'one place in the program is different (%s), so none of it was changed' % m.group(1))
            detail.append('scale3 site %s @%s%s' % (m.group(1), m.group(2), m.group(3)))
        else:
            m = first(r'^scale3: (\d+) of (\d+) sites patched')
            if m and m.group(1) == m.group(2):
                f = last(r'^scale3: screen mode (\d+)x(\d+) -> interface factor (\d+)')
                if f:
                    piece('ON', 'Bigger interface (3x)', 'screen %sx%s, interface %sx' % (f.group(1), f.group(2), f.group(3)))
                else:
                    piece('ON', 'Bigger interface (3x)', 'ready (the game never set a screen mode in this log)')
            elif m:
                piece('OFF', 'Bigger interface (3x)', 'only %s of %s places changed' % (m.group(1), m.group(2)))
            else:
                piece('NOT SEEN', 'Bigger interface (3x)', 'no line about it')
        if has(r'^scale3: pointer hook NOT placed'):
            trouble.append('The mouse pointer stays small (pointer hook not placed).')

    # ---- sharp pictures and text (d3dhook + up/)
    m = last(r'^d3dhook: D3DLog=\d+ UiUpscale=(\d+)')
    if m and m.group(1) == '0':
        piece('OFF', 'Sharp interface pictures', 'switched off in uiscale.ini (UiUpscale=0)')
    elif has(r'texture \+ surface vtables hooked'):
        piece('ON', 'Sharp interface pictures', 'hooked into the graphics')
    elif has(r'CreateDevice\('):
        piece('OFF', 'Sharp interface pictures', 'graphics started but the pack could not hook in')
    else:
        piece('NOT SEEN', 'Sharp interface pictures', 'the game did not get as far as starting its graphics')
    m = last(r'hdtext_install -> (-?\d+)')
    if m:
        n = int(m.group(1))
        if n > 0:
            piece('ON', 'Sharp text', '%d fonts matched' % n)
        else:
            piece('OFF', 'Sharp text', 'no fonts matched (code %d); is hdtext_fonts.bin next to the game?' % n)
    else:
        piece('NOT SEEN', 'Sharp text', 'no line about it')
    # drop-down / list text copied between canvases keeps its sharp letters (double-text fix)
    m = last(r'hdtext: canvas copy hook: (\d+) of (\d+) call sites, carry-over (on|off)')
    if m:
        ncar = len([t for t in text if re.match(r'hdtext: canvas copy .* glyph records carried', t)])
        if m.group(3) == 'off':
            piece('OFF', 'Single sharp letters in drop-downs', 'switched off in uiscale.ini (ListText=0)')
        elif m.group(1) != m.group(2):
            piece('OFF', 'Single sharp letters in drop-downs', 'program different at that place (%s of %s sites)' % (m.group(1), m.group(2)))
            trouble.append('Drop-down text fix not in place: %s of %s sites.' % (m.group(1), m.group(2)))
        else:
            piece('ON', 'Single sharp letters in drop-downs', 'letters carried over %d time%s' % (ncar, '' if ncar == 1 else 's') if ncar else 'in place; no drop-down seen yet')

    # ---- small fixes
    def small(name, on_pat, off_pat, label):
        if has(off_pat):
            m = first(off_pat)
            piece('OFF', label, 'switched off in uiscale.ini' if 'off (' in m.group(0) else m.group(0))
        elif has(on_pat):
            ok = patches_ok(name)
            piece('ON' if ok is not False else 'OFF', label, '' if ok is not False else 'a change did not fit, see details')
        else:
            m = first(r'^%s: .*NOT applied.*' % re.escape(name))
            if m:
                piece('OFF', label, 'the program is different at that place, game\'s own behaviour')
                detail.append(m.group(0))
            else:
                piece('NOT SEEN', label, 'no line about it')

    small('fps list', r'^fps list: Max FPS rows .* applied', r'^fps list: off', 'Max FPS list (V-Sync, 240, 120, 60, 30)')
    small('sound', r'^sound: Buffers list', r'^sound: (off|Buffers list bytes differ).*', '64 sounds at once (Audio > Buffers 64)')
    b = last(r'watch: sound budget \(manager\+0x3c\) = (\d+)')
    if b:
        use = 'sounds at once in use: %s' % b.group(1) if b.group(1) != '0' else 'sound system not started in this run (silent PC?)'
        pieces[-1] = (pieces[-1][0], pieces[-1][1], (pieces[-1][2] + '; ' if pieces[-1][2] else '') + use)
    small('scroll fix', r'^scroll fix: on', r'^scroll fix: off', 'Camera stops when a key is let go')
    small('keys defaults', r'^keys defaults: RESTORE DEFAULTS gives', r'^keys defaults: off', 'RESTORE DEFAULTS gives WASD keys')
    small('new profile hook', r'^new profile keys: a new profile gets', r'^new profile keys: off', 'New profiles get WASD keys')
    # src/ee/profile.cpp: a first start with no profile
    label = 'Profile for a first start'
    m = last(r'^profile: (First-start profile made|Profile present): (.*)$')
    if has(r'^profile: off \(MakeProfile=0\)'):
        piece('OFF', label, 'switched off in uiscale.ini (MakeProfile=0)')
    elif m and m.group(1) == 'Profile present':
        piece('ON', label, 'profile already there, nothing written: ' + m.group(2))
    elif m:
        made = [t for t in text if re.match(r'^profile: made ', t)]
        bad = 'COULD NOT' in m.group(2)
        piece('OFF' if bad else 'ON', label, ('profile made: ' if not bad else 'some files could not be written: ') + m.group(2))
        for t in made:
            detail.append(t)
    elif has(r'^profile: COULD NOT'):
        piece('OFF', label, first(r'^profile: COULD NOT.*').group(0)[9:])
    elif has(r'^profile: module not built in'):
        piece('OFF', label, 'not in this pack build')
    else:
        piece('NOT SEEN', label, 'no line about it (an older pack build?)')
    for t in text:
        mm = re.match(r"^profile: '(.+?)': the HD Project mod is no longer switched on", t)
        if mm:
            detail.append('profile %s: old HD Project mod tick taken out (the textures are loose files now)' % mm.group(1))
    for t in text:
        mm = re.match(r'^sound: profile (.+?) \((.+?)\): Buffers (20 -> \d+)', t)
        if mm:
            detail.append('profile %s (%s): sounds at once raised %s' % (mm.group(1), mm.group(2), mm.group(3)))
    small('camera zoom', r'^camera zoom: distance', r'^camera zoom: off.*', 'Camera zooms further out')
    small('skip logos', r'^skip logos: on', r'^skip logos: off', 'Company logo videos skipped')
    n = len([t for t in text if re.match(r'skip logos: .* not played', t)])
    if n:
        pieces[-1] = (pieces[-1][0], pieces[-1][1], '%d video%s not played' % (n, '' if n == 1 else 's'))

    # ---- branding
    ok = patches_ok('brand: menu version line')
    if has(r'^brand: VersionLine=0'):
        piece('OFF', 'Version line', 'switched off in uiscale.ini')
    elif ok is None:
        piece('NOT SEEN', 'Version line', 'no line about it')
    else:
        mv = last(r'brand: version text "(.*)"')
        piece('ON' if ok else 'OFF', 'Version line', (mv.group(1) if mv else '') if ok else 'the program is different at that place')
    # src/ee/menus.cpp: main menu without MULTI PLAYER / MODS
    for key, what in (('HideMultiplayer', 'MULTI PLAYER'), ('HideMods', 'MODS')):
        label = 'Main menu without %s' % what
        if has(r'^menus: %s=0' % key):
            piece('OFF', label, 'switched off in uiscale.ini (%s=0)' % key)
        elif has(r'^menus: %s=1, .* removed' % key):
            piece('ON', label)
        elif has(r'^menus: %s=1 but' % key):
            piece('OFF', label, 'the program is different at that place, entry stays')
        else:
            piece('NOT SEEN', label, 'no line about it')
    if has(r'^splash4k: Splash4K=0'):
        piece('OFF', '4K start-up picture', 'switched off in uiscale.ini')
    elif has(r'^splash4k: 4K splash painted'):
        piece('ON', '4K start-up picture', 'shown')
    else:
        why = None
        for pat, w in ((r'^splash4k: no ', 'file uiscale_hd\\splash4k_ee.bin is missing'),
                       (r'^splash4k: .* is not valid', 'file uiscale_hd\\splash4k_ee.bin is damaged'),
                       (r'other picture, left alone', 'the game showed a different start-up picture (other language?)'),
                       (r'smaller than', 'the window was too small for the 4K picture'),
                       (r'could not read the source picture', 'could not read the game\'s picture')):
            if has(pat):
                why = w
                break
        if why is None and patches_ok('splash4k') is False:
            why = 'the program is different at that place'
        piece('OFF' if why else 'NOT SEEN', '4K start-up picture', why or 'the game did not ask to draw its start-up picture')

    # ---- remade picture files (content keys, from the log lines)
    found = sorted(set(re.findall(r'\[cache\] key ([0-9a-f]{16}) (\d+x\d+): cache file found', '\n'.join(text))))
    miss = sorted(set(re.findall(r'\[cache\] key ([0-9a-f]{16}) (\d+x\d+): cache file not found', '\n'.join(text))))
    x4 = [t for t in text if re.match(r'\[shrink\] key ', t)]
    if found or miss:
        piece('INFO', 'Remade interface picture files', '%d used, %d pictures without a file (normal for most)' % (len(found), len(miss)))
        detail.append('picture files used: ' + (', '.join('%s_%s.raw' % k for k in found) or 'none'))
        logo = [k for k in miss if k[1] == '512x128']
        for k in logo:
            detail.append('512x128 picture (menu logo size) WITHOUT a file: its file would be uiscale_hd\\%s_%s.raw' % k)
    for t in x4:
        detail.append(t)
    # every piece that is OFF for a reason other than the ini is trouble
    for state, name, why in pieces:
        if state == 'OFF' and 'uiscale.ini' not in why:
            trouble.append('%s is OFF: %s.' % (name, why))
    # ---- general trouble
    for t in text:
        if re.search(r'COULD NOT|could not be written|not writable|LoadLibrary FAILED|REFUSED|exception|access violation', t, re.I):
            trouble.append(t)
    lim = last(r'watch: frame limiter = ([\d.]+) ms')
    if msgs:
        t0, t1 = msgs[0][0], msgs[-1][0]
        def sec(s):
            h, m, x = s.split(':')
            return int(h) * 3600 + int(m) * 60 + float(x)
        dur = sec(t1) - sec(t0)
        if dur < 0:   # the session ran past midnight (the log has clock times only)
            dur += 86400
        end = 'ended normally' if text and text[-1] == 'unloading' else 'log stops without "unloading" (closed hard or still running)'
        info = 'Log from %s to %s (%.0f s), %s.' % (t0, t1, dur, end)
        if dur < 5 and text and text[-1] == 'unloading':
            trouble.append('The game closed itself %.1f s after starting.' % dur)
    else:
        info = 'No time-stamped lines.'
    return pieces, trouble, detail, info, lim


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    lines = open(sys.argv[1], errors='replace').read().splitlines()
    blocks = starts(lines)
    if '--last' in sys.argv:
        blocks = blocks[-1:]
    for i, b in enumerate(blocks):
        pieces, trouble, detail, info, lim = analyse(b)
        print('=== Start %d of %d: %s' % (i + 1, len(blocks), info))
        w = max(len(p[1]) for p in pieces)
        for state, name, why in pieces:
            print('  %-8s %-*s  %s' % (state, w, name, why))
        if lim:
            print('  (last frame limit seen: %s ms)' % lim.group(1))
        print('  Looks like trouble:' + ('' if trouble else ' nothing'))
        for t in dict.fromkeys(trouble):
            print('    - ' + t)
        if detail:
            print('  Details (addresses and byte checks):')
            for d in detail:
                print('    ' + d)
        print()


if __name__ == '__main__':
    main()
