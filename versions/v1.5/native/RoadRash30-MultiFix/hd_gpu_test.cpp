// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Direct3D 11 back end without the game: a hidden window, the pre-compiled shaders, a flat quad drawn into the
// scene target and read back, in both swap-chain modes (child window = Windows default, direct = Wine/Proton).
#include "hd_gpu.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
}
static void say(const char* m) { std::printf("  gpu: %s\n", m); }

static void run(bool childWindow) {
    HWND window = CreateWindowExW(0, L"STATIC", L"rrhd gpu test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(window != nullptr, "test window");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    {
        rrhd::GpuRenderer gpu;
        gpu.log = &say;
        gpu.useChildWindow = childWindow;
        require(gpu.init(window, 640, 480), "Direct3D 11 device, swap chain and shaders");
        require(gpu.ready(), "renderer ready");
        uint32_t palette[256]{};
        palette[7] = 0x00336699;
        gpu.setPalette(palette);
        std::vector<uint8_t> legacy(640 * 480, 0);
        gpu.beginFrame();
        gpu.setHorizon(nullptr, 0, 0);
        gpu.setLegacyBackdrop(legacy.data());
        rrhd::GpuFrameInfo info{};
        info.viewW = 640; info.viewH = 480; info.sceneScale = 1; info.halfW = 320;
        rrhd::GpuQuad quad{};
        const float xs[4] = { 100, 200, 200, 100 }, ys[4] = { 100, 100, 200, 200 };
        for (int i = 0; i < 4; ++i) { quad.x[i] = xs[i]; quad.y[i] = ys[i]; }
        quad.flags = 4u | (255u << 8);   // flat colour, opaque
        quad.color = 7;
        gpu.drawScene(info, &quad, 1);
        std::vector<uint32_t> pixels;
        require(gpu.readScene(pixels) && pixels.size() == 640u * 480u, "scene read-back");
        require((pixels[150 * 640 + 150] & 0xffffff) == 0x336699, "flat quad colour comes from the palette");
        require((pixels[50 * 640 + 50] & 0xffffff) == 0x000000, "outside the quad stays the backdrop");
        // drawScene starts from the backdrop (black): half-transparent white over black is mid grey
        quad.flags = 4u | (128u << 8);
        palette[7] = 0x00ffffff;
        gpu.setPalette(palette);
        gpu.drawScene(info, &quad, 1);
        require(gpu.readScene(pixels), "second read-back");
        uint32_t blended = pixels[150 * 640 + 150] & 0xff;
        require(blended > 0x70 && blended < 0x90, "alpha blending of a half-transparent quad");
        require(gpu.present(0), "present");
        // the swap chain can be released and created again (menus and videos do this)
        gpu.releaseWindow();
        require(!gpu.ready(), "released");
        require(gpu.init(window, 640, 480) && gpu.ready(), "swap chain created again");
    }
    DestroyWindow(window);
}

int main() {
#ifdef RRHD_PRECOMPILED_SHADERS
    std::printf("shaders: pre-compiled DXBC embedded\n");
#else
    std::fprintf(stderr, "FAILED: the shaders were not pre-compiled (build/hd_shaders.inc missing)\n");
    return 1;
#endif
    run(true);
    run(false);
    std::printf("gpu: pre-compiled shaders, flat quad, alpha, present and re-creation passed (child-window and direct swap chain)\n");
    return 0;
}
