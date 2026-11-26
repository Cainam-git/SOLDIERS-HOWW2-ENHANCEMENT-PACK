#!/bin/sh
# Makes: the re-encoded Resource/music/**/*.wma and Resource/video/**/*.wmv of the release (one sound frame per packet).
# Needs: ffmpeg + ffprobe (checked with FFmpeg 9.1 / libavformat 63.16.100: output byte-identical to the release files).
# Called as: sh tools/repack_media.sh GAME_DIR/Resource OUT_DIR   (reads GAME_DIR only; writes OUT_DIR/music, OUT_DIR/video)
#
# Why: the game's original files pack several sound frames into one packet (for example 8 frames of
# 2048 samples = 16384 samples per packet at 44.1 kHz). Under Proton the Windows Media reader hands
# out decoded sound in pieces of at most 16 KB and keeps the rest of a larger buffer; a stop or
# rewind while such a rest is kept can leave its reader thread spinning (the freeze). FFmpeg's WMA v2
# encoder writes exactly one frame (2048 samples) per packet, so no decoded packet is larger than
# 16 KB. The recordings are the same; only the sound is re-encoded (lossy, at an equal or higher
# bit rate). Video pictures are copied untouched.
#
# Bit rates (as in the release):
#   music: original below 128 kb/s -> 128k; 128 to below 160 kb/s -> 160k; 160 kb/s and up -> 192k
#   video: sound track always 192k, picture stream copied (-c:v copy), all streams kept in order (-map 0)
set -e
SRC=${1:?usage: repack_media.sh GAME_DIR/Resource OUT_DIR}
OUT=${2:?usage: repack_media.sh GAME_DIR/Resource OUT_DIR}
case "$(cd "$OUT" 2>/dev/null && pwd)" in "$(cd "$SRC" && pwd)"*) echo "OUT_DIR must not be inside the game folder" >&2; exit 1;; esac

(cd "$SRC/music" && find . -type f -iname '*.wma') | while IFS= read -r f; do
    br=$(ffprobe -v error -select_streams a:0 -show_entries stream=bit_rate -of csv=p=0 "$SRC/music/$f")
    if [ "$br" -lt 128000 ]; then rate=128k; elif [ "$br" -lt 160000 ]; then rate=160k; else rate=192k; fi
    mkdir -p "$OUT/music/$(dirname "$f")"
    ffmpeg -nostdin -hide_banner -v error -y -i "$SRC/music/$f" -c:a wmav2 -b:a $rate "$OUT/music/$f"
done

(cd "$SRC/video" && find . -type f -iname '*.wmv') | while IFS= read -r f; do
    mkdir -p "$OUT/video/$(dirname "$f")"
    ffmpeg -nostdin -hide_banner -v error -y -i "$SRC/video/$f" -map 0 -c:v copy -c:a wmav2 -b:a 192k "$OUT/video/$f"
done
echo "done: $OUT/music, $OUT/video"
