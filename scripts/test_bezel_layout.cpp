// Checks the bezel layout math (runtime\src\bezel_layout.h) without starting the game.
// Build + run (PowerShell, LLVM installed):
//   & "C:\Program Files\LLVM\bin\clang-cl.exe" /nologo /std:c++20 /EHsc /O2 /I runtime\src scripts\test_bezel_layout.cpp /Fe:$env:TEMP\test_bezel_layout.exe
//   & $env:TEMP\test_bezel_layout.exe
// Prints "ok" and the number of checks, or the first failures.
#include <cmath>
#include <cstdio>

#include "bezel_layout.h"

using namespace adam::bezels::layout;

static int failures = 0, checks = 0;

static void expect(bool ok, const char* what, double a = 0, double b = 0, double c = 0) {
    checks++;
    if (!ok && failures++ < 20) {
        printf("FAIL %s (%g %g %g)\n", what, a, b, c);
    }
}

static double wrap(double v, double len) {
    double r = std::fmod(v, len);
    return r < 0 ? r + len : r;
}

// Same position modulo the picture size (the joint itself may come out as 0 or len).
static bool same_coord(double a, double b, double len) {
    double d = std::fabs(a - b);
    return d < 1e-6 || std::fabs(d - len) < 1e-6;
}

int main() {
    const double pictures[][2] = { { 288, 144 }, { 320, 240 }, { 144, 36 }, { 88, 154 } };
    const double sizes[][2] = { { 1712, 963 }, { 1920, 1080 }, { 2560, 1080 }, { 3440, 1440 }, { 1000, 700 } };
    const double tiles[] = { 100, 75, 50, 25, 10 };
    for (auto& pic : pictures) {
        double pw = pic[0], ph = pic[1];
        for (auto& size : sizes) {
            double w = size[0], h = size[1];
            double bar = (w - h * 4.0 / 3.0) / 2.0;
            for (double tile : tiles) {
                double scale = tile_scale(w, h, pw, ph, tile);
                for (double px = 0; px <= 100.0001; px += 12.5) {
                    for (double py = 0; py <= 100.0001; py += 12.5) {
                        double tile_w = pw * scale, tile_h = ph * scale;
                        double origin_x = (w - tile_w) / 2.0 + (px - 50.0) / 100.0 * tile_w;
                        double origin_y = (h - tile_h) / 2.0 + (py - 50.0) / 100.0 * tile_h;
                        for (int side = 0; side < 2; side++) {
                            double x = side == 0 ? 0 : w - bar;
                            Panel p = place(x, bar, w, h, pw, ph, px, py, tile);
                            double area = 0;
                            for (const Segment& s : p.segments) {
                                expect(s.src_x >= -1e-9 && s.src_x + s.src_w <= pw + 1e-6, "inside picture x", s.src_x, s.src_w);
                                expect(s.src_y >= -1e-9 && s.src_y + s.src_h <= ph + 1e-6, "inside picture y", s.src_y, s.src_h);
                                area += s.width * s.height;
                            }
                            expect(std::fabs(area - 1.0) < 1e-6, "segments cover the panel", area, tile, w);
                            // Sample points: the segment containing the point shows the expected picture pixel.
                            for (int i = 0; i < 9; i++) {
                                for (int j = 0; j < 9; j++) {
                                    double fx = (i + 0.37) / 9.0, fy = (j + 0.61) / 9.0;
                                    double col = x + fx * bar, row = fy * h;
                                    double want_u = wrap((col - origin_x) / scale, pw), want_v = wrap((row - origin_y) / scale, ph);
                                    bool found = false;
                                    for (const Segment& s : p.segments) {
                                        if (fx >= s.left - 1e-9 && fx <= s.left + s.width + 1e-9 && fy >= s.top - 1e-9 &&
                                            fy <= s.top + s.height + 1e-9) {
                                            double u = s.src_x + (fx - s.left) / s.width * s.src_w;
                                            double v = s.src_y + (fy - s.top) / s.height * s.src_h;
                                            expect(same_coord(u, want_u, pw) && same_coord(v, want_v, ph), "pixel", u - want_u, v - want_v, tile);
                                            found = true;
                                            break;
                                        }
                                    }
                                    expect(found, "point covered", fx, fy);
                                }
                            }
                        }
                    }
                }
                // 0 and 100 look the same, in both directions.
                double x = 0;
                Panel a = place(x, bar, w, h, pw, ph, 0, 0, tile), b = place(x, bar, w, h, pw, ph, 100, 100, tile);
                const Segment& sa = a.segments[0];
                const Segment& sb = b.segments[0];
                expect(same_coord(sa.src_x, sb.src_x, pw) && same_coord(sa.src_y, sb.src_y, ph), "0 == 100", sa.src_x, sb.src_x);
            }
            // tile 100, 50 / 50 = the earlier centered cover layout for a 2:1 picture: one segment, rows = the whole height.
            if (pw == 288 && ph == 144) {
                double scale = std::max(w / pw, h / ph);
                double old_x = (0 - (w - pw * scale) / 2.0) / scale;
                double old_y = (0 - (h - ph * scale) / 2.0) / scale;
                Panel p = place(0, bar, w, h, pw, ph, 50, 50, 100);
                if (old_x >= 0 && old_x + bar / scale <= pw && old_y >= 0 && old_y + h / scale <= ph + 1e-9) {
                    expect(p.segments.size() == 1 && std::fabs(p.segments[0].src_x - old_x) < 1e-9 &&
                        std::fabs(p.segments[0].src_y - old_y) < 1e-9, "centered as before", old_x, old_y);
                }
            }
        }
    }
    if (failures == 0) {
        printf("ok, %d checks\n", checks);
    } else {
        printf("%d of %d checks failed\n", failures, checks);
    }
    return failures == 0 ? 0 : 1;
}
