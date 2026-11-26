// The scaler behind up_scale_image(): xBRZ 1.9 (third_party/xbrz, GPL-3.0, see License.txt there).
// To try another scaler, replace this file (same function).
#include "up9.h"
#include "../../../third_party/xbrz/xbrz.h"

void up_scale_image(const uint32_t *src, int w, int h, uint32_t *dst, int f, bool has_alpha) {
    // argbUnbuffered: no one-time 64 MB colour-distance table (address space is tight in this 32-bit
    // process). Textures without alpha arrive with alpha forced to 255, which gives the rgb result.
    (void)has_alpha;
    xbrz::scale((size_t)f, src, dst, w, h, xbrz::ColorFormat::argbUnbuffered);
}

// Smooth scaler for photographic art: Lanczos-3, premultiplied alpha, result clamped. An axis
// wraps around when its first and last row/column are alike (tiled art such as the stone bar stays
// seamless), else it clamps at the edge.
#include <cmath>
#include <vector>
void up_smooth_image(const uint32_t *src, int w, int h, uint32_t *dst, int f) {
    auto lanczos = [](double x) {
        if (x == 0) return 1.0;
        if (x <= -3 || x >= 3) return 0.0;
        double px = 3.14159265358979323846 * x;
        return 3 * std::sin(px) * std::sin(px / 3) / (px * px);
    };
    auto edges_alike = [&](bool horiz) {
        long d = 0, n = 0;
        int len = horiz ? h : w;
        for (int i = 0; i < len; i++) {
            uint32_t a = horiz ? src[(size_t)i * w] : src[i];
            uint32_t b = horiz ? src[(size_t)i * w + w - 1] : src[(size_t)(h - 1) * w + i];
            for (int s = 0; s < 32; s += 8) d += std::abs((int)((a >> s) & 255) - (int)((b >> s) & 255));
            n += 4;
        }
        return n && d / n < 24;
    };
    bool wrapx = edges_alike(true), wrapy = edges_alike(false);
    // premultiplied float copy
    std::vector<float> pm((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t p = src[i];
        float a = (p >> 24) / 255.f;
        pm[i * 4 + 0] = (p & 255) * a;
        pm[i * 4 + 1] = ((p >> 8) & 255) * a;
        pm[i * 4 + 2] = ((p >> 16) & 255) * a;
        pm[i * 4 + 3] = (float)(p >> 24);
    }
    auto idx = [](int i, int n, bool wrap) { return wrap ? ((i % n) + n) % n : (i < 0 ? 0 : i >= n ? n - 1 : i); };
    int W = w * f, H = h * f;
    // horizontal pass: h rows x W
    std::vector<float> tmp((size_t)W * h * 4);
    for (int X = 0; X < W; X++) {
        double sx = (X + 0.5) / f - 0.5;
        int x0 = (int)std::floor(sx);
        double wt[6], sum = 0;
        for (int k = 0; k < 6; k++) { wt[k] = lanczos(sx - (x0 - 2 + k)); sum += wt[k]; }
        for (int y = 0; y < h; y++) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 6; k++) {
                const float *p = &pm[((size_t)y * w + idx(x0 - 2 + k, w, wrapx)) * 4];
                for (int c = 0; c < 4; c++) acc[c] += (float)(wt[k] / sum) * p[c];
            }
            for (int c = 0; c < 4; c++) tmp[((size_t)y * W + X) * 4 + c] = acc[c];
        }
    }
    for (int Y = 0; Y < H; Y++) {
        double sy = (Y + 0.5) / f - 0.5;
        int y0 = (int)std::floor(sy);
        double wt[6], sum = 0;
        for (int k = 0; k < 6; k++) { wt[k] = lanczos(sy - (y0 - 2 + k)); sum += wt[k]; }
        for (int X = 0; X < W; X++) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 6; k++) {
                const float *p = &tmp[((size_t)idx(y0 - 2 + k, h, wrapy) * W + X) * 4];
                for (int c = 0; c < 4; c++) acc[c] += (float)(wt[k] / sum) * p[c];
            }
            float a = acc[3] < 0 ? 0 : acc[3] > 255 ? 255 : acc[3];
            uint32_t out = (uint32_t)(a + 0.5f) << 24;
            for (int c = 0; c < 3; c++) {
                float v = a > 0.5f ? acc[c] / (a / 255.f) : 0;
                v = v < 0 ? 0 : v > 255 ? 255 : v;
                out |= (uint32_t)(v + 0.5f) << (8 * c);
            }
            dst[(size_t)Y * W + X] = out;
        }
    }
}
