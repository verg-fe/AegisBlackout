#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <magnification.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "runtimeobject.lib")

#ifndef WDA_MONITOR
#define WDA_MONITOR 0x01
#endif
#ifndef D3D11_RESOURCE_MISC_HW_PROTECTED
#define D3D11_RESOURCE_MISC_HW_PROTECTED 0x80000
#endif
#ifndef DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY
#define DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY 0x20
#endif
#ifndef DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED
#define DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED 0x400
#endif

static FILE* g_outFile = NULL;
static void LogOut(const char* fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    if (g_outFile) {
        fprintf(g_outFile, "%s\n", buf);
        fflush(g_outFile);
    }
}

static int g_vx = 0, g_vy = 0, g_vw = 3840, g_vh = 2160;
static HWND g_targetHwnd = NULL;
static std::string g_mode = "host";
static std::wstring g_targetProc = L"AegisBlackout.exe";

struct PixelStats {
    size_t nonBlack = 0;
    size_t total = 0;
    double pct() const { return total ? (100.0 * (double)nonBlack / (double)total) : 0.0; }
};

static PixelStats CountNonBlackBgraBuffer(const BYTE* data, int w, int h, int stride)
{
    PixelStats st;
    if (!data || w <= 0 || h <= 0 || stride < w * 4) return st;
    int stepY = (h > 400) ? 2 : 1;
    int stepX = (w > 400) ? 2 : 1;
    for (int y = 0; y < h; y += stepY) {
        const BYTE* row = data + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x += stepX) {
            const BYTE* px = row + (size_t)x * 4;
            st.total++;
            if (px[0] || px[1] || px[2]) st.nonBlack++;
        }
    }
    return st;
}

static PixelStats InspectHBitmap(HDC hdcRef, HBITMAP hbm, int w, int h)
{
    PixelStats st;
    if (!hbm || w <= 0 || h <= 0) return st;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> buf((size_t)w * (size_t)h * 4, 0);
    if (GetDIBits(hdcRef, hbm, 0, h, buf.data(), &bmi, DIB_RGB_COLORS) > 0) {
        st = CountNonBlackBgraBuffer(buf.data(), w, h, w * 4);
    }
    return st;
}

static PixelStats CaptureBitBltRegion(HDC hdcSrc, int sx, int sy, int w, int h, DWORD rop, bool clearFirst = true)
{
    PixelStats st;
    if (!hdcSrc || w <= 0 || h <= 0) return st;
    HDC mem = CreateCompatibleDC(hdcSrc);
    HBITMAP bmp = CreateCompatibleBitmap(hdcSrc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    if (clearFirst) PatBlt(mem, 0, 0, w, h, BLACKNESS);
    BitBlt(mem, 0, 0, w, h, hdcSrc, sx, sy, rop);
    st = InspectHBitmap(mem, bmp, w, h);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    return st;
}

/* ---------------- Group A: 11 GDI & Window-Message Variants ---------------- */
static void RunGroupA()
{
    LogOut("\n=== GROUP A: 11 GDI & Window-Message Capture Variants ===");
    int sx = g_vx + g_vw / 4;
    int sy = g_vy + g_vh / 4;
    int rw = 1280, rh = 720;

    HDC hdcScreen = GetDC(NULL);
    PixelStats s1 = CaptureBitBltRegion(hdcScreen, sx, sy, rw, rh, SRCCOPY);
    LogOut("[V01] GDI BitBlt(GetDC(NULL), SRCCOPY): non-black=%zu/%zu (%.2f%%)", s1.nonBlack, s1.total, s1.pct());

    PixelStats s2 = CaptureBitBltRegion(hdcScreen, sx, sy, rw, rh, SRCCOPY | CAPTUREBLT);
    LogOut("[V02] GDI BitBlt(GetDC(NULL), SRCCOPY|CAPTUREBLT): non-black=%zu/%zu (%.2f%%)", s2.nonBlack, s2.total, s2.pct());

    /* V03 StretchBlt */
    HDC mem3 = CreateCompatibleDC(hdcScreen);
    HBITMAP bmp3 = CreateCompatibleBitmap(hdcScreen, 640, 360);
    HBITMAP old3 = (HBITMAP)SelectObject(mem3, bmp3);
    PatBlt(mem3, 0, 0, 640, 360, BLACKNESS);
    StretchBlt(mem3, 0, 0, 640, 360, hdcScreen, sx, sy, rw, rh, SRCCOPY | CAPTUREBLT);
    PixelStats s3 = InspectHBitmap(mem3, bmp3, 640, 360);
    SelectObject(mem3, old3); DeleteObject(bmp3); DeleteDC(mem3);
    LogOut("[V03] GDI StretchBlt(GetDC(NULL), SRCCOPY|CAPTUREBLT): non-black=%zu/%zu (%.2f%%)", s3.nonBlack, s3.total, s3.pct());
    ReleaseDC(NULL, hdcScreen);

    /* V04 CreateDCW("DISPLAY") */
    HDC hdcDisp = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
    PixelStats s4 = hdcDisp ? CaptureBitBltRegion(hdcDisp, sx, sy, rw, rh, SRCCOPY) : PixelStats{};
    LogOut("[V04] GDI CreateDCW(DISPLAY) BitBlt(SRCCOPY): hdc=%p non-black=%zu/%zu (%.2f%%)", (void*)hdcDisp, s4.nonBlack, s4.total, s4.pct());
    if (hdcDisp) DeleteDC(hdcDisp);

    /* V05 CreateDCW("\\\\.\\DISPLAY1") */
    HDC hdcDisp1 = CreateDCW(L"\\\\.\\DISPLAY1", NULL, NULL, NULL);
    PixelStats s5 = hdcDisp1 ? CaptureBitBltRegion(hdcDisp1, sx, sy, rw, rh, SRCCOPY | CAPTUREBLT) : PixelStats{};
    LogOut("[V05] GDI CreateDCW(\\\\.\\DISPLAY1) BitBlt(SRCCOPY|CAPTUREBLT): hdc=%p non-black=%zu/%zu (%.2f%%)", (void*)hdcDisp1, s5.nonBlack, s5.total, s5.pct());
    if (hdcDisp1) DeleteDC(hdcDisp1);

    /* V06 GetPixel 20x20 grid */
    hdcScreen = GetDC(NULL);
    size_t gpNonBlack = 0, gpTotal = 0, gpInvalid = 0;
    for (int gy = 0; gy < 20; gy++) {
        for (int gx = 0; gx < 20; gx++) {
            int px = g_vx + (g_vw * (gx + 1)) / 21;
            int py = g_vy + (g_vh * (gy + 1)) / 21;
            COLORREF c = GetPixel(hdcScreen, px, py);
            gpTotal++;
            if (c == CLR_INVALID) gpInvalid++;
            else if (c != 0) gpNonBlack++;
        }
    }
    ReleaseDC(NULL, hdcScreen);
    LogOut("[V06] GDI GetPixel(GetDC(NULL)) 20x20 grid: non-black=%zu/%zu (%.2f%%) invalid=%zu",
        gpNonBlack, gpTotal, gpTotal ? (100.0 * gpNonBlack / gpTotal) : 0.0, gpInvalid);

    /* V07-V11 target HWND */
    HWND th = g_targetHwnd;
    if (!th || !IsWindow(th)) th = GetForegroundWindow();
    WCHAR ttl[128] = L"";
    if (th) GetWindowTextW(th, ttl, 127);
    RECT trc = { 0, 0, 600, 600 };
    if (th) GetWindowRect(th, &trc);
    int tw = std::max(1, std::min<int>(800, trc.right - trc.left));
    int thh = std::max(1, std::min<int>(800, trc.bottom - trc.top));

    HDC hdcWClient = th ? GetDC(th) : NULL;
    PixelStats s7 = hdcWClient ? CaptureBitBltRegion(hdcWClient, 0, 0, tw, thh, SRCCOPY) : PixelStats{};
    if (hdcWClient) ReleaseDC(th, hdcWClient);
    LogOut("[V07] GDI GetDC(target=0x%llX) BitBlt: non-black=%zu/%zu (%.2f%%)",
        (unsigned long long)(ULONG_PTR)th, s7.nonBlack, s7.total, s7.pct());

    HDC hdcWin = th ? GetWindowDC(th) : NULL;
    PixelStats s8 = hdcWin ? CaptureBitBltRegion(hdcWin, 0, 0, tw, thh, SRCCOPY) : PixelStats{};
    if (hdcWin) ReleaseDC(th, hdcWin);
    LogOut("[V08] GDI GetWindowDC(target=0x%llX) BitBlt: non-black=%zu/%zu (%.2f%%)",
        (unsigned long long)(ULONG_PTR)th, s8.nonBlack, s8.total, s8.pct());

    HDC hdcRef = GetDC(NULL);
    auto runPW = [&](UINT flags) -> std::pair<BOOL, PixelStats> {
        HDC mem = CreateCompatibleDC(hdcRef);
        HBITMAP bmp = CreateCompatibleBitmap(hdcRef, tw, thh);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        PatBlt(mem, 0, 0, tw, thh, BLACKNESS);
        BOOL ok = th ? PrintWindow(th, mem, flags) : FALSE;
        PixelStats ps = InspectHBitmap(mem, bmp, tw, thh);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        return { ok, ps };
    };

    auto r9 = runPW(0);
    LogOut("[V09] PrintWindow(target=0x%llX, 0): ok=%d non-black=%zu/%zu (%.2f%%)",
        (unsigned long long)(ULONG_PTR)th, r9.first, r9.second.nonBlack, r9.second.total, r9.second.pct());

    auto r10 = runPW(PW_CLIENTONLY);
    LogOut("[V10] PrintWindow(target=0x%llX, PW_CLIENTONLY=1): ok=%d non-black=%zu/%zu (%.2f%%)",
        (unsigned long long)(ULONG_PTR)th, r10.first, r10.second.nonBlack, r10.second.total, r10.second.pct());

    auto r11pw = runPW(PW_RENDERFULLCONTENT);
    HDC memWm = CreateCompatibleDC(hdcRef);
    HBITMAP bmpWm = CreateCompatibleBitmap(hdcRef, tw, thh);
    HBITMAP oldWm = (HBITMAP)SelectObject(memWm, bmpWm);
    PatBlt(memWm, 0, 0, tw, thh, BLACKNESS);
    LRESULT wmOk = th ? SendMessageTimeoutW(th, WM_PRINT, (WPARAM)memWm,
        PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND, SMTO_ABORTIFHUNG, 500, NULL) : 0;
    PixelStats r11wm = InspectHBitmap(memWm, bmpWm, tw, thh);
    SelectObject(memWm, oldWm); DeleteObject(bmpWm); DeleteDC(memWm);
    ReleaseDC(NULL, hdcRef);

    LogOut("[V11] PrintWindow(PW_RENDERFULLCONTENT=2): ok=%d non-black=%.2f%% | WM_PRINT: ok=%lld non-black=%.2f%% (target=%ls)",
        r11pw.first, r11pw.second.pct(), (long long)wmOk, r11wm.pct(), ttl);
}

/* ---------------- DXGI / Texture Helper ---------------- */
static PixelStats ReadD3D11Texture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex)
{
    PixelStats st;
    if (!dev || !ctx || !tex) return st;
    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = NULL;
    if (FAILED(dev->CreateTexture2D(&desc, NULL, &staging)) || !staging) return st;
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        st = CountNonBlackBgraBuffer((const BYTE*)mapped.pData, (int)desc.Width, (int)desc.Height, (int)mapped.RowPitch);
        ctx->Unmap(staging, 0);
    }
    staging->Release();
    return st;
}

static bool InitD3D11AndDupl(bool useDup1, ID3D11Device** outDev, ID3D11DeviceContext** outCtx, IDXGIOutputDuplication** outDupl, HRESULT* outHr)
{
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, outDev, &fl, outCtx);
    if (FAILED(hr)) { *outHr = hr; return false; }

    IDXGIDevice* dxgiDev = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIOutput* output = NULL;
    hr = (*outDev)->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->EnumOutputs(0, &output);

    if (SUCCEEDED(hr)) {
        if (!useDup1) {
            IDXGIOutput1* out1 = NULL;
            hr = output->QueryInterface(IID_PPV_ARGS(&out1));
            if (SUCCEEDED(hr)) {
                hr = out1->DuplicateOutput(*outDev, outDupl);
                out1->Release();
            }
        } else {
            IDXGIOutput5* out5 = NULL;
            hr = output->QueryInterface(IID_PPV_ARGS(&out5));
            if (SUCCEEDED(hr)) {
                DXGI_FORMAT fmts[] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM };
                hr = out5->DuplicateOutput1(*outDev, 0, 2, fmts, outDupl);
                out5->Release();
            }
        }
    }
    if (output) output->Release();
    if (adapter) adapter->Release();
    if (dxgiDev) dxgiDev->Release();
    *outHr = hr;
    return SUCCEEDED(hr) && (*outDupl != NULL);
}

/* ---------------- Magnification Callback State ---------------- */
static PixelStats g_magStats;
static bool g_magFired = false;
static BOOL CALLBACK MagScaleCallback(HWND hwnd, void* srcdata, MAGIMAGEHEADER srcheader, void* destdata, MAGIMAGEHEADER destheader, RECT unclipped, RECT clipped, HRGN dirty)
{
    (void)hwnd; (void)destdata; (void)destheader; (void)unclipped; (void)clipped; (void)dirty;
    if (srcdata && srcheader.width > 0 && srcheader.height > 0) {
        g_magStats = CountNonBlackBgraBuffer((const BYTE*)srcdata, (int)srcheader.width, (int)srcheader.height, (int)srcheader.stride);
        g_magFired = true;
    }
    return TRUE;
}

/* ---------------- WGC Helpers (C++/WinRT) ---------------- */
struct WgcResult {
    bool blocked = false;
    HRESULT hr = S_OK;
    int framesCaptured = 0;
    double maxNonBlackPct = 0.0;
    size_t maxNonBlack = 0;
    size_t maxTotal = 0;
    std::string note;
};

static WgcResult RunWgcCapture(bool isMonitor, HMONITOR hMon, HWND targetWnd, int maxFrames)
{
    WgcResult res;
    try {
        HMODULE hModWgc = LoadLibraryW(L"GraphicsCapture.dll");
        if (!hModWgc) {
            /* Keep testing via RoGetActivationFactory just in case another DLL backs it */
            res.note = "LoadLibrary(GraphicsCapture.dll)=NULL; ";
        }

        ID3D11Device* d3dDev = NULL;
        ID3D11DeviceContext* d3dCtx = NULL;
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &d3dDev, &fl, &d3dCtx);
        if (FAILED(hr)) { res.hr = hr; res.blocked = true; res.note += "D3D11CreateDevice failed"; return res; }

        IDXGIDevice* dxgiDev = NULL;
        d3dDev->QueryInterface(IID_PPV_ARGS(&dxgiDev));
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDev{ nullptr };
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDev, reinterpret_cast<::IInspectable**>(winrt::put_abi(winrtDev)));
        dxgiDev->Release();
        if (FAILED(hr)) { d3dCtx->Release(); d3dDev->Release(); res.hr = hr; res.blocked = true; res.note += "CreateDirect3D11Device failed"; return res; }

        auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        if (isMonitor) {
            hr = interop->CreateForMonitor(hMon, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item));
        } else {
            hr = interop->CreateForWindow(targetWnd, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item));
        }
        if (FAILED(hr) || !item) {
            d3dCtx->Release(); d3dDev->Release();
            res.hr = hr;
            res.blocked = true;
            res.note += "CreateForMonitor/Window failed";
            return res;
        }

        auto sz = item.Size();
        auto pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDev,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            sz);
        auto session = pool.CreateCaptureSession(item);
        session.StartCapture();

        for (int attempt = 0; attempt < 40 && res.framesCaptured < maxFrames; attempt++) {
            auto frame = pool.TryGetNextFrame();
            if (!frame) {
                Sleep(25);
                continue;
            }
            res.framesCaptured++;
            auto surf = frame.Surface();
            auto access = surf.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ID3D11Texture2D* tex = NULL;
            if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&tex))) && tex) {
                PixelStats st = ReadD3D11Texture(d3dDev, d3dCtx, tex);
                if (st.pct() >= res.maxNonBlackPct) {
                    res.maxNonBlackPct = st.pct();
                    res.maxNonBlack = st.nonBlack;
                    res.maxTotal = st.total;
                }
                tex->Release();
            }
            frame.Close();
        }
        session.Close();
        pool.Close();
        d3dCtx->Release();
        d3dDev->Release();
    } catch (const winrt::hresult_error& e) {
        res.hr = e.code();
        res.blocked = true;
        char msg[256];
        sprintf_s(msg, sizeof(msg), "WinRT exception hr=0x%08X", (unsigned)e.code());
        res.note += msg;
    } catch (...) {
        res.blocked = true;
        res.note += "Unknown exception";
    }
    return res;
}

/* ---------------- Group B: V12-V20 Modern GPU / Continuous Recording ---------------- */
static void RunGroupB()
{
    LogOut("\n=== GROUP B: 9 GPU / Modern / Continuous Recording Variants (V12-V20) ===");

    /* V12 DXGI 1.2 30-frame loop */
    {
        ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; IDXGIOutputDuplication* dupl = NULL;
        HRESULT hr = S_OK;
        if (InitD3D11AndDupl(false, &dev, &ctx, &dupl, &hr)) {
            int frames = 0, maskedOut = 0;
            double maxPct = 0.0; size_t maxNb = 0, maxTot = 0;
            for (int f = 0; f < 30; f++) {
                DXGI_OUTDUPL_FRAME_INFO fi = {};
                IDXGIResource* res = NULL;
                HRESULT hrF = dupl->AcquireNextFrame(100, &fi, &res);
                if (SUCCEEDED(hrF)) {
                    frames++;
                    if (fi.ProtectedContentMaskedOut) maskedOut++;
                    ID3D11Texture2D* tex = NULL;
                    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
                        PixelStats st = ReadD3D11Texture(dev, ctx, tex);
                        if (st.pct() >= maxPct) { maxPct = st.pct(); maxNb = st.nonBlack; maxTot = st.total; }
                        tex->Release();
                    }
                    res->Release();
                    dupl->ReleaseFrame();
                }
                Sleep(16);
            }
            LogOut("[V12] DXGI 1.2 DuplicateOutput (30-frame video): frames=%d maskedOut=%d max-non-black=%zu/%zu (%.2f%%)",
                frames, maskedOut, maxNb, maxTot, maxPct);
            dupl->Release();
        } else {
            LogOut("[V12] DXGI 1.2 DuplicateOutput: BLOCKED/FAILED hr=0x%08X (0.00%% leak)", (unsigned)hr);
        }
        if (ctx) ctx->Release(); if (dev) dev->Release();
    }

    /* V13 DXGI 1.5 DuplicateOutput1 */
    {
        ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; IDXGIOutputDuplication* dupl = NULL;
        HRESULT hr = S_OK;
        if (InitD3D11AndDupl(true, &dev, &ctx, &dupl, &hr)) {
            int frames = 0, maskedOut = 0;
            double maxPct = 0.0; size_t maxNb = 0, maxTot = 0;
            for (int f = 0; f < 5; f++) {
                DXGI_OUTDUPL_FRAME_INFO fi = {};
                IDXGIResource* res = NULL;
                if (SUCCEEDED(dupl->AcquireNextFrame(150, &fi, &res))) {
                    frames++;
                    if (fi.ProtectedContentMaskedOut) maskedOut++;
                    ID3D11Texture2D* tex = NULL;
                    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
                        PixelStats st = ReadD3D11Texture(dev, ctx, tex);
                        if (st.pct() >= maxPct) { maxPct = st.pct(); maxNb = st.nonBlack; maxTot = st.total; }
                        tex->Release();
                    }
                    res->Release();
                    dupl->ReleaseFrame();
                }
                Sleep(20);
            }
            LogOut("[V13] DXGI 1.5 DuplicateOutput1 (5 frames): frames=%d maskedOut=%d max-non-black=%zu/%zu (%.2f%%)",
                frames, maskedOut, maxNb, maxTot, maxPct);
            dupl->Release();
        } else {
            LogOut("[V13] DXGI 1.5 DuplicateOutput1: BLOCKED/FAILED hr=0x%08X (0.00%% leak)", (unsigned)hr);
        }
        if (ctx) ctx->Release(); if (dev) dev->Release();
    }

    /* V14 Direct3D9 GetFrontBufferData */
    {
        PixelStats st;
        HRESULT hrRes = E_FAIL;
        IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
        if (d3d9) {
            HWND hDummy = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
            D3DPRESENT_PARAMETERS pp = {};
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = hDummy;
            IDirect3DDevice9* dev9 = NULL;
            hrRes = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hDummy, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev9);
            if (SUCCEEDED(hrRes) && dev9) {
                IDirect3DSurface9* surf = NULL;
                hrRes = dev9->CreateOffscreenPlainSurface(g_vw, g_vh, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surf, NULL);
                if (SUCCEEDED(hrRes) && surf) {
                    hrRes = dev9->GetFrontBufferData(0, surf);
                    if (SUCCEEDED(hrRes)) {
                        D3DLOCKED_RECT lr = {};
                        if (SUCCEEDED(surf->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
                            st = CountNonBlackBgraBuffer((const BYTE*)lr.pBits, g_vw, g_vh, lr.Pitch);
                            surf->UnlockRect();
                        }
                    }
                    surf->Release();
                }
                dev9->Release();
            }
            DestroyWindow(hDummy);
            d3d9->Release();
        }
        LogOut("[V14] D3D9 GetFrontBufferData: hr=0x%08X non-black=%zu/%zu (%.2f%%)",
            (unsigned)hrRes, st.nonBlack, st.total, st.pct());
    }

    /* V15 & V16 Dynamic Magnification API */
    {
        HMODULE hMagDll = LoadLibraryW(L"Magnification.dll");
        if (!hMagDll) {
            LogOut("[V15] Magnification API (Normal): BLOCKED (LoadLibraryW(Magnification.dll)=NULL, err=%lu)", GetLastError());
            LogOut("[V16] Magnification API (FilterExclude Curtain): BLOCKED (LoadLibraryW(Magnification.dll)=NULL)");
        } else {
            typedef BOOL(WINAPI* PFN_MagInit)(void);
            typedef BOOL(WINAPI* PFN_MagUninit)(void);
            typedef BOOL(WINAPI* PFN_MagSetCb)(HWND, MagImageScalingCallback);
            typedef BOOL(WINAPI* PFN_MagSetSrc)(HWND, RECT);
            typedef BOOL(WINAPI* PFN_MagSetFlt)(HWND, DWORD, int, HWND*);
            auto pInit   = (PFN_MagInit)GetProcAddress(hMagDll, "MagInitialize");
            auto pUninit = (PFN_MagUninit)GetProcAddress(hMagDll, "MagUninitialize");
            auto pSetCb  = (PFN_MagSetCb)GetProcAddress(hMagDll, "MagSetImageScalingCallback");
            auto pSetSrc = (PFN_MagSetSrc)GetProcAddress(hMagDll, "MagSetWindowSource");
            auto pSetFlt = (PFN_MagSetFlt)GetProcAddress(hMagDll, "MagSetWindowFilterList");

            if (pInit && pInit()) {
                HWND host = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
                    L"STATIC", L"", WS_POPUP | WS_VISIBLE, 0, 0, 400, 400, NULL, NULL, NULL, NULL);
                SetLayeredWindowAttributes(host, 0, 1, LWA_ALPHA);
                HWND hMag = CreateWindowExW(0, L"Magnifier", L"", WS_CHILD | WS_VISIBLE, 0, 0, 400, 400, host, NULL, NULL, NULL);
                if (pSetCb) pSetCb(hMag, MagScaleCallback);

                RECT srcRc = { g_vx + g_vw / 3, g_vy + g_vh / 3, g_vx + g_vw / 3 + 400, g_vy + g_vh / 3 + 400 };
                g_magStats = {}; g_magFired = false;
                if (pSetSrc) pSetSrc(hMag, srcRc);
                LogOut("[V15] Magnification API (Normal): fired=%d non-black=%zu/%zu (%.2f%%)",
                    g_magFired, g_magStats.nonBlack, g_magStats.total, g_magStats.pct());

                /* Find any WDA_MONITOR window to exclude */
                std::vector<HWND> curtains;
                EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                    DWORD aff = 0;
                    if (IsWindowVisible(h) && GetWindowDisplayAffinity(h, &aff) && aff == WDA_MONITOR) {
                        ((std::vector<HWND>*)lp)->push_back(h);
                    }
                    return TRUE;
                }, (LPARAM)&curtains);

                g_magStats = {}; g_magFired = false;
                BOOL fltOk = FALSE;
                if (pSetFlt && !curtains.empty()) {
                    fltOk = pSetFlt(hMag, MW_FILTERMODE_EXCLUDE, (int)curtains.size(), curtains.data());
                }
                if (pSetSrc) pSetSrc(hMag, srcRc);
                LogOut("[V16] Magnification API (FilterExclude WDA_MONITOR count=%zu, ok=%d): fired=%d non-black=%zu/%zu (%.2f%%)",
                    curtains.size(), fltOk, g_magFired, g_magStats.nonBlack, g_magStats.total, g_magStats.pct());

                DestroyWindow(hMag);
                DestroyWindow(host);
                if (pUninit) pUninit();
            } else {
                LogOut("[V15] Magnification API: MagInitialize failed");
                LogOut("[V16] Magnification API: MagInitialize failed");
            }
            FreeLibrary(hMagDll);
        }
    }

    /* V17 WGC CreateForMonitor (10-frame recording) */
    {
        HMONITOR hMon = MonitorFromPoint(POINT{ g_vx + 100, g_vy + 100 }, MONITOR_DEFAULTTOPRIMARY);
        WgcResult wgcMon = RunWgcCapture(true, hMon, NULL, 10);
        LogOut("[V17] WGC CreateForMonitor (10-frame loop): blocked=%d hr=0x%08X frames=%d max-non-black=%zu/%zu (%.2f%%) %s",
            wgcMon.blocked, (unsigned)wgcMon.hr, wgcMon.framesCaptured, wgcMon.maxNonBlack, wgcMon.maxTotal, wgcMon.maxNonBlackPct, wgcMon.note.c_str());
    }

    /* V18 WGC CreateForWindow(target_hwnd) */
    {
        HWND th = (g_targetHwnd && IsWindow(g_targetHwnd)) ? g_targetHwnd : GetForegroundWindow();
        WgcResult wgcWin = RunWgcCapture(false, NULL, th, 5);
        LogOut("[V18] WGC CreateForWindow(0x%llX): blocked=%d hr=0x%08X frames=%d max-non-black=%zu/%zu (%.2f%%) %s",
            (unsigned long long)(ULONG_PTR)th, wgcWin.blocked, (unsigned)wgcWin.hr, wgcWin.framesCaptured,
            wgcWin.maxNonBlack, wgcWin.maxTotal, wgcWin.maxNonBlackPct, wgcWin.note.c_str());
    }

    /* V19 user32!DwmGetDxSharedSurface */
    {
        typedef BOOL(WINAPI* PFN_DwmGetDxSharedSurface)(HWND, HANDLE*, LUID*, ULONG*, ULONG*, ULONGLONG*);
        auto fn = (PFN_DwmGetDxSharedSurface)GetProcAddress(GetModuleHandleW(L"user32.dll"), "DwmGetDxSharedSurface");
        HWND th = (g_targetHwnd && IsWindow(g_targetHwnd)) ? g_targetHwnd : GetForegroundWindow();
        if (fn && th) {
            HANDLE hSurf = NULL; LUID luid = {}; ULONG fmt = 0, flags = 0; ULONGLONG upd = 0;
            BOOL ok = fn(th, &hSurf, &luid, &fmt, &flags, &upd);
            double pct = 0.0;
            if (ok && hSurf) {
                ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; D3D_FEATURE_LEVEL fl;
                if (SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
                    ID3D11Texture2D* tex = NULL;
                    if (SUCCEEDED(dev->OpenSharedResource(hSurf, IID_PPV_ARGS(&tex))) && tex) {
                        pct = ReadD3D11Texture(dev, ctx, tex).pct();
                        tex->Release();
                    }
                    ctx->Release(); dev->Release();
                }
            }
            LogOut("[V19] user32!DwmGetDxSharedSurface(0x%llX): ok=%d hSurf=%p non-black=%.2f%%",
                (unsigned long long)(ULONG_PTR)th, ok, hSurf, pct);
        } else {
            LogOut("[V19] user32!DwmGetDxSharedSurface: unavailable or invalid target HWND");
        }
    }

    /* V20 Continuous 20-frame GDI gdigrab recording loop */
    {
        HDC hdcScreen = GetDC(NULL);
        double maxPct = 0.0; size_t maxNb = 0, maxTot = 0;
        for (int f = 0; f < 20; f++) {
            PixelStats st = CaptureBitBltRegion(hdcScreen, g_vx + g_vw / 4, g_vy + g_vh / 4, 1280, 720, SRCCOPY | CAPTUREBLT);
            if (st.pct() >= maxPct) { maxPct = st.pct(); maxNb = st.nonBlack; maxTot = st.total; }
            Sleep(25);
        }
        ReleaseDC(NULL, hdcScreen);
        LogOut("[V20] Continuous 20-frame GDI gdigrab loop: max-non-black=%zu/%zu (%.2f%%)",
            maxNb, maxTot, maxPct);
    }
}

/* ---------------- Group C: Red-Team Attacks & Hardware-DRM Proof ---------------- */
static DWORD FindShieldPid()
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, g_targetProc.c_str()) == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    return pid;
}

static void RunGroupC_RedTeam()
{
    LogOut("\n=== GROUP C: Red-Team Attacks Against %ls (V21-V27) ===", g_targetProc.c_str());

    /* V21 Legacy IPC Backdoor Check */
    {
        HANDLE h1 = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, L"Global\\ApexShot_Unmask_Event_2026");
        HANDLE h2 = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, L"Global\\ApexShot_Mask_Event_2026");
        bool pass = (h1 == NULL && h2 == NULL);
        if (h1) CloseHandle(h1);
        if (h2) CloseHandle(h2);
        LogOut("[V21] %s Legacy IPC Backdoor Check: UnmaskEvent=%p MaskEvent=%p",
            pass ? "PASS" : "FAIL", h1, h2);
    }

    /* V22 Process Kill / Memory Dump / Injection Check */
    {
        DWORD pid = FindShieldPid();
        HANDLE hTerm = pid ? OpenProcess(PROCESS_TERMINATE, FALSE, pid) : NULL;
        DWORD errTerm = hTerm ? 0 : GetLastError();
        HANDLE hRead = pid ? OpenProcess(PROCESS_VM_READ, FALSE, pid) : NULL;
        DWORD errRead = hRead ? 0 : GetLastError();
        HANDLE hWrite = pid ? OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD, FALSE, pid) : NULL;
        DWORD errWrite = hWrite ? 0 : GetLastError();
        if (hTerm) CloseHandle(hTerm);
        if (hRead) CloseHandle(hRead);
        if (hWrite) CloseHandle(hWrite);
        bool pass = (pid != 0 && hTerm == NULL && errTerm == ERROR_ACCESS_DENIED &&
                     hRead == NULL && errRead == ERROR_ACCESS_DENIED &&
                     hWrite == NULL && errWrite == ERROR_ACCESS_DENIED);
        LogOut("[V22] %s Kernel DACL Anti-Kill/Dump/Inject (PID=%lu): TermErr=%lu ReadErr=%lu WriteErr=%lu",
            pass ? "PASS" : "FAIL", pid, errTerm, errRead, errWrite);
    }

    /* V23 Stealth Fingerprint Scan */
    {
        const wchar_t* probes[] = { L"AegisBlackout", L"BlackoutCurtainClass", L"BlackoutCurtain", L"ApexShotCanvasClass", L"ApexShield" };
        int hits = 0;
        for (auto s : probes) {
            if (FindWindowW(s, NULL) || FindWindowW(NULL, s)) hits++;
        }
        LogOut("[V23] %s Stealth Fingerprint Scan: signatureHits=%d/5", (hits == 0) ? "PASS" : "FAIL", hits);
    }

    /* V24 Window Tampering Attack */
    {
        HWND hCurtain = NULL;
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            DWORD aff = 0; RECT rc = {};
            if (IsWindowVisible(h) && GetWindowDisplayAffinity(h, &aff) && aff == WDA_MONITOR && GetWindowRect(h, &rc)) {
                if ((rc.right - rc.left) >= g_vw && (rc.bottom - rc.top) >= g_vh) {
                    *(HWND*)lp = h;
                    return FALSE;
                }
            }
            return TRUE;
        }, (LPARAM)&hCurtain);

        bool pass = false;
        if (hCurtain) {
            ShowWindow(hCurtain, SW_HIDE);
            MoveWindow(hCurtain, -9999, -9999, 1, 1, TRUE);
            SetWindowPos(hCurtain, HWND_BOTTOM, 0, 0, 1, 1, SWP_NOACTIVATE);
            PostMessageW(hCurtain, WM_CLOSE, 0, 0);
            Sleep(200);

            BOOL stillAlive = IsWindow(hCurtain);
            BOOL stillVis = IsWindowVisible(hCurtain);
            RECT afterRc = {};
            GetWindowRect(hCurtain, &afterRc);
            DWORD affAfter = 0;
            GetWindowDisplayAffinity(hCurtain, &affAfter);

            HDC hdc = GetDC(NULL);
            PixelStats ps = CaptureBitBltRegion(hdc, g_vx + g_vw / 4, g_vy + g_vh / 4, 800, 600, SRCCOPY | CAPTUREBLT);
            ReleaseDC(NULL, hdc);

            pass = stillAlive && stillVis &&
                   (afterRc.right - afterRc.left) >= g_vw &&
                   (afterRc.bottom - afterRc.top) >= g_vh &&
                   affAfter == WDA_MONITOR && ps.nonBlack == 0;
            LogOut("[V24] %s Window Tampering Attack (curtain=0x%llX): alive=%d vis=%d size=%ldx%ld aff=%lu non-black=%.2f%%",
                pass ? "PASS" : "FAIL", (unsigned long long)(ULONG_PTR)hCurtain,
                stillAlive, stillVis, afterRc.right - afterRc.left, afterRc.bottom - afterRc.top, affAfter, ps.pct());
        } else {
            LogOut("[V24] FAIL Window Tampering Attack: could not find fullscreen WDA_MONITOR curtain");
        }
    }

    /* V25 Synthetic SendInput Spoofing Attack */
    {
        auto sendChord = [](WORD vkMain, bool withShift) {
            std::vector<INPUT> seq;
            auto pushKey = [&](WORD vk, DWORD flags) {
                INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwFlags = flags;
                seq.push_back(in);
            };
            pushKey(VK_CONTROL, 0);
            pushKey(VK_MENU, 0);
            if (withShift) pushKey(VK_SHIFT, 0);
            pushKey(vkMain, 0);
            pushKey(vkMain, KEYEVENTF_KEYUP);
            if (withShift) pushKey(VK_SHIFT, KEYEVENTF_KEYUP);
            pushKey(VK_MENU, KEYEVENTF_KEYUP);
            pushKey(VK_CONTROL, KEYEVENTF_KEYUP);
            SendInput((UINT)seq.size(), seq.data(), sizeof(INPUT));
        };

        /* Spoof Ctrl+Alt+X */
        sendChord('X', false);
        HDC hdc = GetDC(NULL);
        double maxLeak = 0.0;
        for (int i = 0; i < 6; i++) {
            Sleep(25);
            PixelStats ps = CaptureBitBltRegion(hdc, g_vx + g_vw / 4, g_vy + g_vh / 4, 600, 600, SRCCOPY | CAPTUREBLT);
            if (ps.pct() > maxLeak) maxLeak = ps.pct();
        }
        ReleaseDC(NULL, hdc);

        /* Spoof emergency exit Ctrl+Alt+Shift+End */
        sendChord(VK_END, true);
        Sleep(150);
        DWORD pidAfter = FindShieldPid();

        int wdaCount = 0;
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            DWORD aff = 0;
            if (IsWindowVisible(h) && GetWindowDisplayAffinity(h, &aff) && aff == WDA_MONITOR) (*(int*)lp)++;
            return TRUE;
        }, (LPARAM)&wdaCount);

        bool pass = (maxLeak == 0.0 && pidAfter != 0 && wdaCount == 1);
        LogOut("[V25] %s Synthetic SendInput Spoof (Ctrl+Alt+X + Ctrl+Alt+Shift+End): maxLeak=%.2f%% pidAlive=%lu wdaWindows=%d",
            pass ? "PASS" : "FAIL", maxLeak, pidAfter, wdaCount);
    }

    /* V26 Hotkey-Scanner / Zero-Conflict Check */
    {
        struct HKSpec { const char* name; UINT mods; UINT vk; };
        HKSpec specs[] = {
            { "Alt+A",      MOD_ALT | MOD_NOREPEAT,               'A' },
            { "Ctrl+Alt+A", MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A' },
            { "Ctrl+Alt+S", MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S' },
            { "Ctrl+Alt+X", MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'X' },
        };
        std::string details;
        int succeeded = 0;
        for (int i = 0; i < 4; i++) {
            BOOL ok = RegisterHotKey(NULL, 950 + i, specs[i].mods, specs[i].vk);
            DWORD err = ok ? 0 : GetLastError();
            if (ok) { UnregisterHotKey(NULL, 950 + i); succeeded++; }
            char tmp[64];
            sprintf_s(tmp, sizeof(tmp), "%s=%s(%lu) ", specs[i].name, ok ? "FREE" : "OCCUPIED_BY_OTHER", err);
            details += tmp;
        }
        LogOut("[V26] %s Hotkey-Scanner Check (%d/4 free): %s",
            (succeeded == 4) ? "PASS" : "INFO", succeeded, details.c_str());
    }

    /* V27 Topmost Z-Order Hijack Attack */
    {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            if (m == WM_PAINT) {
                PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
                HBRUSH br = CreateSolidBrush(RGB(255, 0, 0));
                FillRect(dc, &ps.rcPaint, br);
                DeleteObject(br);
                EndPaint(h, &ps);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
        };
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"RedTeamTopmostProbeClass";
        RegisterClassW(&wc);

        HWND hRed = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            wc.lpszClassName, L"RedProbe", WS_POPUP | WS_VISIBLE,
            400, 400, 600, 600, NULL, NULL, wc.hInstance, NULL);
        SetWindowPos(hRed, HWND_TOPMOST, 400, 400, 600, 600, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        UpdateWindow(hRed);

        for (int i = 0; i < 15; i++) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Sleep(20);
        }

        HDC hdc = GetDC(NULL);
        PixelStats psGdi = CaptureBitBltRegion(hdc, 450, 450, 500, 500, SRCCOPY | CAPTUREBLT);
        ReleaseDC(NULL, hdc);

        /* Also capture via DXGI over red window! */
        double dxgiPct = -1.0;
        ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; IDXGIOutputDuplication* dupl = NULL; HRESULT hr = S_OK;
        if (InitD3D11AndDupl(false, &dev, &ctx, &dupl, &hr)) {
            for (int a = 0; a < 3; a++) {
                DXGI_OUTDUPL_FRAME_INFO fi = {}; IDXGIResource* res = NULL;
                if (SUCCEEDED(dupl->AcquireNextFrame(100, &fi, &res))) {
                    ID3D11Texture2D* tex = NULL;
                    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
                        dxgiPct = ReadD3D11Texture(dev, ctx, tex).pct();
                        tex->Release();
                    }
                    res->Release(); dupl->ReleaseFrame();
                    if (fi.AccumulatedFrames > 0) break;
                }
            }
            dupl->Release();
        }
        if (ctx) ctx->Release(); if (dev) dev->Release();
        DestroyWindow(hRed);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);

        bool pass = (psGdi.nonBlack == 0 && (dxgiPct <= 0.0));
        LogOut("[V27] %s Topmost Z-Order Hijack (600x600 RGB(255,0,0) window): GDI_nonBlack=%.2f%% DXGI_nonBlack=%.2f%%",
            pass ? "PASS" : "FAIL", psGdi.pct(), dxgiPct);
    }
}

/* ---------------- Group D: Sandbox Disk-Scan & Isolation Audit (V28) ---------------- */
static void RunGroupD_DiskAndSandbox()
{
    LogOut("\n=== GROUP D: Sandbox Host Disk-Scan, DLL Lockdown & Window Isolation (V28) ===");
    std::vector<std::wstring> dirs;

    wchar_t userProf[MAX_PATH] = { 0 };
    DWORD upLen = GetEnvironmentVariableW(L"USERPROFILE", userProf, MAX_PATH);
    if (upLen > 0 && upLen < MAX_PATH) {
        dirs.push_back(std::wstring(userProf) + L"\\Desktop\\*");
        dirs.push_back(std::wstring(userProf) + L"\\Pictures\\*");
        dirs.push_back(std::wstring(userProf) + L"\\Downloads\\*");
        dirs.push_back(std::wstring(userProf) + L"\\Documents\\*");
    }
    wchar_t modDir[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(NULL, modDir, MAX_PATH) > 0) {
        wchar_t* slash = wcsrchr(modDir, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            dirs.push_back(std::wstring(modDir) + L"*");
        }
    }

    size_t totalLeakedEntries = 0;
    for (const auto& pat : dirs) {
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = FindFirstFileW(pat.c_str(), &fd);
        size_t count = 0;
        std::wstring sample;
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                    count++;
                    if (sample.empty()) sample = fd.cFileName;
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
            totalLeakedEntries += count;
            LogOut("[V28-DISK] %ls -> VISIBLE entries=%zu (first=%ls)", pat.c_str(), count, sample.empty() ? L"<none>" : sample.c_str());
        } else {
            LogOut("[V28-DISK] %ls -> BLOCKED (err=%lu)", pat.c_str(), GetLastError());
        }
    }

    const wchar_t* dlls[] = {
        L"GraphicsCapture.dll",
        L"Magnification.dll",
        L"CaptureService.dll",
        L"Windows.Internal.CapturePicker.dll",
    };
    for (auto d : dlls) {
        HMODULE h = LoadLibraryW(d);
        DWORD err = h ? 0 : GetLastError();
        if (h) FreeLibrary(h);
        LogOut("[V28-DLL] LoadLibraryW(%ls) -> %s (err=%lu)", d, h ? "LOADED" : "BLOCKED", err);
    }

    int titledVis = 0;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (IsWindowVisible(h)) {
            WCHAR t[64] = L"";
            if (GetWindowTextW(h, t, 63) > 0) (*(int*)lp)++;
        }
        return TRUE;
    }, (LPARAM)&titledVis);
    LogOut("[V28-HWND] Visible titled windows seen by EnumWindows=%d | Total disk entries seen=%zu",
        titledVis, totalLeakedEntries);
}

/* ---------------- Group E: GPU HW_PROTECTED CopyResource / Cross-Device Audit (V29) ---------------- */
struct HwReadbackStats {
    HRESULT stgHr = E_FAIL;
    HRESULT mapHr = E_FAIL;
    size_t total = 0;
    size_t exactPlain = 0;
    size_t uniq = 0;
};

static HwReadbackStats ReadbackTestTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src)
{
    HwReadbackStats st;
    if (!dev || !ctx || !src) return st;
    D3D11_TEXTURE2D_DESC d = {}; src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;

    ID3D11Texture2D* stg = nullptr;
    st.stgHr = dev->CreateTexture2D(&sd, nullptr, &stg);
    if (FAILED(st.stgHr) || !stg) return st;

    ctx->CopyResource(stg, src);
    ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    st.mapHr = ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(st.mapHr) && mapped.pData) {
        std::set<uint32_t> uniq;
        for (int y = 0; y < (int)d.Height; y += 2) {
            const BYTE* row = (const BYTE*)mapped.pData + (size_t)y * mapped.RowPitch;
            for (int x = 0; x < (int)d.Width; x += 2) {
                const BYTE* px = row + (size_t)x * 4;
                st.total++;
                if (px[2] == 255 && px[1] == 0 && px[0] == 255 && px[3] == 255) st.exactPlain++;
                if (uniq.size() < 4096) {
                    uniq.insert(((uint32_t)px[3] << 24) | ((uint32_t)px[2] << 16) | ((uint32_t)px[1] << 8) | px[0]);
                }
            }
        }
        st.uniq = uniq.size();
        ctx->Unmap(stg, 0);
    }
    stg->Release();
    return st;
}

static void RunGroupE_HwDrmAudit()
{
    LogOut("\n=== GROUP E: GPU HW_PROTECTED CopyResource & Cross-Device Enforcement (V29) ===");
    ID3D11Device* devA = nullptr; ID3D11DeviceContext* ctxA = nullptr; D3D_FEATURE_LEVEL flA;
    ID3D11Device* devB = nullptr; ID3D11DeviceContext* ctxB = nullptr; D3D_FEATURE_LEVEL flB;
    HRESULT hrA = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &devA, &flA, &ctxA);
    HRESULT hrB = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &devB, &flB, &ctxB);
    if (FAILED(hrA) || FAILED(hrB) || !devA || !devB) {
        LogOut("[V29] SKIP Hardware D3D11 devices unavailable (hrA=0x%08X hrB=0x%08X)", (unsigned)hrA, (unsigned)hrB);
        if (ctxA) ctxA->Release(); if (devA) devA->Release();
        if (ctxB) ctxB->Release(); if (devB) devB->Release();
        return;
    }

    const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };

    /* 1) SwapChain HW_PROTECTED|DISPLAY_ONLY (0x420) same-device CopyResource & handle export */
    IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fac = nullptr;
    devA->QueryInterface(IID_PPV_ARGS(&dd));
    if (dd) dd->GetAdapter(&ad);
    if (ad) ad->GetParent(IID_PPV_ARGS(&fac));

    bool scPass = false;
    if (fac) {
        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = 256; scd.Height = 256; scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        scd.Flags = DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED | DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY;

        IDXGISwapChain1* sc = nullptr;
        HRESULT hrSc = fac->CreateSwapChainForComposition(devA, &scd, nullptr, &sc);
        if (SUCCEEDED(hrSc) && sc) {
            ID3D11Texture2D* bb = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
                D3D11_TEXTURE2D_DESC bd = {}; bb->GetDesc(&bd);
                ID3D11RenderTargetView* rtv = nullptr;
                if (SUCCEEDED(devA->CreateRenderTargetView(bb, nullptr, &rtv)) && rtv) {
                    ctxA->ClearRenderTargetView(rtv, magenta);
                    ctxA->Flush();
                    rtv->Release();
                }
                HwReadbackStats st = ReadbackTestTexture(devA, ctxA, bb);
                IDXGIResource* res0 = nullptr; HANDLE hLeg = nullptr; HRESULT hrLeg = E_FAIL;
                if (SUCCEEDED(bb->QueryInterface(IID_PPV_ARGS(&res0))) && res0) {
                    hrLeg = res0->GetSharedHandle(&hLeg);
                    res0->Release();
                }
                IDXGIResource1* res1 = nullptr; HANDLE hNt = nullptr; HRESULT hrNt = E_FAIL;
                if (SUCCEEDED(bb->QueryInterface(IID_PPV_ARGS(&res1))) && res1) {
                    hrNt = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &hNt);
                    if (hNt) CloseHandle(hNt);
                    res1->Release();
                }
                scPass = ((bd.MiscFlags & D3D11_RESOURCE_MISC_HW_PROTECTED) != 0) &&
                         (st.exactPlain == 0) && (st.uniq >= 256 || FAILED(st.mapHr)) &&
                         FAILED(hrLeg) && FAILED(hrNt);
                LogOut("[V29-SC] %s SwapChain(0x420) BackBufMisc=0x%X SameDevPlain=%zu/%zu Uniq=%zu SharedHr=(0x%08X,0x%08X)",
                    scPass ? "PASS" : "INFO", bd.MiscFlags, st.exactPlain, st.total, st.uniq, (unsigned)hrLeg, (unsigned)hrNt);
                bb->Release();
            }
            sc->Release();
        } else {
            LogOut("[V29-SC] INFO SwapChain(0x420) not supported on GPU hr=0x%08X", (unsigned)hrSc);
        }
    }
    if (fac) fac->Release(); if (ad) ad->Release(); if (dd) dd->Release();

    /* 2) Cross-Device NT-Shared HW_PROTECTED texture readback via Independent Device B */
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 256; td.Height = 256; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_HW_PROTECTED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ID3D11Texture2D* texA = nullptr;
    HRESULT hrTex = devA->CreateTexture2D(&td, nullptr, &texA);
    if (SUCCEEDED(hrTex) && texA) {
        IDXGIKeyedMutex* kmA = nullptr;
        if (SUCCEEDED(texA->QueryInterface(IID_PPV_ARGS(&kmA))) && kmA) {
            if (SUCCEEDED(kmA->AcquireSync(0, 1000))) {
                ID3D11RenderTargetView* rtv = nullptr;
                if (SUCCEEDED(devA->CreateRenderTargetView(texA, nullptr, &rtv)) && rtv) {
                    ctxA->ClearRenderTargetView(rtv, magenta);
                    ctxA->Flush();
                    rtv->Release();
                }
                kmA->ReleaseSync(0);
            }
            kmA->Release();
        }
        IDXGIResource1* r1 = nullptr; HANDLE hNt = nullptr;
        if (SUCCEEDED(texA->QueryInterface(IID_PPV_ARGS(&r1))) && r1) {
            if (SUCCEEDED(r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &hNt)) && hNt) {
                ID3D11Device1* devB1 = nullptr;
                if (SUCCEEDED(devB->QueryInterface(IID_PPV_ARGS(&devB1))) && devB1) {
                    ID3D11Texture2D* texB = nullptr;
                    if (SUCCEEDED(devB1->OpenSharedResource1(hNt, IID_PPV_ARGS(&texB))) && texB) {
                        D3D11_TEXTURE2D_DESC bd = {}; texB->GetDesc(&bd);
                        IDXGIKeyedMutex* kmB = nullptr;
                        if (SUCCEEDED(texB->QueryInterface(IID_PPV_ARGS(&kmB))) && kmB) {
                            if (SUCCEEDED(kmB->AcquireSync(0, 1000))) {
                                HwReadbackStats stB = ReadbackTestTexture(devB, ctxB, texB);
                                kmB->ReleaseSync(0);
                                bool xPass = ((bd.MiscFlags & D3D11_RESOURCE_MISC_HW_PROTECTED) != 0) &&
                                             (stB.exactPlain == 0) && (stB.uniq >= 256 || FAILED(stB.mapHr));
                                LogOut("[V29-XDEV] %s IndependentDeviceB OpenSharedResource1->CopyResource: Misc=0x%X Plain=%zu/%zu Uniq=%zu",
                                    xPass ? "PASS" : "INFO", bd.MiscFlags, stB.exactPlain, stB.total, stB.uniq);
                            }
                            kmB->Release();
                        }
                        texB->Release();
                    }
                    devB1->Release();
                }
                CloseHandle(hNt);
            }
            r1->Release();
        }
        texA->Release();
    } else {
        LogOut("[V29-XDEV] INFO HW_PROTECTED shared texture hr=0x%08X", (unsigned)hrTex);
    }

    ctxB->Release(); devB->Release();
    ctxA->Release(); devA->Release();
}

int main(int argc, char** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    HDESK hDesk = OpenDesktopW(L"Default", 0, FALSE, 0x01FF);
    if (hDesk) SetThreadDesktop(hDesk);

    std::string outPath;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            g_mode = argv[++i];
        } else if (strcmp(argv[i], "--target-hwnd") == 0 && i + 1 < argc) {
            g_targetHwnd = (HWND)(ULONG_PTR)strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--target-proc") == 0 && i + 1 < argc) {
            const char* p = argv[++i];
            WCHAR wp[MAX_PATH] = { 0 };
            MultiByteToWideChar(CP_UTF8, 0, p, -1, wp, MAX_PATH);
            g_targetProc = wp;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outPath = argv[++i];
        }
    }

    if (!outPath.empty()) {
        fopen_s(&g_outFile, outPath.c_str(), "w");
    }

    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_vw <= 0) g_vw = 3840;
    if (g_vh <= 0) g_vh = 2160;

    HWND fg = GetForegroundWindow();
    LogOut("====================================================================");
    LogOut("AegisBlackout 29-Vector Capture & Hardware-DRM Audit Suite (mode=%s)", g_mode.c_str());
    LogOut("VirtualScreen=(%d,%d %dx%d) ForegroundHWND=0x%llX TargetHWND=0x%llX TargetProc=%ls",
        g_vx, g_vy, g_vw, g_vh, (unsigned long long)(ULONG_PTR)fg, (unsigned long long)(ULONG_PTR)g_targetHwnd, g_targetProc.c_str());
    LogOut("====================================================================");

    if (!fg && g_mode == "host") {
        LogOut("WARNING: GetForegroundWindow()==NULL, desktop attachment check!");
    }

    RunGroupA();
    RunGroupB();
    if (g_mode == "host") {
        RunGroupC_RedTeam();
        RunGroupE_HwDrmAudit();
    }
    RunGroupD_DiskAndSandbox();

    LogOut("\n=== AUDIT COMPLETE ===");
    if (g_outFile) fclose(g_outFile);
    if (hDesk) CloseDesktop(hDesk);
    return 0;
}
