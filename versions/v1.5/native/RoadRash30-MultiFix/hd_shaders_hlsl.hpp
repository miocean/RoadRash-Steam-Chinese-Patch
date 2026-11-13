// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// HLSL of the Direct3D 11 back end. It is compiled at build time by tools/compile_shaders.cpp (the result is
// embedded in the DLL, so no shader compiler is needed at run time, which matters under Wine/Proton) and kept
// here as the fallback that is compiled at run time when the pre-compiled blobs are not available.
#pragma once
namespace rrhd {
inline const char* gpuHlsl() {
    return R"HLSL(
cbuffer Frame : register(b0) {
    float viewW, viewH, sceneScale, halfW;
    int horizonW, horizonRows, horizonScroll, horizonY;
    float uiScale, uiOx, uiOy, uiFlat;
    float uiX0, uiX1, uiY0, uiY1;
    float backdropFilter, presentW, presentH, padC;
    float presentX, presentY, padD, padE;
};
struct Quad { float4 px; float4 py; float4 pu; float4 pv; uint4 info; };
Texture2D<uint>   legacyTex  : register(t0);
Texture2D<uint>   horizonTex : register(t1);
Texture2D<float4> paletteTex : register(t2);
Texture2D<uint>   staticTex  : register(t3);
Texture2D<uint>   frameTex   : register(t4);
StructuredBuffer<Quad> quads : register(t5);
Texture2D<float4> uiTex      : register(t6);
Texture2D<float4> srcTex     : register(t7);
SamplerState pointSamp : register(s0);

float4 pal(uint i) { return float4(paletteTex.Load(int3(i, 0, 0)).rgb, 1); }
int wrapi(int x, int n) { int r = x % n; return r < 0 ? r + n : r; }

float4 VSFull(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSPresent(float4 pos : SV_Position) : SV_Target {
    // The back buffer can differ from the scene target for a moment (window just resized): always stretch the whole scene over it.
    return float4(srcTex.SampleLevel(pointSamp, (pos.xy - float2(presentX, presentY)) / float2(presentW, presentH), 0).rgb, 1);
}
float4 PSCopyCpu(float4 pos : SV_Position) : SV_Target {
    return float4(srcTex.Load(int3(int2(pos.xy), 0)).rgb, 1);
}

uint backdropIndex(int bx, int oldY) {
    oldY = clamp(oldY, 0, 479);
    int hy = oldY - horizonY;
    if (horizonRows > 0 && hy >= 0 && hy < horizonRows)
        return horizonTex.Load(int3(wrapi(bx - horizonScroll, horizonW), hy, 0)).r;
    return legacyTex.Load(int3(clamp(bx, 0, 639), oldY, 0)).r;
}
float4 PSBackdrop(float4 pos : SV_Position) : SV_Target {
    int x = int(pos.x), y = int(pos.y);
    float fy = (y + 0.5) / sceneScale;
    float fx = (x + 0.5 - halfW) / sceneScale + 320.0;
    if (backdropFilter < 0.5)
        return pal(backdropIndex(int(floor(fx)), int(floor(fy))));
    // Bilinear sky / panorama (texture filtering also covers the backdrop)
    float sx = fx - 0.5, sy = fy - 0.5;
    int ix = int(floor(sx)), iy = int(floor(sy));
    float wx = sx - ix, wy = sy - iy;
    float3 c00 = pal(backdropIndex(ix, iy)).rgb, c10 = pal(backdropIndex(ix + 1, iy)).rgb;
    float3 c01 = pal(backdropIndex(ix, iy + 1)).rgb, c11 = pal(backdropIndex(ix + 1, iy + 1)).rgb;
    return float4(lerp(lerp(c00, c10, wx), lerp(c01, c11, wx), wy), 1);
}

struct QOut { float4 pos : SV_Position; nointerpolation uint q : QUAD; };
QOut VSQuad(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    Quad q = quads[iid];
    float x0 = floor(min(min(q.px.x, q.px.y), min(q.px.z, q.px.w))), x1 = ceil(max(max(q.px.x, q.px.y), max(q.px.z, q.px.w))) + 1;
    float y0 = floor(min(min(q.py.x, q.py.y), min(q.py.z, q.py.w))), y1 = ceil(max(max(q.py.x, q.py.y), max(q.py.z, q.py.w))) + 1;
    float2 c[6] = { float2(x0, y0), float2(x1, y0), float2(x0, y1), float2(x0, y1), float2(x1, y0), float2(x1, y1) };
    float2 p = c[vid];
    QOut o;
    o.pos = float4(p.x / viewW * 2 - 1, 1 - p.y / viewH * 2, 0, 1);
    o.q = iid;
    return o;
}

struct Cover { bool ok; float u; float v; float2 dx; float2 dy; };   // dx, dy: texels per output pixel
Cover evalQuad(Quad q, float sx, float sy) {
    float ex[4] = { q.px.x, q.px.y, q.px.z, q.px.w };
    float ey[4] = { q.py.x, q.py.y, q.py.z, q.py.w };
    float eu[4] = { q.pu.x, q.pu.y, q.pu.z, q.pu.w };
    float ev[4] = { q.pv.x, q.pv.y, q.pv.z, q.pv.w };
    float hx[4], hu[4], hv[4], su[4], sv[4]; int n = 0;
    [unroll] for (int e = 0; e < 4; ++e) {
        int b = (e + 1) & 3;
        float ay = ey[e], by = ey[b];
        if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
            float t = (sy - ay) / (by - ay);
            hx[n] = ex[e] + (ex[b] - ex[e]) * t; hu[n] = eu[e] + (eu[b] - eu[e]) * t; hv[n] = ev[e] + (ev[b] - ev[e]) * t;
            su[n] = (eu[b] - eu[e]) / (by - ay); sv[n] = (ev[b] - ev[e]) / (by - ay);   // texels per output row along this edge
            ++n;
        }
    }
    // insertion sort by x
    [unroll] for (int a = 1; a < 4; ++a) {
        [unroll] for (int k = a; k > 0; --k) {
            if (k < n && hx[k] < hx[k - 1]) {
                float tx = hx[k]; hx[k] = hx[k - 1]; hx[k - 1] = tx;
                float tu = hu[k]; hu[k] = hu[k - 1]; hu[k - 1] = tu;
                float tv = hv[k]; hv[k] = hv[k - 1]; hv[k - 1] = tv;
                float tsu = su[k]; su[k] = su[k - 1]; su[k - 1] = tsu;
                float tsv = sv[k]; sv[k] = sv[k - 1]; sv[k - 1] = tsv;
            }
        }
    }
    Cover c; c.ok = false; c.u = 0; c.v = 0; c.dx = float2(0, 0); c.dy = float2(0, 0);
    [unroll] for (int s = 0; s < 4; s += 2) {
        if (s + 1 < n && hx[s + 1] > hx[s] && sx >= hx[s] && sx < hx[s + 1]) {
            float f = (sx - hx[s]) / (hx[s + 1] - hx[s]);
            c.u = hu[s] + (hu[s + 1] - hu[s]) * f; c.v = hv[s] + (hv[s + 1] - hv[s]) * f;
            c.dx = float2(hu[s + 1] - hu[s], hv[s + 1] - hv[s]) / (hx[s + 1] - hx[s]);
            c.dy = float2(su[s] + (su[s + 1] - su[s]) * f, sv[s] + (sv[s + 1] - sv[s]) * f);
            c.ok = true;
        }
    }
    return c;
}

// Vertical extent of the quad on the column x = sx: (top, bottom), or (1, 0) when the column misses it.
float2 vspan(Quad q, float sx) {
    float ex[4] = { q.px.x, q.px.y, q.px.z, q.px.w };
    float ey[4] = { q.py.x, q.py.y, q.py.z, q.py.w };
    float lo = 1e9, hi = -1e9;
    [unroll] for (int e = 0; e < 4; ++e) {
        int b = (e + 1) & 3;
        if ((ex[e] <= sx && ex[b] > sx) || (ex[b] <= sx && ex[e] > sx)) {
            float y = ey[e] + (ey[b] - ey[e]) * ((sx - ex[e]) / (ex[b] - ex[e]));
            lo = min(lo, y); hi = max(hi, y);
        }
    }
    return hi >= lo ? float2(lo, hi) : float2(1, 0);
}

uint texel(bool useFrame, int x, int y) {
    return useFrame ? frameTex.Load(int3(x, y, 0)).r : staticTex.Load(int3(x, y, 0)).r;
}
// Bilinear sample (transparent texels carry no weight). Returns rgb and the total weight.
float4 bilinearAt(float u, float v, int ax, int ay0, int tw, int th, bool useFrame, bool transparent) {
    float fu = u - 0.5, fv = v - 0.5;
    int ix = int(floor(fu)), iy = int(floor(fv));
    float wx = fu - ix, wy = fv - iy;
    int xa = clamp(ix, 0, tw - 1), xb = clamp(ix + 1, 0, tw - 1), ya = clamp(iy, 0, th - 1), yb = clamp(iy + 1, 0, th - 1);
    uint i00 = texel(useFrame, ax + xa, ay0 + ya), i10 = texel(useFrame, ax + xb, ay0 + ya);
    uint i01 = texel(useFrame, ax + xa, ay0 + yb), i11 = texel(useFrame, ax + xb, ay0 + yb);
    float w00 = (1 - wx) * (1 - wy), w10 = wx * (1 - wy), w01 = (1 - wx) * wy, w11 = wx * wy;
    if (transparent) { w00 *= (i00 != 0); w10 *= (i10 != 0); w01 *= (i01 != 0); w11 *= (i11 != 0); }
    float tot = w00 + w10 + w01 + w11;
    if (tot <= 0) return float4(0, 0, 0, 0);
    float3 c = (pal(i00).rgb * w00 + pal(i10).rgb * w10 + pal(i01).rgb * w01 + pal(i11).rgb * w11) / tot;
    return float4(c, tot);
}

float4 PSQuad(QOut i) : SV_Target {
    Quad q = quads[i.q];
    float sx = i.pos.x, sy = i.pos.y;
    uint flags = q.info.z;
    float qa = float((flags >> 8) & 255u) / 255.0;
    Cover c0 = evalQuad(q, sx, sy);
    // Strips thinner than one output row (water and road far away are drawn as many of them) cover only part of
    // a row: blend them in by that coverage instead of an all-or-nothing row, which produced streaks.
    if ((flags & 16u) != 0) {
        float2 sp = vspan(q, sx);
        if (sp.y > sp.x && sp.y - sp.x < 1.0) {
            float top = max(sp.x, sy - 0.5), bot = min(sp.y, sy + 0.5);
            if (bot <= top) discard;
            qa *= saturate(bot - top);
            c0 = evalQuad(q, sx, 0.5 * (top + bot));
        }
    }
    if (!c0.ok) discard;
    float u = c0.u, v = c0.v;
    if (flags & 4u) return float4(pal(q.info.w).rgb, qa);
    int ax = int(q.info.x & 0xffff), ay0 = int(q.info.x >> 16), tw = int(q.info.y & 0xffff), th = int(q.info.y >> 16);
    bool transparent = (flags & 1u) != 0, useFrame = (flags & 8u) != 0;
    int tx = clamp(int(u), 0, tw - 1), ty = clamp(int(v), 0, th - 1);
    uint idx = texel(useFrame, ax + tx, ay0 + ty);
    if (transparent && idx == 0) discard;            // nearest decides coverage (as on the CPU)
    if ((flags & 18u) == 0) return float4(pal(idx).rgb, qa);   // texture filtering off: plain nearest
    // Footprint of one output pixel in texels. Where a texture is squeezed (road and water towards the
    // horizon, far objects) one nearest/bilinear sample per pixel skips texels and shimmers or streaks, so
    // the footprint is averaged along its longer axis instead (anisotropic filtering).
    float2 dx = c0.dx, dy = c0.dy;
    float lx = length(dx), ly = length(dy);
    float2 d = lx > ly ? dx : dy;
    // A pixel can cover more than the whole texture (thin far-away strips such as the water): never reach past
    // the texture, average all of it instead.
    float2 ext = float2(max(tw - 2, 1), max(th - 2, 1));
    float shrink = min(1.0, min(ext.x / max(abs(d.x), 1e-4), ext.y / max(abs(d.y), 1e-4)));
    if (shrink < 1.0) { d *= shrink; u = tw * 0.5; v = th * 0.5; }
    float L = max(lx, ly) * shrink;
    if ((flags & 16u) != 0 && L > 1.25) {
        int n = int(min(ceil(L), 32.0));
        float3 sum = float3(0, 0, 0); float wsum = 0;
        for (int k = 0; k < n; ++k) {
            float t = (k + 0.5) / n - 0.5;
            float4 s = bilinearAt(u + d.x * t, v + d.y * t, ax, ay0, tw, th, useFrame, transparent);
            sum += s.rgb * s.a; wsum += s.a;
        }
        if (wsum <= 0) return float4(pal(idx).rgb, qa);
        return float4(sum / wsum, qa);
    }
    if ((flags & 2u) == 0) return float4(pal(idx).rgb, qa);
    float4 b = bilinearAt(u, v, ax, ay0, tw, th, useFrame, transparent);
    if (b.a <= 0) return float4(pal(0).rgb, qa);
    return float4(b.rgb, qa);
}

float4 PSUi(float4 pos : SV_Position) : SV_Target {
    if (pos.x < uiX0 || pos.x >= uiX1 || pos.y < uiY0 || pos.y >= uiY1) discard;
    float fx = (pos.x - uiOx) / uiScale - 0.5, fy = (pos.y - uiOy) / uiScale - 0.5;
    if (uiFlat > 0.5) {                            // large overlays stay flat
        float4 t = uiTex.Load(int3(clamp(int(floor(fx + 0.5)), 0, 639), clamp(int(floor(fy + 0.5)), 0, 479), 0));
        if (t.a < 0.5) discard;
        return float4(t.rgb, 1);
    }
    int ix = int(floor(fx)), iy = int(floor(fy));
    float wx = fx - ix, wy = fy - iy;
    int xa = clamp(ix, 0, 639), xb = clamp(ix + 1, 0, 639), ya = clamp(iy, 0, 479), yb = clamp(iy + 1, 0, 479);
    float4 t00 = uiTex.Load(int3(xa, ya, 0)), t10 = uiTex.Load(int3(xb, ya, 0)), t01 = uiTex.Load(int3(xa, yb, 0)), t11 = uiTex.Load(int3(xb, yb, 0));
    float m00 = t00.a > 0.5, m10 = t10.a > 0.5, m01 = t01.a > 0.5, m11 = t11.a > 0.5;
    if (m00 + m10 + m01 + m11 == 0) discard;
    float w00 = (1 - wx) * (1 - wy) * m00, w10 = wx * (1 - wy) * m10, w01 = (1 - wx) * wy * m01, w11 = wx * wy * m11;
    float total = w00 + w10 + w01 + w11;
    float3 col = (t00.rgb * w00 + t10.rgb * w10 + t01.rgb * w01 + t11.rgb * w11) / total;
    if (m00 * m10 * m01 * m11 > 0.5) return float4(col, 1);
    float alpha = saturate((total * 256 - 90) / 76);
    if (alpha <= 0) discard;
    return float4(col, alpha);
}
)HLSL";
}
// Entry points: name, shader model, vertex shader?
struct GpuShaderEntry { const char* name; const char* target; bool vertex; };
inline const GpuShaderEntry* gpuShaderEntries(int& count) {
    static const GpuShaderEntry list[] = {
        { "VSFull", "vs_5_0", true }, { "VSQuad", "vs_5_0", true },
        { "PSBackdrop", "ps_5_0", false }, { "PSQuad", "ps_5_0", false },
        { "PSCopyCpu", "ps_5_0", false }, { "PSUi", "ps_5_0", false },
        { "PSPresent", "ps_5_0", false } };
    count = int(sizeof list / sizeof list[0]);
    return list;
}
}  // namespace rrhd
