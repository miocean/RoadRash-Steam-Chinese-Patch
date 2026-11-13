// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rrhd {
struct Vertex { double x, y, u = 0, v = 0; };
struct Texture {
    const uint8_t* pixels = nullptr;
    int width = 0, height = 0;
    bool transparent = false;
};

// Scan-convert the original ordered quads at the destination resolution.
// No 480-row scratch arrays, 16-bit screen coordinates or enlarged old frame.
inline void quad(uint8_t* out, int width, int height, const Vertex (&p)[4],
                 const Texture& tex, uint8_t color, int clipTop=0, int clipBottom=0x7fffffff) {
    double lo = p[0].y, hi = lo;
    for (const auto& v : p) { lo = std::min(lo, v.y); hi = std::max(hi, v.y); }
    clipBottom=std::min(height,clipBottom);
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi < clipTop || lo >= clipBottom) return;
    int begin = int(std::max(double(clipTop), std::ceil(lo - 0.5)));
    int end = int(std::min(double(clipBottom), std::ceil(hi - 0.5)));
    for (int y = begin; y < end; ++y) {
        Vertex hits[4]; int n = 0;
        const double sampleY = y + 0.5;
        for (int i = 0; i < 4; ++i) {
            const auto& a = p[i]; const auto& b = p[(i + 1) & 3];
            if ((a.y <= sampleY && b.y > sampleY) || (b.y <= sampleY && a.y > sampleY)) {
                const double t = (sampleY - a.y) / (b.y - a.y);
                hits[n++] = {a.x + (b.x-a.x)*t, sampleY,
                             a.u + (b.u-a.u)*t, a.v + (b.v-a.v)*t};
            }
        }
        std::sort(hits, hits + n, [](const Vertex& a, const Vertex& b) { return a.x < b.x; });
        for (int i = 0; i + 1 < n; i += 2) {
            const auto& a = hits[i]; const auto& b = hits[i+1];
            if (!(b.x > a.x) || b.x < 0 || a.x >= width) continue;
            int x0 = int(std::max(0.0, std::ceil(a.x - 0.5)));
            int x1 = int(std::min(double(width), std::ceil(b.x - 0.5)));
            uint8_t* row = out + size_t(y) * width;
            if (!tex.pixels) {
                if (x1 > x0) {
                    std::memset(row + x0, color, x1-x0);
                }
                continue;
            }
            const double du = (b.u-a.u)/(b.x-a.x), dv = (b.v-a.v)/(b.x-a.x);
            double u = a.u + (x0 + 0.5 - a.x)*du, v = a.v + (x0 + 0.5 - a.x)*dv;
            for (int x = x0; x < x1; ++x, u += du, v += dv) {
                // Truncation equals floor for in-range texture coordinates;
                // negatives clamp to zero either way. Avoid two library floor
                // calls for every covered pixel in this 32-bit executable.
                int tx = std::clamp(int(u), 0, tex.width-1);
                int ty = std::clamp(int(v), 0, tex.height-1);
                uint8_t index = tex.pixels[size_t(ty)*tex.width + tx];
                if (index || !tex.transparent) {
                    row[x] = index;
                }
            }
        }
    }
}

// Packed 8-bit-per-channel linear interpolation, weight 0..256.
inline uint32_t lerp8(uint32_t a, uint32_t b, unsigned w) {
    uint32_t rb = (((a & 0xff00ffu) * (256 - w) + (b & 0xff00ffu) * w) >> 8) & 0xff00ffu;
    uint32_t g  = (((a & 0x00ff00u) * (256 - w) + (b & 0x00ff00u) * w) >> 8) & 0x00ff00u;
    return rb | g;
}

}
