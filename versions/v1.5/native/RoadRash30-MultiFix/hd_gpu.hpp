// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Direct3D 11 back end for the HD renderer. It lives inside RoadRashMultiFix.dll (loaded through the
// ddraw.dll proxy), so the game executable is still untouched. Everything here is optional: when
// the device, the swap chain or a shader cannot be created, ready() stays false and the caller
// keeps using the CPU/GDI path.
//
// Per frame (gameplay):  backdrop (panorama / legacy picture)  ->  textured/flat quads in draw
// order (scan-conversion identical to rrhd::quad, with optional bilinear filtering)  ->  smoothed
// UI layer  ->  scaled to the window and presented. Menus: a CPU-composed RGBA picture replaces the
// first two steps. Textures live in R8_UINT atlases and the palette lookup happens in the shader.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>        // waitable swap chain (1.3) and tearing support (1.5)
#include <d3dcompiler.h>
#include "hd_shaders_hlsl.hpp"
#if defined(__has_include)
#if __has_include("hd_shaders.inc")
#include "hd_shaders.inc"   // generated at build time: DXBC of every entry point
#define RRHD_PRECOMPILED_SHADERS 1
#endif
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rrhd {

struct GpuQuad {            // 80 bytes, mirrored by the HLSL structured buffer
    float x[4], y[4], u[4], v[4];
    uint32_t atlasXY;       // texture rect origin in its atlas: x | y << 16
    uint32_t sizeWH;        // texture rect size: w | h << 16
    uint32_t flags;         // bit0 transparent(index 0), bit1 bilinear, bit2 flat, bit3 per-frame atlas, bit4 anisotropic averaging, bits 8-15 opacity (255 = solid)
    uint32_t color;         // flat colour palette index
};
static_assert(sizeof(GpuQuad) == 80, "GpuQuad must match the HLSL layout");

struct GpuFrameInfo {       // constant buffer (b0), 16-byte multiples
    float viewW, viewH, sceneScale, halfW;
    int32_t horizonW, horizonRows, horizonScroll, horizonY;
    float uiScale, uiOx, uiOy, uiFlat;
    float uiX0, uiX1, uiY0, uiY1;      // fitted 640x480 area in output pixels
    float backdropFilter, presentW, presentH, padC; // backdropFilter: 1 = bilinear sky/backdrop; presentW/H: size of the area the scene is stretched over while presenting
    float presentX, presentY, padD, padE;           // top-left of that area in back-buffer pixels
};

class GpuRenderer {
public:
    GpuFrameInfo lastInfo{};
    // Where the picture goes in the window (client pixels). 0 size = the whole window. A smaller box leaves black bars.
    int boxX = 0, boxY = 0, boxW = 0, boxH = 0;
    void (*log)(const char*) = nullptr;
    // true: the swap chain lives on a child window of the game window (Windows: keeps the DWM from holding on to the
    // last flip-model frame). false: the swap chain is created on the game window itself (the usual path under Wine/Proton).
    bool useChildWindow = true;
    bool failed = false;
    // Low-latency presentation (set before init): a waitable swap chain with at most one queued frame, and
    // tearing presents when VSync is off (no compositor copy wait). Both fall back silently when unsupported.
    bool lowLatency = true;
    bool allowTearing = true;
    bool tearingActive() const { return tearingOk; }
    bool waitableActive() const { return waitable != nullptr; }

    bool ready() const { return device && swap && !failed; }
    HWND windowHandle() const { return window; }
    int sceneWidth() const { return sceneW; }
    int sceneHeight() const { return sceneH; }

    ~GpuRenderer() { shutdown(); }

    // Creates the device and swap chain for the game window. Safe to call repeatedly.
    // Another component (the video player) may need the window: drop only the swap chain.
    void releaseWindow() {
        if (ctx) ctx->ClearState();
        rel(backRTV); rel(swap);
        closeWaitable();
        if (ctx) ctx->Flush();              // make sure DXGI really destroyed the swap chain first
        if (child) { DestroyWindow(child); child = nullptr; }
        if (window && IsWindow(window)) InvalidateRect(window, nullptr, FALSE);   // let the game repaint what it drew with GDI
        window = nullptr;
    }

    bool init(HWND hwnd, int sceneWidthIn, int sceneHeightIn) {
        if (failed) return false;
        if (device && swap) { if (hwnd == window) return resizeScene(sceneWidthIn, sceneHeightIn); releaseWindow(); }
        if (device) {                       // device kept, swap chain was released
            window = hwnd;
            if (!createSwapChain(hwnd)) return false;
            return resizeScene(sceneWidthIn, sceneHeightIn);
        }
        window = hwnd;
        HMODULE d3d = LoadLibraryW(L"d3d11.dll"), dxgi = LoadLibraryW(L"dxgi.dll");
        if (!d3d || !dxgi) return fail("d3d11/dxgi not available");
        auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d, "D3D11CreateDevice"));
        typedef HRESULT(WINAPI* CreateFactory1)(REFIID, void**);
        auto factoryFn = reinterpret_cast<CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        if (!create || !factoryFn) return fail("D3D11 entry points missing");
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 }, got{};
        HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device, &got, &ctx);
        if (FAILED(hr)) return fail("D3D11CreateDevice failed (hardware, feature level 11.0)", hr);
        { // adapter name for the log
            IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC desc{}; ad->GetDesc(&desc);
                char name[160]; WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
                char msg[256]; std::snprintf(msg, sizeof msg, "Direct3D 11 renderer on %s", name); say(msg);
                std::snprintf(msg, sizeof msg, "adapter: vendor %04x device %04x, %u MB dedicated video memory, feature level 0x%x, shaders %s, swap chain on %s",
                    desc.VendorId, desc.DeviceId, unsigned(desc.DedicatedVideoMemory >> 20), unsigned(got),
#ifdef RRHD_PRECOMPILED_SHADERS
                    "pre-compiled",
#else
                    "compiled at run time",
#endif
                    useChildWindow ? "a child window" : "the game window"); say(msg);
                // Which implementations answer (DXVK, wined3d, the vendor driver): the module paths tell.
                struct { const wchar_t* wide; const char* narrow; } modules[] = { { L"d3d11.dll", "d3d11.dll" }, { L"dxgi.dll", "dxgi.dll" } };
                for (const auto& m : modules) {
                    wchar_t path[MAX_PATH]{}; HMODULE h = GetModuleHandleW(m.wide);
                    if (h && GetModuleFileNameW(h, path, MAX_PATH)) {
                        char utf8[600]; WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof utf8, nullptr, nullptr);
                        std::snprintf(msg, sizeof msg, "module %s = %.300s", m.narrow, utf8); say(msg);
                    }
                }
            }
            if (ad) ad->Release(); if (dd) dd->Release();
        }
        factoryProc = reinterpret_cast<void*>(factoryFn);
        if (!createSwapChain(hwnd)) return false;
        if (!createPipeline()) return false;
        if (!createAtlases()) return false;
        return resizeScene(sceneWidthIn, sceneHeightIn);
    }

    bool createSwapChain(HWND hwnd) {
        typedef HRESULT(WINAPI* CreateFactory1)(REFIID, void**);
        auto factoryFn = reinterpret_cast<CreateFactory1>(factoryProc);
        IDXGIFactory1* factory = nullptr;
        if (FAILED(factoryFn(__uuidof(IDXGIFactory1), (void**)&factory))) return fail("CreateDXGIFactory1 failed");
        IDXGIFactory2* factory2 = nullptr;
        if (FAILED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory2))) { factory->Release(); return fail("DXGI 1.2 factory unavailable"); }
        RECT rc{}; GetClientRect(hwnd, &rc);
        backW = std::max<LONG>(1, rc.right); backH = std::max<LONG>(1, rc.bottom);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = backW; sd.Height = backH; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        // The swap chain lives on its own child window that covers the game window and is destroyed when the
        // race ends: the game window itself never gets a flip-model chain, so whatever GDI drew there reappears.
        HWND target = hwnd;
        if (useChildWindow) {
            static ATOM cls = 0;
            if (!cls) {
                WNDCLASSW wc{}; wc.lpfnWndProc = childProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"RRHD_GpuSurface";
                wc.hCursor = nullptr; cls = RegisterClassW(&wc);
            }
            child = CreateWindowExW(0, L"RRHD_GpuSurface", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, backW, backH, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!child) { factory2->Release(); factory->Release(); return fail("could not create the Direct3D surface window"); }
            SetWindowPos(child, HWND_TOP, 0, 0, backW, backH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            target = child;
        }
        tearingOk = false; swapFlags = 0;
        if (allowTearing) {
            IDXGIFactory5* factory5 = nullptr; BOOL tearing = FALSE;
            if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&factory5))) {
                if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)) && tearing) tearingOk = true;
                factory5->Release();
            }
        }
        if (tearingOk) swapFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        if (lowLatency) swapFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        sd.Flags = swapFlags;
        HRESULT hr = factory2->CreateSwapChainForHwnd(device, target, &sd, nullptr, nullptr, &swap);
        if (FAILED(hr) && swapFlags) {      // an older driver may refuse the flags: plain flip-model chain
            tearingOk = false; swapFlags = 0; sd.Flags = 0;
            hr = factory2->CreateSwapChainForHwnd(device, target, &sd, nullptr, nullptr, &swap);
        }
        if (SUCCEEDED(hr)) factory->MakeWindowAssociation(target, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        factory2->Release(); factory->Release();
        if (FAILED(hr)) return fail("CreateSwapChainForHwnd failed", hr);
        if (swapFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
            IDXGISwapChain2* swap2 = nullptr;
            if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&swap2))) {
                swap2->SetMaximumFrameLatency(1);
                waitable = swap2->GetFrameLatencyWaitableObject();
                swap2->Release();
            }
        }
        if (log) { char m[160]; std::snprintf(m, sizeof m, "present: %s, tearing %s", waitable ? "waitable swap chain (1 frame latency)" : "standard swap chain", tearingOk ? "allowed" : "off"); log(m); }
        return createBackBufferView();
    }
    void closeWaitable() { if (waitable) { CloseHandle(waitable); waitable = nullptr; } }
    void shutdown() {
        if (ctx) ctx->ClearState();
        rel(backRTV); rel(swap);
        closeWaitable();
        if (child) { DestroyWindow(child); child = nullptr; }
        rel(sceneRT); rel(sceneRTV); rel(sceneSRV); rel(cpuTex); rel(cpuSRV); rel(backRTV);
        rel(swap); rel(ctx); rel(device);
        rel(paletteTex); rel(paletteSRV); rel(legacyTex); rel(legacySRV); rel(horizonTex); rel(horizonSRV);
        rel(staticAtlas); rel(staticSRV); rel(frameAtlas); rel(frameSRV); rel(uiTex); rel(uiSRV);
        rel(quadBuf); rel(quadSRV); rel(frameCB);
        rel(vsFull); rel(psBackdrop); rel(vsQuad); rel(psQuad); rel(psCopyCpu); rel(psUi); rel(psPresent);
        rel(blendOpaque); rel(blendUi); rel(blendQuad); rel(rast); rel(pointSampler); rel(linearSampler); rel(readStage);
        for (int i = 0; i < TS_SETS; ++i) { rel(tsDisjoint[i]); rel(tsStart[i]); rel(tsEnd[i]); tsInFlight[i] = false; }
        tsReady = false; tsOpen = false; tsWrite = 0;
        window = nullptr;
    }

    // ---------------------------------------------------------------- per-frame uploads
    void setPalette(const uint32_t* colors256) {      // 0x00RRGGBB entries
        uint32_t tmp[256];
        for (int i = 0; i < 256; ++i) tmp[i] = 0xff000000u | colors256[i];
        ctx->UpdateSubresource(paletteTex, 0, nullptr, tmp, 256 * 4, 0);
    }
    void setLegacyBackdrop(const uint8_t* pixels640x480) {
        ctx->UpdateSubresource(legacyTex, 0, nullptr, pixels640x480, 640, 0);
    }
    void setHorizon(const uint8_t* pixels, int w, int h) {
        rel(horizonTex); rel(horizonSRV);
        horizonW = w; horizonRows = h;
        if (!pixels || w <= 0 || h <= 0) { horizonW = horizonRows = 0; return; }
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UINT; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sr{ pixels, UINT(w), 0 };
        if (FAILED(device->CreateTexture2D(&td, &sr, &horizonTex)) || FAILED(device->CreateShaderResourceView(horizonTex, nullptr, &horizonSRV))) {
            rel(horizonTex); rel(horizonSRV); horizonW = horizonRows = 0;
        }
    }
    // The static atlas caches level textures by pointer; call when a level (re)loads.
    void flushTextures() { staticMap.clear(); staticShelves.clear(); staticTop = 0; }
    void beginFrame() {
        frameShelves.clear(); frameTop = 0;
        // Wait until the swap chain can take a frame: at most one frame is ever queued, so input is never
        // older than one frame. The short timeout keeps the game running if a frame was abandoned.
        if (waitable) WaitForSingleObjectEx(waitable, 40, TRUE);
        timingBegin();
    }

    // GPU time per frame (Profile=1): timestamp queries around the scene, the UI and the scaling pass. Results arrive a few
    // frames late; takeGpuTimes() hands out what has been collected since the last call (milliseconds).
    bool timing = false;
    std::vector<float> takeGpuTimes() { std::vector<float> out; out.swap(gpuTimes); return out; }
    void timingBegin() {
        if (!timing || !device) return;
        if (!tsReady) {
            D3D11_QUERY_DESC qd{}; bool ok = true;
            for (int i = 0; i < TS_SETS && ok; ++i) {
                qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; ok = SUCCEEDED(device->CreateQuery(&qd, &tsDisjoint[i]));
                qd.Query = D3D11_QUERY_TIMESTAMP; ok = ok && SUCCEEDED(device->CreateQuery(&qd, &tsStart[i])) && SUCCEEDED(device->CreateQuery(&qd, &tsEnd[i]));
            }
            if (!ok) { timing = false; return; }
            tsReady = true;
        }
        collectTimes();
        ctx->Begin(tsDisjoint[tsWrite]); ctx->End(tsStart[tsWrite]); tsOpen = true;
    }
    void timingEnd() {
        if (!timing || !tsReady || !tsOpen) return;
        ctx->End(tsEnd[tsWrite]); ctx->End(tsDisjoint[tsWrite]); tsInFlight[tsWrite] = true; tsOpen = false;
        tsWrite = (tsWrite + 1) % TS_SETS;
    }
    void collectTimes() {
        for (int n = 0; n < TS_SETS; ++n) {
            int i = (tsWrite + n) % TS_SETS;       // oldest first
            if (!tsInFlight[i]) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 a = 0, b = 0;
            if (ctx->GetData(tsDisjoint[i], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
            if (ctx->GetData(tsStart[i], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
            if (ctx->GetData(tsEnd[i], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
            tsInFlight[i] = false;
            if (!dj.Disjoint && dj.Frequency && b >= a && gpuTimes.size() < 4096) gpuTimes.push_back(float(double(b - a) * 1000.0 / double(dj.Frequency)));
        }
    }

    // Uploads a W x H indexed texture (or finds it in the cache) and returns its atlas origin.
    bool texture(const uint8_t* pixels, int w, int h, bool transient, uint32_t& xy, uint32_t& flagsOut) {
        if (!pixels || w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
        if (transient) {
            int x, y;
            if (!allocate(frameShelves, frameTop, FRAME_ATLAS, FRAME_ATLAS, w, h, x, y)) return false;
            D3D11_BOX box{ UINT(x), UINT(y), 0, UINT(x + w), UINT(y + h), 1 };
            ctx->UpdateSubresource(frameAtlas, 0, &box, pixels, UINT(w), 0);
            xy = uint32_t(x) | (uint32_t(y) << 16); flagsOut = 8; return true;
        }
        uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(pixels)) << 24) ^ (uint64_t(w) << 12) ^ uint64_t(h);
        uint32_t sum = checksum(pixels, w, h);
        auto it = staticMap.find(key);
        if (it != staticMap.end()) {
            if (it->second.sum == sum) { xy = it->second.xy; flagsOut = 0; return true; }
            int x = it->second.xy & 0xffff, y = it->second.xy >> 16; // same size: refresh in place
            D3D11_BOX box{ UINT(x), UINT(y), 0, UINT(x + w), UINT(y + h), 1 };
            ctx->UpdateSubresource(staticAtlas, 0, &box, pixels, UINT(w), 0);
            it->second.sum = sum; xy = it->second.xy; flagsOut = 0; return true;
        }
        int x, y;
        if (!allocate(staticShelves, staticTop, STATIC_ATLAS, STATIC_ATLAS, w, h, x, y)) return false;
        D3D11_BOX box{ UINT(x), UINT(y), 0, UINT(x + w), UINT(y + h), 1 };
        ctx->UpdateSubresource(staticAtlas, 0, &box, pixels, UINT(w), 0);
        staticMap[key] = { uint32_t(x) | (uint32_t(y) << 16), sum };
        xy = uint32_t(x) | (uint32_t(y) << 16); flagsOut = 0; return true;
    }
    bool staticAtlasFull() const { return staticFull; }
    void clearStaticFull() { staticFull = false; }

    // ---------------------------------------------------------------- drawing
    // (Re)create the scene render target when the internal render size changes.
    bool resizeScene(int w, int h) {
        w = std::max(1, w); h = std::max(1, h);
        if (sceneRT && w == sceneW && h == sceneH) return true;
        rel(sceneRT); rel(sceneRTV); rel(sceneSRV); rel(cpuTex); rel(cpuSRV); rel(readStage);
        sceneW = w; sceneH = h;
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &sceneRT)) || FAILED(device->CreateRenderTargetView(sceneRT, nullptr, &sceneRTV)) ||
            FAILED(device->CreateShaderResourceView(sceneRT, nullptr, &sceneSRV))) return fail("scene render target failed");
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &cpuTex)) || FAILED(device->CreateShaderResourceView(cpuTex, nullptr, &cpuSRV)))
            return fail("CPU frame texture failed");
        return true;
    }

    // Draw a picture composed on the CPU (menus, fallback) into the scene target.
    void drawCpuFrame(const uint32_t* rgba) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(cpuTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        for (int y = 0; y < sceneH; ++y) std::memcpy(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, rgba + size_t(y) * sceneW, size_t(sceneW) * 4);
        ctx->Unmap(cpuTex, 0);
        bindScene();
        ctx->PSSetShader(psCopyCpu, nullptr, 0);
        ctx->PSSetShaderResources(7, 1, &cpuSRV);
        ctx->OMSetBlendState(blendOpaque, nullptr, 0xffffffff);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr; ctx->PSSetShaderResources(7, 1, &none);
    }

    // Draw the 3D scene: backdrop then quads in order.
    void drawScene(const GpuFrameInfo& info, const GpuQuad* quads, size_t count) {
        updateFrameInfo(info);
        bindScene();
        ctx->OMSetBlendState(blendOpaque, nullptr, 0xffffffff);
        ID3D11ShaderResourceView* srvs[5] = { legacySRV, horizonSRV ? horizonSRV : legacySRV, paletteSRV, staticSRV, frameSRV };
        ctx->PSSetShaderResources(0, 5, srvs);
        ctx->PSSetShader(psBackdrop, nullptr, 0);
        ctx->Draw(3, 0);
        if (count) {
            if (count > quadCapacity && !growQuads(count)) return;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(quadBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
            std::memcpy(m.pData, quads, count * sizeof(GpuQuad));
            ctx->Unmap(quadBuf, 0);
            ctx->VSSetShaderResources(5, 1, &quadSRV);
            ctx->PSSetShaderResources(5, 1, &quadSRV);
            ctx->VSSetShader(vsQuad, nullptr, 0);
            ctx->PSSetShader(psQuad, nullptr, 0);
            ctx->OMSetBlendState(blendQuad, nullptr, 0xffffffff);   // distant objects fade in with their alpha
            // 4096 instances per call keeps each draw modest; order is preserved across calls.
            for (size_t first = 0; first < count; first += 4096) {
                UINT n = UINT(std::min<size_t>(4096, count - first));
                ctx->DrawInstanced(6, n, 0, UINT(first));
            }
            ID3D11ShaderResourceView* none = nullptr;
            ctx->VSSetShaderResources(5, 1, &none); ctx->PSSetShaderResources(5, 1, &none);
            ctx->VSSetShader(vsFull, nullptr, 0);
        }
        ID3D11ShaderResourceView* nones[5] = {}; ctx->PSSetShaderResources(0, 5, nones);
    }

    // Smoothed UI layer: rgb = colour, a = coverage mask (0 / 255) at 640x480.
    void drawUi(const GpuFrameInfo& info, const uint32_t* layer640x480, bool flat) {
        GpuFrameInfo copy = info; copy.uiFlat = flat ? 1.0f : 0.0f;
        updateFrameInfo(copy);
        ctx->UpdateSubresource(uiTex, 0, nullptr, layer640x480, 640 * 4, 0);
        bindScene();
        ctx->OMSetBlendState(blendUi, nullptr, 0xffffffff);
        ctx->PSSetShader(psUi, nullptr, 0);
        ctx->PSSetShaderResources(6, 1, &uiSRV);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr; ctx->PSSetShaderResources(6, 1, &none);
    }

    // Scale the scene to the window and present.
    bool present(int syncInterval) {
        RECT rc{}; GetClientRect(window, &rc);
        int cw = std::max<LONG>(1, rc.right), ch = std::max<LONG>(1, rc.bottom);
        if (cw != backW || ch != backH) {
            if (backRTV) { ctx->OMSetRenderTargets(0, nullptr, nullptr); rel(backRTV); }
            if (child) SetWindowPos(child, HWND_TOP, 0, 0, cw, ch, SWP_NOACTIVATE);
            HRESULT hr = swap->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, swapFlags);
            if (FAILED(hr)) return fail("ResizeBuffers failed", hr);
            backW = cw; backH = ch;
            if (!createBackBufferView()) return false;
        }
        if ((backW != sceneW || backH != sceneH) && log) {
            static int lastBW = 0, lastBH = 0, lastSW = 0, lastSH = 0;
            if (lastBW != backW || lastBH != backH || lastSW != sceneW || lastSH != sceneH) {
                lastBW = backW; lastBH = backH; lastSW = sceneW; lastSH = sceneH;
                char m[160]; std::snprintf(m, sizeof m, "present: back buffer %dx%d, scene %dx%d (scene is stretched over the window)", backW, backH, sceneW, sceneH);
                log(m);
            }
        }
        bool boxed = boxW > 0 && boxH > 0 && boxX >= 0 && boxY >= 0 && boxX + boxW <= backW && boxY + boxH <= backH && (boxW != backW || boxH != backH);
        { GpuFrameInfo copy = lastInfo; copy.presentW = float(boxed ? boxW : backW); copy.presentH = float(boxed ? boxH : backH);
          copy.presentX = boxed ? float(boxX) : 0.0f; copy.presentY = boxed ? float(boxY) : 0.0f; updateFrameInfo(copy); }
        D3D11_VIEWPORT vp = boxed ? D3D11_VIEWPORT{ float(boxX), float(boxY), float(boxW), float(boxH), 0, 1 } : D3D11_VIEWPORT{ 0, 0, float(backW), float(backH), 0, 1 };
        ctx->OMSetRenderTargets(1, &backRTV, nullptr);
        if (boxed) { const float black[4] = { 0, 0, 0, 1 }; ctx->ClearRenderTargetView(backRTV, black); }   // the bars
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetShader(vsFull, nullptr, 0);
        ctx->PSSetShader(psPresent, nullptr, 0);
        ctx->PSSetShaderResources(7, 1, &sceneSRV);
        ctx->PSSetSamplers(0, 1, &pointSampler);   // never filter the picture: menus and text stay crisp
        ctx->OMSetBlendState(blendOpaque, nullptr, 0xffffffff);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr; ctx->PSSetShaderResources(7, 1, &none);
        timingEnd();
        HRESULT hr = swap->Present(syncInterval, (syncInterval == 0 && tearingOk) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return fail("device lost", hr);
        return true;
    }

    // Copy the scene target (what was drawn before scaling) back to the CPU, 0x00RRGGBB.
    bool readScene(std::vector<uint32_t>& out) {
        if (!sceneRT) return false;
        if (!readStage) {
            D3D11_TEXTURE2D_DESC td{}; td.Width = sceneW; td.Height = sceneH; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &readStage))) return false;
        }
        ctx->CopyResource(readStage, sceneRT);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(readStage, 0, D3D11_MAP_READ, 0, &m))) return false;
        out.resize(size_t(sceneW) * sceneH);
        for (int y = 0; y < sceneH; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
            for (int x = 0; x < sceneW; ++x) out[size_t(y) * sceneW + x] = row[x] & 0x00ffffffu;
        }
        ctx->Unmap(readStage, 0);
        return true;
    }

    int horizonWidth() const { return horizonW; }
    int horizonHeight() const { return horizonRows; }

private:
    static constexpr int STATIC_ATLAS = 4096, FRAME_ATLAS = 2048;
    struct Shelf { int y, h, x; };
    struct Entry { uint32_t xy, sum; };

    HWND window = nullptr, child = nullptr;
    static LRESULT CALLBACK childProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCHITTEST) return HTTRANSPARENT;   // mouse input belongs to the game window underneath
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        return DefWindowProcW(h, m, w, l);
    }
    ID3D11Device* device = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* backRTV = nullptr;
    HANDLE waitable = nullptr;
    bool tearingOk = false;
    UINT swapFlags = 0;
    ID3D11Texture2D *sceneRT = nullptr, *cpuTex = nullptr, *paletteTex = nullptr, *legacyTex = nullptr, *horizonTex = nullptr,
        *staticAtlas = nullptr, *frameAtlas = nullptr, *uiTex = nullptr, *readStage = nullptr;
    ID3D11RenderTargetView* sceneRTV = nullptr;
    ID3D11ShaderResourceView *sceneSRV = nullptr, *cpuSRV = nullptr, *paletteSRV = nullptr, *legacySRV = nullptr, *horizonSRV = nullptr,
        *staticSRV = nullptr, *frameSRV = nullptr, *uiSRV = nullptr, *quadSRV = nullptr;
    ID3D11Buffer *quadBuf = nullptr, *frameCB = nullptr;
    ID3D11VertexShader *vsFull = nullptr, *vsQuad = nullptr;
    ID3D11PixelShader *psBackdrop = nullptr, *psQuad = nullptr, *psCopyCpu = nullptr, *psUi = nullptr, *psPresent = nullptr;
    ID3D11BlendState *blendOpaque = nullptr, *blendUi = nullptr, *blendQuad = nullptr;
    static const int TS_SETS = 6;
    ID3D11Query *tsDisjoint[TS_SETS] = {}, *tsStart[TS_SETS] = {}, *tsEnd[TS_SETS] = {};
    bool tsReady = false, tsOpen = false, tsInFlight[TS_SETS] = {};
    int tsWrite = 0;
    std::vector<float> gpuTimes;
    ID3D11RasterizerState* rast = nullptr;
    ID3D11SamplerState *pointSampler = nullptr, *linearSampler = nullptr;
    pD3DCompile compileFn = nullptr;
    void* factoryProc = nullptr;
    int sceneW = 0, sceneH = 0, backW = 0, backH = 0, horizonW = 0, horizonRows = 0;
    size_t quadCapacity = 0;
    std::unordered_map<uint64_t, Entry> staticMap;
    std::vector<Shelf> staticShelves, frameShelves;
    int staticTop = 0, frameTop = 0;
    bool staticFull = false;

    template<class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    void say(const char* m) { if (log) log(m); }
    bool fail(const char* m, HRESULT hr = S_OK) {
        char msg[256]; std::snprintf(msg, sizeof msg, "Direct3D renderer disabled: %s (hr=0x%08lx)", m, static_cast<unsigned long>(hr));
        say(msg); failed = true; return false;
    }
    static uint32_t checksum(const uint8_t* p, int w, int h) {
        uint32_t s = 2166136261u; size_t n = size_t(w) * h, step = std::max<size_t>(1, n / 96);
        for (size_t i = 0; i < n; i += step) s = (s ^ p[i]) * 16777619u;
        return s ^ uint32_t(n);
    }
    bool allocate(std::vector<Shelf>& shelves, int& top, int aw, int ah, int w, int h, int& x, int& y) {
        for (auto& s : shelves)
            if (h <= s.h && s.x + w <= aw && h * 4 >= s.h) { x = s.x; y = s.y; s.x += w; return true; }
        if (top + h > ah) { if (&shelves == &staticShelves) staticFull = true; return false; }
        shelves.push_back({ top, h, w }); x = 0; y = top; top += h; return true;
    }
    bool createBackBufferView() {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return fail("GetBuffer failed");
        HRESULT hr = device->CreateRenderTargetView(back, nullptr, &backRTV);
        back->Release();
        return SUCCEEDED(hr) || fail("back buffer view failed", hr);
    }
    ID3DBlob* compile(const char* src, const char* entry, const char* target) {
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = compileFn(src, std::strlen(src), "hd_gpu.hlsl", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (FAILED(hr)) {
            char msg[600]; std::snprintf(msg, sizeof msg, "shader %s failed: %.400s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            say(msg); if (err) err->Release(); return nullptr;
        }
        if (err) err->Release();
        return code;
    }
    bool createPipeline() {
        struct { const char* e; const char* t; void** out; bool vs; } list[] = {
            { "VSFull", "vs_5_0", (void**)&vsFull, true }, { "VSQuad", "vs_5_0", (void**)&vsQuad, true },
            { "PSBackdrop", "ps_5_0", (void**)&psBackdrop, false }, { "PSQuad", "ps_5_0", (void**)&psQuad, false },
            { "PSCopyCpu", "ps_5_0", (void**)&psCopyCpu, false }, { "PSUi", "ps_5_0", (void**)&psUi, false },
            { "PSPresent", "ps_5_0", (void**)&psPresent, false } };
        for (auto& s : list) {
            // Pre-compiled DXBC (embedded at build time) when available: no shader compiler is needed at run time.
            const void* data = nullptr; size_t size = 0; ID3DBlob* code = nullptr;
#ifdef RRHD_PRECOMPILED_SHADERS
            for (const auto& b : rrhdDxbcTable) if (!std::strcmp(b.name, s.e)) { data = b.data; size = b.size; }
#endif
            if (!data) {
                if (!compileFn) {
                    HMODULE c = LoadLibraryW(L"d3dcompiler_47.dll");
                    compileFn = c ? reinterpret_cast<pD3DCompile>(GetProcAddress(c, "D3DCompile")) : nullptr;
                }
                if (!compileFn) return fail("no pre-compiled shaders and d3dcompiler_47 is not available");
                code = compile(hlsl(), s.e, s.t);
                if (!code) return fail("shader compilation failed");
                data = code->GetBufferPointer(); size = code->GetBufferSize();
            }
            HRESULT hr = s.vs ? device->CreateVertexShader(data, size, nullptr, (ID3D11VertexShader**)s.out)
                              : device->CreatePixelShader(data, size, nullptr, (ID3D11PixelShader**)s.out);
            if (code) code->Release();
            if (FAILED(hr)) return fail("shader creation failed", hr);
        }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(GpuFrameInfo); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &frameCB))) return fail("constant buffer failed");
        D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&bl, &blendOpaque))) return fail("blend state failed");
        bl.RenderTarget[0].BlendEnable = TRUE; bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (FAILED(device->CreateBlendState(&bl, &blendUi))) return fail("UI blend state failed");
        if (FAILED(device->CreateBlendState(&bl, &blendQuad))) return fail("quad blend state failed");
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rd, &rast))) return fail("rasterizer state failed");
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(device->CreateSamplerState(&sd, &pointSampler))) return fail("sampler failed");
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        if (FAILED(device->CreateSamplerState(&sd, &linearSampler))) return fail("sampler failed");
        return true;
    }
    bool tex2d(ID3D11Texture2D*& t, ID3D11ShaderResourceView*& v, int w, int h, DXGI_FORMAT f) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &t)) && SUCCEEDED(device->CreateShaderResourceView(t, nullptr, &v));
    }
    bool createAtlases() {
        if (!tex2d(staticAtlas, staticSRV, STATIC_ATLAS, STATIC_ATLAS, DXGI_FORMAT_R8_UINT) ||
            !tex2d(frameAtlas, frameSRV, FRAME_ATLAS, FRAME_ATLAS, DXGI_FORMAT_R8_UINT) ||
            !tex2d(paletteTex, paletteSRV, 256, 1, DXGI_FORMAT_B8G8R8A8_UNORM) ||
            !tex2d(legacyTex, legacySRV, 640, 480, DXGI_FORMAT_R8_UINT) ||
            !tex2d(uiTex, uiSRV, 640, 480, DXGI_FORMAT_B8G8R8A8_UNORM)) return fail("texture creation failed");
        return growQuads(2048);
    }
    bool growQuads(size_t count) {
        rel(quadBuf); rel(quadSRV);
        quadCapacity = std::max<size_t>(count, quadCapacity * 2);
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = UINT(quadCapacity * sizeof(GpuQuad)); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = sizeof(GpuQuad);
        if (FAILED(device->CreateBuffer(&bd, nullptr, &quadBuf))) return fail("quad buffer failed");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.FirstElement = 0; sv.Buffer.NumElements = UINT(quadCapacity);
        if (FAILED(device->CreateShaderResourceView(quadBuf, &sv, &quadSRV))) return fail("quad view failed");
        return true;
    }
    void updateFrameInfo(const GpuFrameInfo& info) {
        lastInfo = info;
        ctx->UpdateSubresource(frameCB, 0, nullptr, &info, 0, 0);
        ctx->VSSetConstantBuffers(0, 1, &frameCB); ctx->PSSetConstantBuffers(0, 1, &frameCB);
    }
    void bindScene() {
        ctx->OMSetRenderTargets(1, &sceneRTV, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, float(sceneW), float(sceneH), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rast);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vsFull, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &pointSampler);
    }

    static const char* hlsl() { return gpuHlsl(); }
};

}  // namespace rrhd
