// Makes: the "xbrz" column of tools/art_candidates.py (3x xBRZ of a 1x BGRA file, same scaler and settings as the dll).
// Needs: a native C++20 compiler; xBRZ from third_party/xbrz.
// Built as: g++ -std=c++20 -O2 tools/xbrz3.cpp third_party/xbrz/xbrz.cpp -o tools/xbrz3   (run as: tools/xbrz3 IN.raw OUT.raw W H)
// xbrz3 IN.raw OUT.raw W H : 3x xBRZ (argbUnbuffered, as the dll does) on BGRA bytes (uint32 ARGB little-endian)
#include "../third_party/xbrz/xbrz.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int c, char **v) {
    if (c < 5) return 1;
    int w = atoi(v[3]), h = atoi(v[4]);
    std::vector<uint32_t> s((size_t)w * h), d((size_t)w * h * 9);
    FILE *f = fopen(v[1], "rb"); fread(s.data(), 4, s.size(), f); fclose(f);
    xbrz::scale(3, s.data(), d.data(), w, h, xbrz::ColorFormat::argbUnbuffered);
    f = fopen(v[2], "wb"); fwrite(d.data(), 4, d.size(), f); fclose(f);
}
