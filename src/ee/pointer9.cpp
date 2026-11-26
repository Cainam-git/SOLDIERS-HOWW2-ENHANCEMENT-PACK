// pointer9: smooth enlargement of the mouse pointers.
//
// scale3.cpp enlarges every pointer DXVK sets (32x32 Win32 cursor made from the game's Direct3D
// cursor) by the interface factor, caches it per pointer and scales the hotspot. Plain pixel repetition
// shows f x f blocks, so the picture goes through the interface art's own scaler instead:
// up_scale_image = xBRZ (argbUnbuffered, alpha handled by xBRZ itself) for whole factors 2..6, the 1-bit mask
// all set so the alpha decides (scale3 does the same).
//   ee_pointer_enlarge(src, w, h, dst, f): src w x h ARGB, dst (w*f) x (h*f).
// xBRZ when the sharp interface is on ([EE] UiUpscale=1) and 2 <= f <= 6; otherwise pixel repetition.
// There is no remade pointer art, so no file lookup here.
// No Windows calls, so this file also builds natively for an offline test.
#include <stdint.h>
#include "ee.h"

void up_scale_image(const uint32_t *src, int w, int h, uint32_t *dst, int f, bool has_alpha);   // up/up9_xbrz.cpp

extern "C" void ee_pointer_enlarge(const uint32_t *src, int w, int h, uint32_t *dst, int f) {
    static int smooth = -1;
    if (smooth < 0) {
        smooth = ee_ini_int("UiUpscale", 1) != 0;
        ee_log("pointer9: pointers enlarged %s", smooth ? "smooth (xBRZ, as pack 1.1)" : "by pixel repetition (UiUpscale=0)");
    }
    if (smooth && f >= 2 && f <= 6) {
        up_scale_image(src, w, h, dst, f, true);
        return;
    }
    const int nw = w * f, nh = h * f;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) dst[y * nw + x] = src[(y / f) * w + x / f];
}
