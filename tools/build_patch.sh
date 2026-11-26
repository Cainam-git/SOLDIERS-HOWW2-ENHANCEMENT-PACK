#!/bin/sh
# Builds the Enhancement Pack release: three zips that are unpacked INTO the GOG game folder (overwrite: yes).
#   first run:   sh tools/build_patch.sh --pack DIR --ee DIR --hd DIR --program DIR
#   later runs:  sh tools/build_patch.sh                (the inputs are kept in disc/inputs.conf)
#   check:       sh tools/build_patch.sh --verify GAME  (a game folder the zips were unpacked into: every file
#                present with its bytes, and no two names that differ only in case)
# Output in disc/ (git-ignored): ...-part1of3.zip (pack, music and videos, program files, README, LICENSE),
# part2of3 (Enhanced Edition data), part3of3 (HD textures), PARTS.sha256, FILES.sha256, BUILD-NOTES.txt.
# A group too large for one zip below 1.9 GiB is split and the parts renumbered "of N". Same inputs = same bytes.
#
# Inputs (each checked by sha256 against the lists in tools/release before anything is written):
#   --pack     d3d9.dll (sh src/ee/build.sh), uiscale.ini (src/ee/uiscale.ini), hdtext_fonts.bin
#              (tools/fontdump.py), uiscale_hd/ (tools/hdcache.py + art_straighten.py and the single-picture
#              scripts tame*.py, make3x.py, xredraw.py, bottombar/; tools/brand/ for the logo art and
#              splash4k_ee.bin), resource/interface/logo.tga + logo_small.tga (tools/brand/), and the re-encoded
#              music and videos under resource/ and resource.en.intro/ (tools/repack_media.sh)
#   --ee       an installed Enhanced Edition R1.28.8 game folder (88 files, tools/release/layer1_ee_R1.28.8.sha256)
#   --hd       the HD Project 0.51 mod folder; its resource/ goes in as loose files (layer4_hd_project_0.51.sha256)
#   --program  a folder with the program files that go beside the game
#
# Path case: every folder and file name a GOG install already has is written with GOG's spelling (Resource/...,
# gog_1.28.3E_18993.sha256), so on Linux the files merge into GOG's folders and overwrite GOG's files; other
# names keep their own spelling. The game reads only resource.pak, resource.<language>*.pak and the loose
# folders resource/ and resource.<language>/: game.pak, expansion.pak, localize.pak and the editor stay unused.
# GOG files left in the loose folder are covered at their exact path: the two credits pictures (logo_1c.bmp,
# logo_cm.bmp: the Enhanced Edition's own, from resource.pak) and 116 music files in Resource/music (our
# re-encoded copy, the same file as under resource.en.intro). BUILD-NOTES.txt lists every case.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ulimit -v 8388608 2>/dev/null
exec python3 -I "$ROOT/tools/release/build_zips.py" "$@"
