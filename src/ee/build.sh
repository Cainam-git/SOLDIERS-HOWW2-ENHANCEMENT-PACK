#!/bin/sh
# Builds the Enhanced Edition pack: d3d9.dll (32-bit, static runtime) for the Enhanced Edition program R1.28.8.
#   sh src/ee/build.sh [out_dir]          (default: build/ee/)  ->  <out_dir>/d3d9.dll
# Needs: i686-w64-mingw32-g++ / -strip (MinGW-w64, 32-bit). The released file was built with GCC 16.2 and
# GNU Binutils 2.47; other versions build a working dll, but not the same bytes.
#
# Every src/ee/*.cpp is compiled in, plus up/ (interface upscaler and sharp text)
# and xBRZ from third_party/. The modern key table comes from src/keys/modern_keys.inc (made by
# src/keys/make_preset.py; the generated file is in the repository).
#
# Reproducing the released dll byte for byte: set the build stamps. Enhancement Pack 1.2
# (sha256 db8231c95737c8d501fd029a4f10e94ebc32e9ad8538850ad7ade836b43eecad):
#   BUILD_TIME="2026-11-26 12:30:00" LINK_TIME="2026-11-26 12:30:00" BUILD_TZ=CET-1 sh src/ee/build.sh
# Without them all stamps are "now".
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$ROOT/build/ee}"
mkdir -p "$OUT/obj"
OUT="$(cd "$OUT" && pwd)"
OBJ="$OUT/obj"

# GCC takes __DATE__/__TIME__ from SOURCE_DATE_EPOCH as UTC; GNU ld and strip take the PE time stamps from it.
CC_EPOCH=""; LD_EPOCH=""; EX_EPOCH=""
[ -n "$BUILD_TIME" ] && CC_EPOCH=$(date -u -d "$BUILD_TIME" +%s)
[ -n "$LINK_TIME" ] && LD_EPOCH=$(TZ=${BUILD_TZ:-UTC} date -d "$LINK_TIME" +%s)
EX_EPOCH=$LD_EPOCH
[ -n "$EXPORT_TIME" ] && EX_EPOCH=$(TZ=${BUILD_TZ:-UTC} date -d "$EXPORT_TIME" +%s)

# xBRZ: its assert messages carry the source path as the compiler saw it. The path below is relative to the
# repository root and padded with "./" to the length the 1.2 release build used, so the dll holds no folder
# name of the build machine and a build from any checkout gives the same bytes.
XBRZ_SRC="./././././././././././././././././././././/third_party/xbrz/xbrz.cpp"
(cd "$ROOT"; ulimit -v 8388608 2>/dev/null || true
 i686-w64-mingw32-g++ -std=c++20 -O2 -c "$XBRZ_SRC" -o "$OBJ/xbrz.o")

cd "$HERE"
SRC="$(LC_ALL=C ls *.cpp) up/up9.cpp up/up9_xbrz.cpp up/hdtext9.cpp"
# each module replaces an empty stand-in in main.cpp when it is present
DEFS="-DEE_HAVE_SCALE3 -DEE_HAVE_SMALL -DEE_HAVE_D3DHOOK -DEE_HAVE_BRAND -DEE_HAVE_MENUS -DEE_HAVE_PROFILE -DUP_HDTEXT"
CXX="i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wno-unused-function -I. -static -static-libgcc -static-libstdc++ -shared"

compile() {  # $1 = source, $2 = object
    if [ -n "$CC_EPOCH" ]; then SOURCE_DATE_EPOCH=$CC_EPOCH $CXX $DEFS -c "$1" -o "$2"
    else (unset SOURCE_DATE_EPOCH; $CXX $DEFS -c "$1" -o "$2"); fi
}
OBJS=""
for s in $SRC; do
    o="$OBJ/$(basename "$s" .cpp).o"
    (ulimit -v 8388608 2>/dev/null || true; compile "$s" "$o")
    OBJS="$OBJS $o"
done
OBJS="$OBJS $OBJ/xbrz.o"

# GNU ld picks a dll's preferred load address from the output path unless it is given; fixed here to the
# value of the 1.2 release dll.
LINK="$CXX -o $OUT/d3d9.dll $OBJS d3d9.def -Wl,--enable-stdcall-fixup -Wl,--image-base=0x62140000 -luuid -lole32 -luser32 -lgdi32"
STRIP="i686-w64-mingw32-strip $OUT/d3d9.dll"
if [ -n "$LD_EPOCH" ]; then SOURCE_DATE_EPOCH=$EX_EPOCH $LINK; SOURCE_DATE_EPOCH=$LD_EPOCH $STRIP
else (unset SOURCE_DATE_EPOCH; $LINK; $STRIP); fi
echo "built $OUT/d3d9.dll"
sha256sum "$OUT/d3d9.dll" 2>/dev/null || true
