#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <aclapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef WDA_MONITOR
#define WDA_MONITOR 0x00000001
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef HID_USAGE_PAGE_GENERIC
#define HID_USAGE_PAGE_GENERIC 0x01
#endif
#ifndef HID_USAGE_GENERIC_MOUSE
#define HID_USAGE_GENERIC_MOUSE 0x02
#endif
#ifndef HID_USAGE_GENERIC_KEYBOARD
#define HID_USAGE_GENERIC_KEYBOARD 0x06
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
#ifndef DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY
#define DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY 0x20
#endif
#ifndef DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED
#define DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED 0x400
#endif
#ifndef D3D11_RESOURCE_MISC_HW_PROTECTED
#define D3D11_RESOURCE_MISC_HW_PROTECTED 0x80000
#endif

#define WM_APP_RESTORE_CURTAIN (WM_APP + 1)
#define TIMER_WATCHDOG_ID      1
#define TIMER_HWDRM_HEARTBEAT  2

/* Startup-only diagnostic logger (never writes during runtime screenshot events) */
static void StartupLog(const char* line, BOOL overwrite)
{
    wchar_t logPath[MAX_PATH] = { 0 };
    DWORD len = GetModuleFileNameW(NULL, logPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(logPath, L'\\');
    if (slash) {
        *(slash + 1) = L'\0';
    } else {
        logPath[0] = L'\0';
    }
    if (wcscat_s(logPath, MAX_PATH, L"aegis_init.log") != 0) return;

    FILE* f = NULL;
    if (_wfopen_s(&f, logPath, overwrite ? L"w" : L"a") == 0 && f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

static DWORD RandU32(void)
{
    DWORD v = 0;
    if (BCryptGenRandom(NULL, (PUCHAR)&v, sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        v = (DWORD)GetTickCount64() ^ GetCurrentProcessId();
    }
    return v;
}

static HINSTANCE g_hInst = NULL;
static HWND      g_hCurtain = NULL;
static HWND      g_hCanvasWnd = NULL;
static HWND      g_hInputWnd = NULL;
static HWINEVENTHOOK g_hWinEventFg = NULL;
static HWINEVENTHOOK g_hWinEventShow = NULL;

static int g_vx = 0, g_vy = 0, g_vw = 3840, g_vh = 2160;
static volatile BOOL g_bInternalUnmasking = FALSE;

/* 24x7 GPU Hardware-Protected DRM Overlay state */
static ID3D11Device*           g_pD3DDev      = nullptr;
static ID3D11DeviceContext*    g_pD3DCtx      = nullptr;
static IDXGISwapChain1*        g_pSwapChain   = nullptr;
static ID3D11RenderTargetView* g_pRTV         = nullptr;
static IDCompositionDevice*    g_pDCompDev    = nullptr;
static IDCompositionTarget*    g_pDCompTarget = nullptr;
static IDCompositionVisual*    g_pDCompVisual = nullptr;
static BOOL    g_bHwDrmActive   = FALSE;
static int     g_scW            = 0;
static int     g_scH            = 0;
static UINT    g_actualScFlags  = 0;
static UINT    g_actualBbMisc   = 0;
static HRESULT g_lastCommitHr   = E_FAIL;

/* Offscreen 4K snapshot buffers (scrubbed with SecureZeroMemory on close) */
static HDC     g_hdcPristine  = NULL;
static HBITMAP g_hbmPristine  = NULL;
static void*   g_pBitsPristine = NULL;

static HDC     g_hdcDarkened  = NULL;
static HBITMAP g_hbmDarkened  = NULL;
static void*   g_pBitsDark    = NULL;

/* Canvas interactive selection state */
static BOOL  g_bSelecting = FALSE;
static BOOL  g_bSelected  = FALSE;
static POINT g_ptStart    = { 0, 0 };
static POINT g_ptCurrent  = { 0, 0 };
static RECT  g_rcSelection = { 0, 0, 0, 0 };

/* Hardware-only Raw Input state */
static BOOL g_physLCtrl = FALSE, g_physRCtrl = FALSE;
static BOOL g_physLAlt  = FALSE, g_physRAlt  = FALSE;
static BOOL g_physLShift = FALSE, g_physRShift = FALSE;
static ULONGLONG g_lastSnipTriggerTick   = 0;
static ULONGLONG g_lastPhysMouseDownTick = 0;
static ULONGLONG g_canvasOpenedTick      = 0;

/* Verified physical HID device cache */
#define MAX_DEV_CACHE 32
struct DevCacheEntry {
    HANDLE hDevice;
    BOOL   isPhysical;
};
static DevCacheEntry g_devCache[MAX_DEV_CACHE];
static int g_devCacheCount = 0;

static void RefreshVirtualScreenBounds(void)
{
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_vx = x;
    g_vy = y;
    g_vw = (w > 0) ? w : 3840;
    g_vh = (h > 0) ? h : 2160;
}

static void ReleaseHwDrmOverlay(void)
{
    if (g_pDCompVisual) { g_pDCompVisual->Release(); g_pDCompVisual = nullptr; }
    if (g_pDCompTarget) { g_pDCompTarget->Release(); g_pDCompTarget = nullptr; }
    if (g_pDCompDev)    { g_pDCompDev->Release();    g_pDCompDev = nullptr; }
    if (g_pRTV)         { g_pRTV->Release();         g_pRTV = nullptr; }
    if (g_pSwapChain)   { g_pSwapChain->Release();   g_pSwapChain = nullptr; }
    if (g_pD3DCtx)      { g_pD3DCtx->Release();      g_pD3DCtx = nullptr; }
    if (g_pD3DDev)      { g_pD3DDev->Release();      g_pD3DDev = nullptr; }
    g_bHwDrmActive  = FALSE;
    g_scW = g_scH   = 0;
    g_actualScFlags = 0;
    g_actualBbMisc  = 0;
    g_lastCommitHr  = E_FAIL;
}

static BOOL InitHwDrmOverlay(HWND hwndCurtain)
{
    ReleaseHwDrmOverlay();
    if (!hwndCurtain || g_vw <= 0 || g_vh <= 0) return FALSE;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &g_pD3DDev, &fl, &g_pD3DCtx
    );
    if (FAILED(hr) || !g_pD3DDev || !g_pD3DCtx) {
        ReleaseHwDrmOverlay();
        return FALSE;
    }

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory2 = nullptr;

    hr = g_pD3DDev->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (SUCCEEDED(hr) && dxgiDev) hr = dxgiDev->GetAdapter(&adapter);
    if (SUCCEEDED(hr) && adapter) hr = adapter->GetParent(IID_PPV_ARGS(&factory2));

    if (SUCCEEDED(hr) && factory2) {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width       = (UINT)g_vw;
        desc.Height      = (UINT)g_vh;
        desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Stereo      = FALSE;
        desc.SampleDesc.Count   = 1;
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling     = DXGI_SCALING_STRETCH;
        desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;

        const UINT candidates[] = {
            DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED | DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY,
            DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED,
            DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY,
        };
        for (UINT flags : candidates) {
            desc.Flags = flags;
            hr = factory2->CreateSwapChainForComposition(g_pD3DDev, &desc, nullptr, &g_pSwapChain);
            if (SUCCEEDED(hr) && g_pSwapChain) break;
        }
    }

    if (g_pSwapChain) {
        DXGI_SWAP_CHAIN_DESC1 actual = {};
        if (SUCCEEDED(g_pSwapChain->GetDesc1(&actual))) {
            g_actualScFlags = actual.Flags;
        }

        ID3D11Texture2D* pBackBuf = nullptr;
        hr = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuf));
        if (SUCCEEDED(hr) && pBackBuf) {
            D3D11_TEXTURE2D_DESC bbDesc = {};
            pBackBuf->GetDesc(&bbDesc);
            g_actualBbMisc = bbDesc.MiscFlags;
            hr = g_pD3DDev->CreateRenderTargetView(pBackBuf, nullptr, &g_pRTV);
            pBackBuf->Release();
        }
    }

    if (SUCCEEDED(hr) && g_pRTV) {
        const float clearRgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f / 255.0f };
        g_pD3DCtx->ClearRenderTargetView(g_pRTV, clearRgba);
        hr = g_pSwapChain->Present(0, 0);
    }

    if (SUCCEEDED(hr) && dxgiDev) {
        hr = DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&g_pDCompDev));
        if (SUCCEEDED(hr) && g_pDCompDev) {
            hr = g_pDCompDev->CreateTargetForHwnd(hwndCurtain, TRUE, &g_pDCompTarget);
        }
        if (SUCCEEDED(hr) && g_pDCompDev) {
            hr = g_pDCompDev->CreateVisual(&g_pDCompVisual);
        }
        if (SUCCEEDED(hr) && g_pDCompVisual) {
            hr = g_pDCompVisual->SetContent(g_pSwapChain);
        }
        if (SUCCEEDED(hr) && g_pDCompTarget) {
            hr = g_pDCompTarget->SetRoot(g_pDCompVisual);
        }
        if (SUCCEEDED(hr) && g_pDCompDev) {
            g_lastCommitHr = g_pDCompDev->Commit();
            hr = g_lastCommitHr;
        }
    }

    if (factory2) factory2->Release();
    if (adapter)  adapter->Release();
    if (dxgiDev)  dxgiDev->Release();

    if (FAILED(hr) || !g_pSwapChain || !g_pRTV || !g_pDCompVisual || !g_pDCompTarget || FAILED(g_lastCommitHr)) {
        ReleaseHwDrmOverlay();
        return FALSE;
    }

    g_scW = g_vw;
    g_scH = g_vh;
    g_bHwDrmActive = TRUE;
    return TRUE;
}

static void PresentHwDrmFrame(void)
{
    if (!g_bHwDrmActive || !g_pD3DCtx || !g_pRTV || !g_pSwapChain) return;

    const float clearRgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f / 255.0f };
    g_pD3DCtx->ClearRenderTargetView(g_pRTV, clearRgba);
    HRESULT hr = g_pSwapChain->Present(0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        ReleaseHwDrmOverlay();
        InitHwDrmOverlay(g_hCurtain);
    }
}

static void HardenProcessSecurity(void)
{
    /*
     * Restrict process DACL:
     *   SYSTEM (SY) + Elevated Administrators (BA) -> GenericAll (GA)
     *   Everyone (WD) -> SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION (0x00101000)
     */
    PSECURITY_DESCRIPTOR pSD = NULL;
    const wchar_t* sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00101000;;;WD)";
    DWORD daclRes = ERROR_SUCCESS;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &pSD, NULL)) {
        BOOL present = FALSE, defaulted = FALSE;
        PACL pDacl = NULL;
        if (GetSecurityDescriptorDacl(pSD, &present, &pDacl, &defaulted) && present) {
            daclRes = SetSecurityInfo(
                GetCurrentProcess(),
                SE_KERNEL_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                NULL, NULL, pDacl, NULL
            );
        } else {
            daclRes = GetLastError();
        }
        LocalFree(pSD);
    } else {
        daclRes = GetLastError();
    }

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ep = { 0 };
    ep.DisableExtensionPoints = 1;
    BOOL epOk = SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ep, sizeof(ep));

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY il = { 0 };
    il.NoRemoteImages = 1;
    il.NoLowMandatoryLabelImages = 1;
    BOOL ilOk = SetProcessMitigationPolicy(ProcessImageLoadPolicy, &il, sizeof(il));

    char msg[160];
    sprintf_s(msg, sizeof(msg),
        "Hardening: DACL_res=%lu ExtPt=%d (err=%lu) ImgLoad=%d (err=%lu)",
        daclRes, epOk, epOk ? 0UL : GetLastError(), ilOk, ilOk ? 0UL : GetLastError());
    StartupLog(msg, FALSE);
}

static BOOL IsPhysicalHardwareDevice(HANDLE hDevice)
{
    if (!hDevice) return FALSE; /* Synthetic SendInput / keybd_event / mouse_event */

    for (int i = 0; i < g_devCacheCount; i++) {
        if (g_devCache[i].hDevice == hDevice) {
            return g_devCache[i].isPhysical;
        }
    }

    BOOL result = FALSE;
    WCHAR devName[512];
    UINT cb = 512;
    UINT len = GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, devName, &cb);
    if (len != (UINT)-1 && len > 0) {
        devName[511] = L'\0';
        for (WCHAR* p = devName; *p; ++p) {
            if (*p >= L'a' && *p <= L'z') *p = (WCHAR)(*p - L'a' + L'A');
        }
        if (wcsstr(devName, L"RDP_KBD") || wcsstr(devName, L"RDP_MOU") || wcsstr(devName, L"ROOT#RDP")) {
            result = FALSE;
        } else if (wcsstr(devName, L"\\\\?\\HID#") ||
                   wcsstr(devName, L"\\\\?\\ACPI#") ||
                   wcsstr(devName, L"\\\\?\\USB#") ||
                   wcsstr(devName, L"\\\\?\\BTHENUM#") ||
                   wcsstr(devName, L"\\\\?\\SERENUM#") ||
                   wcsstr(devName, L"\\\\?\\PCMCIA#")) {
            result = TRUE;
        }
    }

    if (g_devCacheCount < MAX_DEV_CACHE) {
        g_devCache[g_devCacheCount].hDevice = hDevice;
        g_devCache[g_devCacheCount].isPhysical = result;
        g_devCacheCount++;
    }
    return result;
}

static void ReleaseAndScrubSnapshots(void)
{
    size_t byteSize = (size_t)g_vw * (size_t)g_vh * 4;
    if (g_pBitsPristine && byteSize > 0) {
        SecureZeroMemory(g_pBitsPristine, byteSize);
    }
    if (g_pBitsDark && byteSize > 0) {
        SecureZeroMemory(g_pBitsDark, byteSize);
    }
    if (g_hdcPristine)  { DeleteDC(g_hdcPristine);     g_hdcPristine = NULL; }
    if (g_hbmPristine)  { DeleteObject(g_hbmPristine); g_hbmPristine = NULL; }
    g_pBitsPristine = NULL;
    if (g_hdcDarkened)  { DeleteDC(g_hdcDarkened);     g_hdcDarkened = NULL; }
    if (g_hbmDarkened)  { DeleteObject(g_hbmDarkened); g_hbmDarkened = NULL; }
    g_pBitsDark = NULL;
}

static void EnsureCurtainIntegrity(void)
{
    if (!g_hCurtain || g_bInternalUnmasking) return;

    RefreshVirtualScreenBounds();

    if (g_bHwDrmActive && (g_vw != g_scW || g_vh != g_scH)) {
        InitHwDrmOverlay(g_hCurtain);
    }

    RECT rc = { 0 };
    GetWindowRect(g_hCurtain, &rc);
    DWORD aff = 0;
    GetWindowDisplayAffinity(g_hCurtain, &aff);
    BOOL vis = IsWindowVisible(g_hCurtain);

    BOOL coveredByOther = FALSE;
    HWND prev = GetWindow(g_hCurtain, GW_HWNDPREV);
    int hops = 0;
    while (prev && hops++ < 32) {
        if (prev != g_hCanvasWnd && IsWindowVisible(prev)) {
            RECT prc = { 0 };
            if (GetWindowRect(prev, &prc) &&
                (prc.right - prc.left) > 1 &&
                (prc.bottom - prc.top) > 1) {
                coveredByOther = TRUE;
                break;
            }
        }
        prev = GetWindow(prev, GW_HWNDPREV);
    }

    if (!vis || aff != WDA_MONITOR || coveredByOther ||
        rc.left != g_vx || rc.top != g_vy ||
        (rc.right - rc.left) != g_vw || (rc.bottom - rc.top) != g_vh) {
        ShowWindow(g_hCurtain, SW_SHOWNOACTIVATE);
        if (!g_bHwDrmActive) {
            SetLayeredWindowAttributes(g_hCurtain, 0, 1, LWA_ALPHA);
        }
        SetWindowDisplayAffinity(g_hCurtain, WDA_MONITOR);
        SetWindowPos(g_hCurtain, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);

        if (g_hCanvasWnd && IsWindowVisible(g_hCanvasWnd)) {
            SetWindowDisplayAffinity(g_hCanvasWnd, WDA_MONITOR);
            SetWindowPos(g_hCanvasWnd, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh, SWP_SHOWWINDOW);
        }
    }
}

static void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
    (void)hHook; (void)dwEventThread; (void)dwmsEventTime; (void)idChild;
    if (g_bInternalUnmasking || !g_hCurtain) return;
    if (idObject != OBJID_WINDOW || !hwnd) return;
    if (hwnd == g_hCurtain || hwnd == g_hCanvasWnd) return;

    if (event == EVENT_SYSTEM_FOREGROUND ||
        (event == EVENT_OBJECT_SHOW && (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))) {
        EnsureCurtainIntegrity();
    }
}

static RECT GetNormalizedRect(POINT p1, POINT p2)
{
    RECT rc;
    rc.left   = std::min(p1.x, p2.x);
    rc.top    = std::min(p1.y, p2.y);
    rc.right  = std::max(p1.x, p2.x);
    rc.bottom = std::max(p1.y, p2.y);
    if (rc.left < 0) rc.left = 0;
    if (rc.top < 0) rc.top = 0;
    if (rc.right > g_vw) rc.right = g_vw;
    if (rc.bottom > g_vh) rc.bottom = g_vh;
    return rc;
}

static BOOL TakePrivilegedSnapshot(void)
{
    ReleaseAndScrubSnapshots();
    RefreshVirtualScreenBounds();

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return FALSE;

    g_hdcPristine = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = g_vw;
    bmi.bmiHeader.biHeight      = -g_vh;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_hbmPristine = CreateDIBSection(g_hdcPristine, &bmi, DIB_RGB_COLORS, &g_pBitsPristine, NULL, 0);
    if (!g_hbmPristine || !g_pBitsPristine) {
        ReleaseDC(NULL, hdcScreen);
        ReleaseAndScrubSnapshots();
        return FALSE;
    }
    SelectObject(g_hdcPristine, g_hbmPristine);

    /* Ultra-tight internal unmask window bounded strictly by DwmFlush() */
    g_bInternalUnmasking = TRUE;
    if (g_bHwDrmActive && g_pDCompVisual && g_pDCompDev) {
        g_pDCompVisual->SetContent(nullptr);
        g_pDCompDev->Commit();
    }
    ShowWindow(g_hCurtain, SW_HIDE);
    DwmFlush();
    DwmFlush();

    BitBlt(g_hdcPristine, 0, 0, g_vw, g_vh, hdcScreen, g_vx, g_vy, SRCCOPY | CAPTUREBLT);

    /* Immediately restore HW-DRM + WDA_MONITOR BEFORE building preview buffer */
    if (g_bHwDrmActive && g_pDCompVisual && g_pDCompDev && g_pSwapChain) {
        PresentHwDrmFrame();
        g_pDCompVisual->SetContent(g_pSwapChain);
        g_pDCompDev->Commit();
    }
    ShowWindow(g_hCurtain, SW_SHOWNOACTIVATE);
    if (!g_bHwDrmActive) {
        SetLayeredWindowAttributes(g_hCurtain, 0, 1, LWA_ALPHA);
    }
    SetWindowDisplayAffinity(g_hCurtain, WDA_MONITOR);
    SetWindowPos(g_hCurtain, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_bInternalUnmasking = FALSE;

    ReleaseDC(NULL, hdcScreen);

    g_hdcDarkened = CreateCompatibleDC(g_hdcPristine);
    g_hbmDarkened = CreateDIBSection(g_hdcDarkened, &bmi, DIB_RGB_COLORS, &g_pBitsDark, NULL, 0);
    if (!g_hbmDarkened || !g_pBitsDark) {
        ReleaseAndScrubSnapshots();
        return FALSE;
    }
    SelectObject(g_hdcDarkened, g_hbmDarkened);

    BYTE* src = (BYTE*)g_pBitsPristine;
    BYTE* dst = (BYTE*)g_pBitsDark;
    size_t total = (size_t)g_vw * (size_t)g_vh * 4;
    for (size_t i = 0; i < total; i += 4) {
        dst[i]     = (BYTE)((src[i]     * 2) / 5);
        dst[i + 1] = (BYTE)((src[i + 1] * 2) / 5);
        dst[i + 2] = (BYTE)((src[i + 2] * 2) / 5);
        dst[i + 3] = 255;
    }
    return TRUE;
}

static void CopySelectionToClipboard(RECT rc)
{
    if (!g_pBitsPristine) return;
    if (rc.left < 0) rc.left = 0;
    if (rc.top < 0) rc.top = 0;
    if (rc.right > g_vw) rc.right = g_vw;
    if (rc.bottom > g_vh) rc.bottom = g_vh;

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    size_t dibSize = sizeof(BITMAPINFOHEADER) + (size_t)w * (size_t)h * 4;
    HGLOBAL hGlobal = GlobalAlloc(GHND, dibSize);
    if (!hGlobal) return;

    BYTE* pMem = (BYTE*)GlobalLock(hGlobal);
    if (pMem) {
        BITMAPINFOHEADER* pHeader = (BITMAPINFOHEADER*)pMem;
        pHeader->biSize        = sizeof(BITMAPINFOHEADER);
        pHeader->biWidth       = w;
        pHeader->biHeight      = h;
        pHeader->biPlanes      = 1;
        pHeader->biBitCount    = 32;
        pHeader->biCompression = BI_RGB;
        pHeader->biSizeImage   = (DWORD)((size_t)w * (size_t)h * 4);

        BYTE* pDestBits = pMem + sizeof(BITMAPINFOHEADER);
        BYTE* pSrcBits  = (BYTE*)g_pBitsPristine;

        for (int row = 0; row < h; row++) {
            int srcRow     = rc.top + (h - 1 - row);
            size_t srcOff  = ((size_t)srcRow * (size_t)g_vw + (size_t)rc.left) * 4;
            size_t destOff = (size_t)row * (size_t)w * 4;
            memcpy(pDestBits + destOff, pSrcBits + srcOff, (size_t)w * 4);
        }

        GlobalUnlock(hGlobal);

        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_DIB, hGlobal)) {
                GlobalFree(hGlobal);
            }
            CloseClipboard();
            MessageBeep(MB_OK);
        } else {
            GlobalFree(hGlobal);
        }
    } else {
        GlobalFree(hGlobal);
    }
}

static void CloseCanvasAndScrub(void)
{
    if (g_hCanvasWnd) {
        ShowWindow(g_hCanvasWnd, SW_HIDE);
    }
    g_bSelecting = FALSE;
    g_bSelected  = FALSE;
    ReleaseAndScrubSnapshots();
}

static void FinalizeSelectionCapture(void)
{
    /* Strict anti-silent gate: visible canvas open >= 150ms required */
    if (!g_hCanvasWnd || !IsWindowVisible(g_hCanvasWnd) ||
        (GetTickCount64() - g_canvasOpenedTick) < 150) {
        return;
    }
    if (g_bSelected) {
        CopySelectionToClipboard(g_rcSelection);
    } else {
        RECT rcFull = { 0, 0, g_vw, g_vh };
        CopySelectionToClipboard(rcFull);
    }
    CloseCanvasAndScrub();
}

static void DrawMagnifier(HDC hdc, int mx, int my)
{
    if (!g_hdcPristine) return;

    int loupeSize  = 140;
    int zoomFactor = 4;
    int sampleGrid = loupeSize / zoomFactor;

    int lx = mx + 20;
    int ly = my + 20;
    if (lx + loupeSize + 20 > g_vw) lx = mx - loupeSize - 20;
    if (ly + loupeSize + 60 > g_vh) ly = my - loupeSize - 60;

    StretchBlt(hdc, lx, ly, loupeSize, loupeSize,
        g_hdcPristine,
        mx - sampleGrid / 2, my - sampleGrid / 2, sampleGrid, sampleGrid,
        SRCCOPY);

    HPEN hCrossPen = CreatePen(PS_SOLID, 1, RGB(0, 255, 128));
    HPEN hOldPen   = (HPEN)SelectObject(hdc, hCrossPen);
    int cx = lx + loupeSize / 2;
    int cy = ly + loupeSize / 2;
    MoveToEx(hdc, cx - 12, cy, NULL);
    LineTo(hdc, cx + 13, cy);
    MoveToEx(hdc, cx, cy - 12, NULL);
    LineTo(hdc, cx, cy + 13);

    HPEN hBorderPen   = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    SelectObject(hdc, hBorderPen);
    HBRUSH hNullBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, lx - 1, ly - 1, lx + loupeSize + 1, ly + loupeSize + 1);

    COLORREF pixelColor = GetPixel(g_hdcPristine, mx, my);
    BYTE r = GetRValue(pixelColor);
    BYTE g = GetGValue(pixelColor);
    BYTE b = GetBValue(pixelColor);

    RECT rcBadge = { lx, ly + loupeSize + 2, lx + loupeSize, ly + loupeSize + 46 };
    HBRUSH hBadgeBrush = CreateSolidBrush(RGB(20, 20, 24));
    FillRect(hdc, &rcBadge, hBadgeBrush);
    DeleteObject(hBadgeBrush);

    WCHAR szInfo1[64], szInfo2[64];
    wsprintfW(szInfo1, L"#%02X%02X%02X", r, g, b);
    wsprintfW(szInfo2, L"(%d, %d)", mx, my);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 255, 128));
    HFONT hFontBold = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFontBold);

    RECT rc1 = { lx + 4, ly + loupeSize + 3, lx + loupeSize - 4, ly + loupeSize + 22 };
    DrawTextW(hdc, szInfo1, -1, &rc1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(hdc, RGB(200, 200, 200));
    RECT rc2 = { lx + 4, ly + loupeSize + 23, lx + loupeSize - 4, ly + loupeSize + 43 };
    DrawTextW(hdc, szInfo2, -1, &rc2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, hOldFont);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hNullBrush);
    DeleteObject(hFontBold);
    DeleteObject(hCrossPen);
    DeleteObject(hBorderPen);
}

LRESULT CALLBACK CanvasWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_hdcDarkened && g_hdcPristine) {
            HDC hdcMem     = CreateCompatibleDC(hdc);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdc, g_vw, g_vh);
            HBITMAP hOldBm = (HBITMAP)SelectObject(hdcMem, hbmMem);

            BitBlt(hdcMem, 0, 0, g_vw, g_vh, g_hdcDarkened, 0, 0, SRCCOPY);

            if (g_bSelecting || g_bSelected) {
                RECT rc = g_rcSelection;
                int rw  = rc.right - rc.left;
                int rh  = rc.bottom - rc.top;

                if (rw > 0 && rh > 0) {
                    BitBlt(hdcMem, rc.left, rc.top, rw, rh, g_hdcPristine, rc.left, rc.top, SRCCOPY);

                    HPEN hPen         = CreatePen(PS_SOLID, 2, RGB(0, 255, 128));
                    HBRUSH hOldBrush  = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
                    HPEN hOldPen      = (HPEN)SelectObject(hdcMem, hPen);
                    Rectangle(hdcMem, rc.left, rc.top, rc.right, rc.bottom);

                    WCHAR szDim[64];
                    wsprintfW(szDim, L" %d \x00D7 %d ", rw, rh);
                    SetBkMode(hdcMem, OPAQUE);
                    SetBkColor(hdcMem, RGB(15, 15, 20));
                    SetTextColor(hdcMem, RGB(255, 255, 255));

                    HFONT hFont    = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

                    RECT rcPill = { rc.left, rc.top - 28, rc.left + 150, rc.top - 4 };
                    if (rcPill.top < 0) {
                        rcPill.top    = rc.bottom + 6;
                        rcPill.bottom = rc.bottom + 30;
                    }
                    DrawTextW(hdcMem, szDim, -1, &rcPill, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(hdcMem, hOldFont);
                    SelectObject(hdcMem, hOldPen);
                    SelectObject(hdcMem, hOldBrush);
                    DeleteObject(hFont);
                    DeleteObject(hPen);
                }
            }

            /* Prominent top-center anti-silent HUD banner */
            {
                int hudW = 960;
                if (hudW > g_vw - 40) hudW = g_vw - 40;
                int hudX = (g_vw - hudW) / 2;
                RECT rcHud = { hudX, 24, hudX + hudW, 68 };
                HBRUSH hHudBg = CreateSolidBrush(RGB(12, 16, 22));
                FillRect(hdcMem, &rcHud, hHudBg);
                DeleteObject(hHudBg);

                HPEN hHudPen = CreatePen(PS_SOLID, 2, RGB(0, 255, 128));
                HBRUSH hNullB = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
                HPEN hOldHudP = (HPEN)SelectObject(hdcMem, hHudPen);
                Rectangle(hdcMem, rcHud.left, rcHud.top, rcHud.right, rcHud.bottom);

                HFONT hHudFont = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT hOldHudF = (HFONT)SelectObject(hdcMem, hHudFont);
                SetBkMode(hdcMem, TRANSPARENT);
                SetTextColor(hdcMem, RGB(0, 255, 140));
                DrawTextW(hdcMem,
                    L" [AegisBlackout \u7269\u7406\u70ed\u952e\u7279\u6743\u622a\u5c4f\u5df2\u6fc0\u6d3b]  \u62d6\u62fd\u9009\u533a\u540e\u6309 Enter \u6216\u53cc\u51fb\u590d\u5236  |  Esc \u53d6\u6d88 ",
                    -1, &rcHud, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdcMem, hOldHudF);
                SelectObject(hdcMem, hOldHudP);
                SelectObject(hdcMem, hNullB);
                DeleteObject(hHudFont);
                DeleteObject(hHudPen);
            }

            DrawMagnifier(hdcMem, g_ptCurrent.x, g_ptCurrent.y);
            BitBlt(hdc, 0, 0, g_vw, g_vh, hdcMem, 0, 0, SRCCOPY);

            SelectObject(hdcMem, hOldBm);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
        g_bSelecting  = TRUE;
        g_bSelected   = FALSE;
        g_ptStart.x   = std::max<int>(0, std::min<int>(GET_X_LPARAM(lParam), g_vw));
        g_ptStart.y   = std::max<int>(0, std::min<int>(GET_Y_LPARAM(lParam), g_vh));
        g_ptCurrent   = g_ptStart;
        g_rcSelection = GetNormalizedRect(g_ptStart, g_ptCurrent);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_MOUSEMOVE:
        g_ptCurrent.x = std::max<int>(0, std::min<int>(GET_X_LPARAM(lParam), g_vw));
        g_ptCurrent.y = std::max<int>(0, std::min<int>(GET_Y_LPARAM(lParam), g_vh));
        if (g_bSelecting) {
            g_rcSelection = GetNormalizedRect(g_ptStart, g_ptCurrent);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONUP:
        if (g_bSelecting) {
            g_bSelecting  = FALSE;
            g_ptCurrent.x = std::max<int>(0, std::min<int>(GET_X_LPARAM(lParam), g_vw));
            g_ptCurrent.y = std::max<int>(0, std::min<int>(GET_Y_LPARAM(lParam), g_vh));
            g_rcSelection = GetNormalizedRect(g_ptStart, g_ptCurrent);
            int rw = g_rcSelection.right - g_rcSelection.left;
            int rh = g_rcSelection.bottom - g_rcSelection.top;
            if (rw > 5 && rh > 5) {
                g_bSelected = TRUE;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        if ((GetTickCount64() - g_lastPhysMouseDownTick) < 500) {
            FinalizeSelectionCapture();
        }
        return 0;

    case WM_KEYDOWN:
        /* Only verified physical WM_INPUT accepts VK_RETURN / VK_ESCAPE */
        (void)wParam;
        return 0;

    case WM_CLOSE:
        CloseCanvasAndScrub();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowSnippingCanvas(void)
{
    if (!TakePrivilegedSnapshot()) return;

    if (!g_hCanvasWnd) {
        WCHAR canvasCls[64];
        wsprintfW(canvasCls, L"ShellCompOverlay_%08X", RandU32());
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc   = CanvasWndProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursor(NULL, IDC_CROSS);
        wc.lpszClassName = canvasCls;
        wc.style         = CS_DBLCLKS;
        RegisterClassW(&wc);

        g_hCanvasWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            wc.lpszClassName,
            L"",
            WS_POPUP,
            g_vx, g_vy, g_vw, g_vh,
            NULL, NULL, g_hInst, NULL
        );
    } else {
        SetWindowPos(g_hCanvasWnd, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh, SWP_SHOWWINDOW);
    }

    if (g_hCanvasWnd) {
        SetWindowDisplayAffinity(g_hCanvasWnd, WDA_MONITOR);
        g_bSelecting = FALSE;
        g_bSelected  = FALSE;
        ZeroMemory(&g_rcSelection, sizeof(g_rcSelection));
        g_ptCurrent.x = g_vw / 2;
        g_ptCurrent.y = g_vh / 2;

        ShowWindow(g_hCanvasWnd, SW_SHOW);
        g_canvasOpenedTick = GetTickCount64();
        SetForegroundWindow(g_hCanvasWnd);
        SetWindowPos(g_hCanvasWnd, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh, SWP_SHOWWINDOW);
        InvalidateRect(g_hCanvasWnd, NULL, FALSE);
    }
}

static void CleanShutdown(UINT exitCode)
{
    if (g_hWinEventFg)   { UnhookWinEvent(g_hWinEventFg);   g_hWinEventFg = NULL; }
    if (g_hWinEventShow) { UnhookWinEvent(g_hWinEventShow); g_hWinEventShow = NULL; }
    if (g_hCurtain) {
        KillTimer(g_hCurtain, TIMER_WATCHDOG_ID);
        KillTimer(g_hCurtain, TIMER_HWDRM_HEARTBEAT);
    }
    ReleaseHwDrmOverlay();
    ReleaseAndScrubSnapshots();
    if (g_hCanvasWnd) { DestroyWindow(g_hCanvasWnd); g_hCanvasWnd = NULL; }
    if (g_hInputWnd)  { DestroyWindow(g_hInputWnd);  g_hInputWnd = NULL; }
    if (g_hCurtain) {
        g_bInternalUnmasking = TRUE;
        DestroyWindow(g_hCurtain);
        g_hCurtain = NULL;
    }
    PostQuitMessage((int)exitCode);
}

static void ProcessPhysicalRawInput(LPARAM lParam)
{
    UINT dwSize = 0;
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER)) != 0 || dwSize == 0) {
        return;
    }

    BYTE stackBuf[256];
    BYTE* buf = (dwSize <= sizeof(stackBuf)) ? stackBuf : (BYTE*)malloc(dwSize);
    if (!buf) return;

    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
        RAWINPUT* raw = (RAWINPUT*)buf;
        if (IsPhysicalHardwareDevice(raw->header.hDevice)) {
            if (raw->header.dwType == RIM_TYPEMOUSE) {
                if (raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
                    g_lastPhysMouseDownTick = GetTickCount64();
                }
            } else if (raw->header.dwType == RIM_TYPEKEYBOARD) {
                RAWKEYBOARD kb = raw->data.keyboard;
                if (kb.VKey != 0xFF) {
                    BOOL isBreak = (kb.Flags & RI_KEY_BREAK) != 0;
                    BOOL isE0    = (kb.Flags & RI_KEY_E0) != 0;

                    if (kb.VKey == VK_CONTROL) {
                        if (isE0) g_physRCtrl = !isBreak; else g_physLCtrl = !isBreak;
                    } else if (kb.VKey == VK_LCONTROL) {
                        g_physLCtrl = !isBreak;
                    } else if (kb.VKey == VK_RCONTROL) {
                        g_physRCtrl = !isBreak;
                    } else if (kb.VKey == VK_MENU) {
                        if (isE0) g_physRAlt = !isBreak; else g_physLAlt = !isBreak;
                    } else if (kb.VKey == VK_LMENU) {
                        g_physLAlt = !isBreak;
                    } else if (kb.VKey == VK_RMENU) {
                        g_physRAlt = !isBreak;
                    } else if (kb.VKey == VK_SHIFT) {
                        if (kb.MakeCode == 0x36) g_physRShift = !isBreak; else g_physLShift = !isBreak;
                    } else if (kb.VKey == VK_LSHIFT) {
                        g_physLShift = !isBreak;
                    } else if (kb.VKey == VK_RSHIFT) {
                        g_physRShift = !isBreak;
                    }

                    if (!isBreak) {
                        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) g_physLCtrl = g_physRCtrl = FALSE;
                        if ((GetAsyncKeyState(VK_MENU)    & 0x8000) == 0) g_physLAlt  = g_physRAlt  = FALSE;
                        if ((GetAsyncKeyState(VK_SHIFT)   & 0x8000) == 0) g_physLShift = g_physRShift = FALSE;

                        BOOL ctrlHeld  = (g_physLCtrl  || g_physRCtrl)  && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
                        BOOL altHeld   = (g_physLAlt   || g_physRAlt)   && ((GetAsyncKeyState(VK_MENU)    & 0x8000) != 0);
                        BOOL shiftHeld = (g_physLShift || g_physRShift) && ((GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0);

                        BOOL canvasOpen = (g_hCanvasWnd != NULL && IsWindowVisible(g_hCanvasWnd));

                        if (canvasOpen && kb.VKey == VK_RETURN) {
                            FinalizeSelectionCapture();
                        } else if (canvasOpen && kb.VKey == VK_ESCAPE) {
                            CloseCanvasAndScrub();
                        } else if (!canvasOpen && ctrlHeld && altHeld && !shiftHeld && kb.VKey == 'Q') {
                            ULONGLONG now = GetTickCount64();
                            if ((now - g_lastSnipTriggerTick) >= 350) {
                                g_lastSnipTriggerTick = now;
                                ShowSnippingCanvas();
                            }
                        } else if (ctrlHeld && altHeld && shiftHeld && kb.VKey == VK_END) {
                            CleanShutdown(0x5A5A);
                        }
                    }
                }
            }
        }
    }

    if (buf != stackBuf) free(buf);
}

LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INPUT) {
        ProcessPhysicalRawInput(lParam);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK CurtainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_CLOSE:
        return 0;

    case WM_SYSCOMMAND: {
        UINT cmd = (UINT)(wParam & 0xFFF0);
        if (cmd == SC_CLOSE || cmd == SC_MINIMIZE || cmd == SC_MOVE || cmd == SC_SIZE) {
            return 0;
        }
        break;
    }

    case WM_WINDOWPOSCHANGING:
        if (!g_bInternalUnmasking) {
            WINDOWPOS* wp = (WINDOWPOS*)lParam;
            wp->hwndInsertAfter = HWND_TOPMOST;
            wp->x  = g_vx;
            wp->y  = g_vy;
            wp->cx = g_vw;
            wp->cy = g_vh;
            wp->flags &= ~(SWP_HIDEWINDOW | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE);
            wp->flags |= (SWP_SHOWWINDOW | SWP_NOACTIVATE);
            return 0;
        }
        break;

    case WM_SHOWWINDOW:
        if (wParam == FALSE && !g_bInternalUnmasking) {
            PostMessageW(hwnd, WM_APP_RESTORE_CURTAIN, 0, 0);
        }
        break;

    case WM_APP_RESTORE_CURTAIN:
        EnsureCurtainIntegrity();
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        EnsureCurtainIntegrity();
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_WATCHDOG_ID) {
            EnsureCurtainIntegrity();
        } else if (wParam == TIMER_HWDRM_HEARTBEAT) {
            if (g_bHwDrmActive && !g_bInternalUnmasking) {
                PresentHwDrmFrame();
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    (void)hPrev; (void)lpCmd; (void)nShow;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_hInst = hInst;

    StartupLog("AegisBlackout v3.0 startup begin (DPI Aware v2, 24x7 HwDRM + Zero-Silent, Ctrl+Alt+Q)", TRUE);

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\DwmCompHelper_Mtx_a9e1");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        StartupLog("Mutex exists, exiting", FALSE);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    HDESK hDesk = OpenDesktopW(L"Default", 0, FALSE, 0x01FF);
    if (hDesk) {
        SetThreadDesktop(hDesk);
    }

    HardenProcessSecurity();
    RefreshVirtualScreenBounds();

    /* 1. Stealth fullscreen topmost Dual-Lock curtain (GPU HW_PROTECTED|DISPLAY_ONLY + WDA_MONITOR) */
    WCHAR curtainCls[64];
    wsprintfW(curtainCls, L"DwmSurfaceHost_%08X", RandU32());
    WNDCLASSW wcCurtain = { 0 };
    wcCurtain.lpfnWndProc   = CurtainWndProc;
    wcCurtain.hInstance     = hInst;
    wcCurtain.lpszClassName = curtainCls;
    RegisterClassW(&wcCurtain);

    g_hCurtain = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        wcCurtain.lpszClassName,
        L"",
        WS_POPUP | WS_VISIBLE,
        g_vx, g_vy, g_vw, g_vh,
        NULL, NULL, hInst, NULL
    );
    if (!g_hCurtain) {
        StartupLog("Failed to create HwDRM curtain window", FALSE);
        CloseHandle(hMutex);
        if (hDesk) CloseDesktop(hDesk);
        return 1;
    }

    g_bHwDrmActive = InitHwDrmOverlay(g_hCurtain);
    if (!g_bHwDrmActive) {
        g_bInternalUnmasking = TRUE;
        DestroyWindow(g_hCurtain);
        g_bInternalUnmasking = FALSE;
        g_hCurtain = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            wcCurtain.lpszClassName,
            L"",
            WS_POPUP | WS_VISIBLE,
            g_vx, g_vy, g_vw, g_vh,
            NULL, NULL, hInst, NULL
        );
        if (!g_hCurtain) {
            StartupLog("Fallback curtain window creation failed", FALSE);
            CloseHandle(hMutex);
            if (hDesk) CloseDesktop(hDesk);
            return 1;
        }
        SetLayeredWindowAttributes(g_hCurtain, 0, 1, LWA_ALPHA);
    }

    BOOL affOk = SetWindowDisplayAffinity(g_hCurtain, WDA_MONITOR);
    UpdateWindow(g_hCurtain);

    char hwLog[192];
    sprintf_s(hwLog, sizeof(hwLog),
        "HwDRM: active=%d SwapChainFlags=0x%X BackBufMisc=0x%X (HW_PROTECTED=%d) DCompCommit=0x%08X",
        g_bHwDrmActive, g_actualScFlags, g_actualBbMisc,
        (g_actualBbMisc & D3D11_RESOURCE_MISC_HW_PROTECTED) ? 1 : 0, (unsigned)g_lastCommitHr);
    StartupLog(hwLog, FALSE);

    /* 2. Zero-hotkey Raw Input sink */
    WCHAR inputCls[64];
    wsprintfW(inputCls, L"InputSinkHost_%08X", RandU32());
    WNDCLASSW wcInput = { 0 };
    wcInput.lpfnWndProc   = InputWndProc;
    wcInput.hInstance     = hInst;
    wcInput.lpszClassName = inputCls;
    RegisterClassW(&wcInput);

    g_hInputWnd = CreateWindowExW(0, wcInput.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    RAWINPUTDEVICE rids[2];
    rids[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    rids[0].usUsage     = HID_USAGE_GENERIC_KEYBOARD;
    rids[0].dwFlags     = RIDEV_INPUTSINK;
    rids[0].hwndTarget  = g_hInputWnd;

    rids[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    rids[1].usUsage     = HID_USAGE_GENERIC_MOUSE;
    rids[1].dwFlags     = RIDEV_INPUTSINK;
    rids[1].hwndTarget  = g_hInputWnd;

    BOOL rawOk = RegisterRawInputDevices(rids, 2, sizeof(RAWINPUTDEVICE));
    if (!rawOk) {
        if (g_hInputWnd) DestroyWindow(g_hInputWnd);
        g_hInputWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            wcInput.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
        rids[0].hwndTarget = g_hInputWnd;
        rids[1].hwndTarget = g_hInputWnd;
        rawOk = RegisterRawInputDevices(rids, 2, sizeof(RAWINPUTDEVICE));
    }

    /* 3. Active Topmost Watchdog (100ms) + 24x7 HW-DRM Present Heartbeat (1000ms) */
    SetTimer(g_hCurtain, TIMER_WATCHDOG_ID, 100, NULL);
    if (g_bHwDrmActive) {
        SetTimer(g_hCurtain, TIMER_HWDRM_HEARTBEAT, 1000, NULL);
    }
    g_hWinEventFg = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
    );
    g_hWinEventShow = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
    );

    char summary[160];
    sprintf_s(summary, sizeof(summary),
        "Ready: WDA=%d RawInput=%d WinEvent=(%p,%p) Bounds=(%d,%d,%dx%d)",
        affOk, rawOk, (void*)g_hWinEventFg, (void*)g_hWinEventShow, g_vx, g_vy, g_vw, g_vh);
    StartupLog(summary, FALSE);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ReleaseHwDrmOverlay();
    ReleaseAndScrubSnapshots();
    CloseHandle(hMutex);
    if (hDesk) CloseDesktop(hDesk);
    return (int)msg.wParam;
}
