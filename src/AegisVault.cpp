#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <wincodec.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <d3d12.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#ifndef D3D11_RESOURCE_MISC_HW_PROTECTED
#define D3D11_RESOURCE_MISC_HW_PROTECTED 0x80000
#endif

#ifndef DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED
#define DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED 0x400
#endif

#ifndef DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY
#define DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY 0x20
#endif

#define MAX_PROTECTED_DIM 4096

static const wchar_t* kVaultClass = L"AegisVaultProtWnd";

template <typename T>
static void SafeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

static void SecureZeroVec(std::vector<BYTE>& v)
{
    if (!v.empty()) {
        SecureZeroMemory(v.data(), v.size());
        v.clear();
        v.shrink_to_fit();
    }
}

static void SecureZeroWStr(std::wstring& s)
{
    if (!s.empty()) {
        SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
        s.clear();
        s.shrink_to_fit();
    }
}

static const char* kVsSrc =
"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
"VSOut main(uint id : SV_VertexID) {\n"
"    VSOut o;\n"
"    float2 uv = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
"    o.uv = uv;\n"
"    return o;\n"
"}\n";

static const char* kPsSrc =
"Texture2D<float4> g_tex : register(t0);\n"
"SamplerState g_samp : register(s0);\n"
"cbuffer Params : register(b0) { float4 uvRect; };\n"
"float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
"    float2 tuv = uvRect.xy + uv * uvRect.zw;\n"
"    if (tuv.x < 0.0f || tuv.x > 1.0f || tuv.y < 0.0f || tuv.y > 1.0f)\n"
"        return float4(0.067f, 0.075f, 0.098f, 1.0f);\n"
"    return float4(g_tex.Sample(g_samp, tuv).rgb, 1.0f);\n"
"}\n";

struct CbParams {
    float u0;
    float v0;
    float du;
    float dv;
};

struct VaultState {
    HWND hwnd = nullptr;
    UINT winW = 1200;
    UINT winH = 800;

    ID3D11Device*             dev = nullptr;
    ID3D11DeviceContext*      ctx = nullptr;
    IDXGISwapChain1*          sc = nullptr;
    ID3D11RenderTargetView*   bbRtv = nullptr;
    ID3D11VertexShader*       vs = nullptr;
    ID3D11PixelShader*        ps = nullptr;
    ID3D11SamplerState*       samp = nullptr;
    ID3D11Buffer*             cb = nullptr;

    ID3D11Texture2D*          protTex = nullptr;
    ID3D11RenderTargetView*   protRtv = nullptr;
    ID3D11ShaderResourceView* protSrv = nullptr;

    UINT scFlags = 0;
    UINT bbMisc = 0;
    UINT protTexMisc = 0;
    DWORD wdaActual = 0;

    UINT contentW = 0;
    UINT contentH = 0;
    int  scrollY = 0;
    bool isImage = false;
    bool cpuScrubbed = false;
};

static VaultState g_v;

static void HardenVaultMitigations(void)
{
    typedef BOOL(WINAPI* PFN_SetMitigation)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (!hK32) return;
    PFN_SetMitigation pSet = (PFN_SetMitigation)GetProcAddress(hK32, "SetProcessMitigationPolicy");
    if (!pSet) return;

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ep = { 0 };
    ep.DisableExtensionPoints = 1;
    pSet(ProcessExtensionPointDisablePolicy, &ep, sizeof(ep));

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY il = { 0 };
    il.NoRemoteImages = 1;
    il.NoLowMandatoryLabelImages = 1;
    pSet(ProcessImageLoadPolicy, &il, sizeof(il));
}

static void UpdateVaultTitle(void)
{
    if (!g_v.hwnd) return;
    WCHAR title[256] = L"";
    _snwprintf_s(title, _TRUNCATE,
        L"AegisVault | SwapChain=0x%X (HW_PROTECTED=%d) | VRAM_Misc=0x%X | WDA=0x%X | CPU明文=0B(已擦除) | 滚动=%d",
        g_v.scFlags,
        (g_v.scFlags & DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED) ? 1 : 0,
        g_v.protTexMisc,
        g_v.wdaActual,
        g_v.scrollY);
    SetWindowTextW(g_v.hwnd, title);
}

static bool InitD3D11Vault(HWND hwnd, UINT w, UINT h)
{
    g_v.hwnd = hwnd;
    g_v.winW = w > 0 ? w : 800;
    g_v.winH = h > 0 ? h : 600;

    IDXGIFactory2* fac = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    IDXGIAdapter1* pAdapter = nullptr;
    if (fac) {
        IDXGIFactory1* fac1 = nullptr;
        if (SUCCEEDED(fac->QueryInterface(IID_PPV_ARGS(&fac1))) && fac1) {
            for (UINT i = 0; ; i++) {
                IDXGIAdapter1* cand = nullptr;
                if (fac1->EnumAdapters1(i, &cand) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 d = {};
                cand->GetDesc1(&d);
                if (d.VendorId == 0x1002) {
                    pAdapter = cand;
                    break;
                }
                cand->Release();
            }
            fac1->Release();
        }
    }

    D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        pAdapter,
        pAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &g_v.dev, &flOut, &g_v.ctx);
    SafeRelease(pAdapter);
    if (FAILED(hr) || !g_v.dev || !g_v.ctx) {
        SafeRelease(fac);
        return false;
    }

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* ad = nullptr;
    if (!fac && SUCCEEDED(g_v.dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) && dxgiDev) {
        if (SUCCEEDED(dxgiDev->GetAdapter(&ad)) && ad) {
            ad->GetParent(IID_PPV_ARGS(&fac));
        }
    }
    SafeRelease(ad);
    SafeRelease(dxgiDev);
    if (!fac) return false;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = g_v.winW;
    sd.Height = g_v.winH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    const UINT candidates[] = {
        DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED | DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY,
        DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED,
        DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY,
        0
    };
    for (UINT f : candidates) {
        sd.Flags = f;
        hr = fac->CreateSwapChainForHwnd(g_v.dev, hwnd, &sd, nullptr, nullptr, &g_v.sc);
        if (SUCCEEDED(hr) && g_v.sc) break;
    }
    SafeRelease(fac);
    if (!g_v.sc) return false;

    DXGI_SWAP_CHAIN_DESC1 actual = {};
    g_v.sc->GetDesc1(&actual);
    g_v.scFlags = actual.Flags;

    ID3D11Texture2D* pBB = nullptr;
    if (SUCCEEDED(g_v.sc->GetBuffer(0, IID_PPV_ARGS(&pBB))) && pBB) {
        D3D11_TEXTURE2D_DESC bbd = {};
        pBB->GetDesc(&bbd);
        g_v.bbMisc = bbd.MiscFlags;
        hr = g_v.dev->CreateRenderTargetView(pBB, nullptr, &g_v.bbRtv);
        pBB->Release();
    }
    if (FAILED(hr) || !g_v.bbRtv) return false;

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (FAILED(D3DCompile(kVsSrc, strlen(kVsSrc), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, nullptr)))
        return false;
    if (FAILED(D3DCompile(kPsSrc, strlen(kPsSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, nullptr))) {
        SafeRelease(vsBlob);
        return false;
    }
    HRESULT hrVS = g_v.dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_v.vs);
    HRESULT hrPS = g_v.dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_v.ps);
    SafeRelease(vsBlob);
    SafeRelease(psBlob);
    if (FAILED(hrVS) || FAILED(hrPS)) return false;

    D3D11_SAMPLER_DESC sampD = {};
    sampD.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampD.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampD.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampD.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(g_v.dev->CreateSamplerState(&sampD, &g_v.samp))) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(CbParams);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_v.dev->CreateBuffer(&cbd, nullptr, &g_v.cb))) return false;

    return true;
}

static void DrawQuadWithSrv(ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv,
    UINT viewW, UINT viewH, float u0, float v0, float du, float dv)
{
    if (!g_v.ctx || !rtv || !g_v.vs || !g_v.ps) return;

    CbParams p = { u0, v0, du, dv };
    g_v.ctx->UpdateSubresource(g_v.cb, 0, nullptr, &p, 0, 0);

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)viewW, (float)viewH, 0.0f, 1.0f };
    g_v.ctx->RSSetViewports(1, &vp);
    g_v.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_v.ctx->IASetInputLayout(nullptr);
    g_v.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_v.ctx->VSSetShader(g_v.vs, nullptr, 0);
    g_v.ctx->PSSetShader(g_v.ps, nullptr, 0);
    g_v.ctx->PSSetConstantBuffers(0, 1, &g_v.cb);
    g_v.ctx->PSSetSamplers(0, 1, &g_v.samp);
    g_v.ctx->PSSetShaderResources(0, 1, &srv);
    g_v.ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_v.ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    g_v.ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
}

static void RenderFrame(void)
{
    if (!g_v.ctx || !g_v.bbRtv || !g_v.sc) return;

    const float bg[4] = { 0.067f, 0.075f, 0.098f, 1.0f };
    g_v.ctx->ClearRenderTargetView(g_v.bbRtv, bg);

    if (g_v.protSrv && g_v.contentW > 0 && g_v.contentH > 0) {
        if (!g_v.isImage) {
            int maxScroll = (int)g_v.contentH - (int)g_v.winH;
            if (maxScroll < 0) maxScroll = 0;
            if (g_v.scrollY < 0) g_v.scrollY = 0;
            if (g_v.scrollY > maxScroll) g_v.scrollY = maxScroll;

            float du = (float)g_v.winW / (float)g_v.contentW;
            if (du > 1.0f) du = 1.0f;
            float dv = (float)g_v.winH / (float)g_v.contentH;
            float v0 = (float)g_v.scrollY / (float)g_v.contentH;
            DrawQuadWithSrv(g_v.protSrv, g_v.bbRtv, g_v.winW, g_v.winH, 0.0f, v0, du, dv);
        } else {
            float sx = (float)g_v.winW / (float)g_v.contentW;
            float sy = (float)g_v.winH / (float)g_v.contentH;
            float scale = (sx < sy) ? sx : sy;
            if (scale <= 0.0f) scale = 1.0f;
            float drawW = (float)g_v.contentW * scale;
            float drawH = (float)g_v.contentH * scale;
            float offX = ((float)g_v.winW - drawW) * 0.5f;
            float offY = ((float)g_v.winH - drawH) * 0.5f;
            float u0 = -offX / drawW;
            float v0 = -offY / drawH;
            float du = (float)g_v.winW / drawW;
            float dv = (float)g_v.winH / drawH;
            DrawQuadWithSrv(g_v.protSrv, g_v.bbRtv, g_v.winW, g_v.winH, u0, v0, du, dv);
        }
    }

    g_v.sc->Present(1, 0);
}

static bool UploadPixelsToProtectedVram(const BYTE* bgra, UINT w, UINT h, bool isImage)
{
    if (!g_v.dev || !g_v.ctx || !bgra || w == 0 || h == 0) return false;
    if (w > MAX_PROTECTED_DIM) w = MAX_PROTECTED_DIM;
    if (h > MAX_PROTECTED_DIM) h = MAX_PROTECTED_DIM;

    D3D11_TEXTURE2D_DESC tdTmp = {};
    tdTmp.Width = w;
    tdTmp.Height = h;
    tdTmp.MipLevels = 1;
    tdTmp.ArraySize = 1;
    tdTmp.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tdTmp.SampleDesc.Count = 1;
    tdTmp.Usage = D3D11_USAGE_DEFAULT;
    tdTmp.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = { bgra, w * 4, 0 };
    ID3D11Texture2D* pTmpTex = nullptr;
    HRESULT hrTmp = g_v.dev->CreateTexture2D(&tdTmp, &init, &pTmpTex);
    ID3D11ShaderResourceView* pTmpSrv = nullptr;
    if (SUCCEEDED(hrTmp) && pTmpTex) {
        hrTmp = g_v.dev->CreateShaderResourceView(pTmpTex, nullptr, &pTmpSrv);
    }
    if (FAILED(hrTmp) || !pTmpSrv) {
        SafeRelease(pTmpSrv);
        SafeRelease(pTmpTex);
        return false;
    }

    SafeRelease(g_v.protSrv);
    SafeRelease(g_v.protRtv);
    SafeRelease(g_v.protTex);

    D3D11_TEXTURE2D_DESC tdProt = tdTmp;
    tdProt.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    tdProt.MiscFlags = D3D11_RESOURCE_MISC_HW_PROTECTED;
    HRESULT hrProt = g_v.dev->CreateTexture2D(&tdProt, nullptr, &g_v.protTex);
    if (FAILED(hrProt) || !g_v.protTex) {
        tdProt.MiscFlags = 0;
        hrProt = g_v.dev->CreateTexture2D(&tdProt, nullptr, &g_v.protTex);
    }

    if (SUCCEEDED(hrProt) && g_v.protTex) {
        D3D11_TEXTURE2D_DESC actual = {};
        g_v.protTex->GetDesc(&actual);
        g_v.protTexMisc = actual.MiscFlags;
        hrProt = g_v.dev->CreateRenderTargetView(g_v.protTex, nullptr, &g_v.protRtv);
        if (SUCCEEDED(hrProt)) {
            hrProt = g_v.dev->CreateShaderResourceView(g_v.protTex, nullptr, &g_v.protSrv);
        }
    }

    if (SUCCEEDED(hrProt) && g_v.protRtv && g_v.protSrv) {
        DrawQuadWithSrv(pTmpSrv, g_v.protRtv, w, h, 0.0f, 0.0f, 1.0f, 1.0f);
        g_v.ctx->Flush();
        g_v.contentW = w;
        g_v.contentH = h;
        g_v.scrollY = 0;
        g_v.isImage = isImage;
    }

    // Immediate teardown of temporary unprotected GPU staging surface
    SafeRelease(pTmpSrv);
    SafeRelease(pTmpTex);
    return SUCCEEDED(hrProt);
}

static void WipeProtectedVram(void)
{
    if (g_v.ctx && g_v.protRtv) {
        const float blk[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_v.ctx->ClearRenderTargetView(g_v.protRtv, blk);
        g_v.ctx->Flush();
    }
    SafeRelease(g_v.protSrv);
    SafeRelease(g_v.protRtv);
    SafeRelease(g_v.protTex);
    g_v.contentW = 0;
    g_v.contentH = 0;
    g_v.scrollY = 0;
    g_v.protTexMisc = 0;

    if (g_v.ctx && g_v.bbRtv && g_v.sc) {
        const float blk[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        for (int i = 0; i < 2; i++) {
            g_v.ctx->ClearRenderTargetView(g_v.bbRtv, blk);
            g_v.sc->Present(0, 0);
        }
        g_v.ctx->Flush();
    }
}

static bool RenderTextToProtectedVram(std::wstring& text, const wchar_t* bannerLabel)
{
    UINT canvasW = (g_v.winW > 900) ? g_v.winW : 1152;
    if (canvasW > MAX_PROTECTED_DIM) canvasW = MAX_PROTECTED_DIM;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HFONT hFont = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Microsoft YaHei UI");
    HFONT hBannerFont = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");

    HGDIOBJ oldFont = SelectObject(hdcMem, hFont);
    RECT calcRc = { 28, 66, (LONG)canvasW - 28, MAX_PROTECTED_DIM };
    DrawTextW(hdcMem, text.c_str(), (int)text.size(), &calcRc,
        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_CALCRECT);

    UINT canvasH = (UINT)(calcRc.bottom + 48);
    if (canvasH < g_v.winH) canvasH = g_v.winH;
    if (canvasH < 600) canvasH = 600;
    if (canvasH > MAX_PROTECTED_DIM) canvasH = MAX_PROTECTED_DIM;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)canvasW;
    bmi.bmiHeader.biHeight = -(LONG)canvasH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (hbm && bits) {
        HGDIOBJ oldBm = SelectObject(hdcMem, hbm);

        RECT fullRc = { 0, 0, (LONG)canvasW, (LONG)canvasH };
        HBRUSH brBg = CreateSolidBrush(RGB(18, 20, 26));
        FillRect(hdcMem, &fullRc, brBg);
        DeleteObject(brBg);

        RECT hdrRc = { 0, 0, (LONG)canvasW, 46 };
        HBRUSH brHdr = CreateSolidBrush(RGB(27, 32, 44));
        FillRect(hdcMem, &hdrRc, brHdr);
        DeleteObject(brHdr);

        RECT lineRc = { 0, 46, (LONG)canvasW, 47 };
        HBRUSH brLine = CreateSolidBrush(RGB(52, 66, 92));
        FillRect(hdcMem, &lineRc, brLine);
        DeleteObject(brLine);

        SetBkMode(hdcMem, TRANSPARENT);
        SelectObject(hdcMem, hBannerFont);
        SetTextColor(hdcMem, RGB(125, 211, 252));
        RECT hdrTxt = { 24, 12, (LONG)canvasW - 24, 40 };
        DrawTextW(hdcMem, bannerLabel, -1, &hdrTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(hdcMem, hFont);
        SetTextColor(hdcMem, RGB(230, 237, 243));
        RECT bodyRc = { 28, 66, (LONG)canvasW - 28, (LONG)canvasH - 20 };
        DrawTextW(hdcMem, text.c_str(), (int)text.size(), &bodyRc,
            DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);

        GdiFlush();
        ok = UploadPixelsToProtectedVram((const BYTE*)bits, canvasW, canvasH, false);

        // Immediately zero temporary CPU DIB pixels
        SecureZeroMemory(bits, (size_t)canvasW * (size_t)canvasH * 4);
        SelectObject(hdcMem, oldBm);
        DeleteObject(hbm);
    }

    SecureZeroWStr(text);
    g_v.cpuScrubbed = ok;

    SelectObject(hdcMem, oldFont);
    DeleteObject(hFont);
    DeleteObject(hBannerFont);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    UpdateVaultTitle();
    RenderFrame();
    return ok;
}

static void LoadWelcomeScreen(void)
{
    WCHAR msg[1024] = L"";
    _snwprintf_s(msg, _TRUNCATE,
        L"====================================================================\r\n"
        L"  AegisVault — AMD RX 9070 硬件保护显存绝密查看器\r\n"
        L"====================================================================\r\n\r\n"
        L"  [当前硬件安全链路]\r\n"
        L"   * 交换链标志      : 0x%X (HW_PROTECTED=%d, DISPLAY_ONLY=%d)\r\n"
        L"   * 保护显存资源    : MiscFlags = 0x%X (渲染进入芯片安全显存区)\r\n"
        L"   * 窗口防捕获锁    : WDA_EXCLUDEFROMCAPTURE (0x%X)\r\n"
        L"   * CPU 明文驻留    : 0 字节 (上传显存后立即 SecureZeroMemory 擦除)\r\n\r\n"
        L"  [使用方式]\r\n"
        L"   * 直接把 .txt / .json / .key / .png / .jpg 文件拖进此窗口\r\n"
        L"   * 按 [ Ctrl + O ]       : 打开文本或图片\r\n"
        L"   * 按 [ Ctrl + V ]       : 导入剪贴板文字/截图到保护显存，并立刻清空剪贴板\r\n"
        L"   * 鼠标滚轮 / 方向键     : 在保护显存内上下滚动浏览\r\n"
        L"   * 按 [ Ctrl + Shift + C]: 立即黑屏擦除显存内容\r\n"
        L"   * 按 [ Esc ]            : 安全抹除保护显存并退出\r\n",
        g_v.scFlags,
        (g_v.scFlags & DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED) ? 1 : 0,
        (g_v.scFlags & DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY) ? 1 : 0,
        g_v.bbMisc,
        g_v.wdaActual);
    std::wstring s(msg);
    RenderTextToProtectedVram(s,
        L"AegisVault 就绪 | 拖入文件 / Ctrl+O / Ctrl+V 导入（明文不留在 CPU 内存与剪贴板）");
}

static std::wstring DecodeTextBytesAndScrub(std::vector<BYTE>& raw)
{
    std::wstring out;
    if (raw.empty()) return out;

    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        size_t chars = (raw.size() - 2) / 2;
        out.assign((const wchar_t*)(raw.data() + 2), chars);
        SecureZeroVec(raw);
        return out;
    }

    const BYTE* p = raw.data();
    size_t len = raw.size();
    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        p += 3; len -= 3;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)p, (int)len, nullptr, 0);
    UINT cp = CP_UTF8;
    if (wlen <= 0) {
        cp = 936;
        wlen = MultiByteToWideChar(936, 0, (const char*)p, (int)len, nullptr, 0);
    }
    if (wlen > 0) {
        out.resize((size_t)wlen);
        MultiByteToWideChar(cp, 0, (const char*)p, (int)len, &out[0], wlen);
    }
    SecureZeroVec(raw);
    return out;
}

static bool IsImageExt(const wchar_t* path)
{
    const wchar_t* dot = wcsrchr(path, L'.');
    if (!dot) return false;
    static const wchar_t* exts[] = { L".png", L".jpg", L".jpeg", L".bmp", L".webp", L".gif", L".tif", L".tiff", L".ico" };
    for (const wchar_t* e : exts) {
        if (_wcsicmp(dot, e) == 0) return true;
    }
    return false;
}

static bool LoadFileIntoVault(const wchar_t* path)
{
    if (!path || !*path) return false;

    if (IsImageExt(path)) {
        IWICImagingFactory* wic = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapSource* src = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* conv = nullptr;

        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (SUCCEEDED(hr) && wic) hr = wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
        if (SUCCEEDED(hr) && dec) hr = dec->GetFrame(0, &frame);

        UINT w = 0, h = 0;
        if (SUCCEEDED(hr) && frame) hr = frame->GetSize(&w, &h);
        if (SUCCEEDED(hr) && w > 0 && h > 0) {
            src = frame;
            src->AddRef();
            if (w > MAX_PROTECTED_DIM || h > MAX_PROTECTED_DIM) {
                double s = std::min((double)MAX_PROTECTED_DIM / w, (double)MAX_PROTECTED_DIM / h);
                UINT nw = std::max(1u, (UINT)(w * s));
                UINT nh = std::max(1u, (UINT)(h * s));
                if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                    SUCCEEDED(scaler->Initialize(src, nw, nh, WICBitmapInterpolationModeFant))) {
                    src->Release();
                    src = scaler;
                    src->AddRef();
                    w = nw; h = nh;
                }
            }
            if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                std::vector<BYTE> px((size_t)w * (size_t)h * 4, 0);
                if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) {
                    bool ok = UploadPixelsToProtectedVram(px.data(), w, h, true);
                    SecureZeroVec(px);
                    g_v.cpuScrubbed = ok;
                    UpdateVaultTitle();
                    RenderFrame();
                    SafeRelease(conv); SafeRelease(scaler); SafeRelease(src);
                    SafeRelease(frame); SafeRelease(dec); SafeRelease(wic);
                    return ok;
                }
            }
        }
        SafeRelease(conv); SafeRelease(scaler); SafeRelease(src);
        SafeRelease(frame); SafeRelease(dec); SafeRelease(wic);
        return false;
    }

    HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(hf, &sz);
    DWORD toRead = (sz.QuadPart > 1024 * 1024) ? (1024 * 1024) : (DWORD)sz.QuadPart;
    std::vector<BYTE> raw(toRead, 0);
    DWORD rd = 0;
    if (toRead > 0) ReadFile(hf, raw.data(), toRead, &rd, nullptr);
    CloseHandle(hf);
    raw.resize(rd);

    std::wstring text = DecodeTextBytesAndScrub(raw);
    if (text.empty()) text = L"(空文件)";
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? (name + 1) : path;
    WCHAR hdr[260] = L"";
    _snwprintf_s(hdr, _TRUNCATE, L"[保护显存查看] %ls | CPU 明文已立即清零", name);
    return RenderTextToProtectedVram(text, hdr);
}

static void ImportFromClipboardAndClear(HWND hwnd)
{
    if (!OpenClipboard(hwnd)) return;

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            const wchar_t* p = (const wchar_t*)GlobalLock(hData);
            if (p) {
                std::wstring text(p);
                GlobalUnlock(hData);
                EmptyClipboard();
                CloseClipboard();
                RenderTextToProtectedVram(text, L"[剪贴板文本已转入保护显存 | 原剪贴板与 CPU 明文均已销毁]");
                return;
            }
        }
    } else if (IsClipboardFormatAvailable(CF_BITMAP)) {
        HBITMAP hBm = (HBITMAP)GetClipboardData(CF_BITMAP);
        BITMAP bm = {};
        if (hBm && GetObjectW(hBm, sizeof(bm), &bm) > 0 && bm.bmWidth > 0 && bm.bmHeight > 0) {
            UINT w = (UINT)std::min<LONG>(bm.bmWidth, MAX_PROTECTED_DIM);
            UINT h = (UINT)std::min<LONG>(bm.bmHeight, MAX_PROTECTED_DIM);
            HDC hdc = GetDC(nullptr);
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = (LONG)w;
            bmi.bmiHeader.biHeight = -(LONG)h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            std::vector<BYTE> px((size_t)w * (size_t)h * 4, 0);
            int got = GetDIBits(hdc, hBm, 0, h, px.data(), &bmi, DIB_RGB_COLORS);
            ReleaseDC(nullptr, hdc);
            EmptyClipboard();
            CloseClipboard();
            if (got > 0) {
                bool ok = UploadPixelsToProtectedVram(px.data(), w, h, true);
                SecureZeroVec(px);
                g_v.cpuScrubbed = ok;
                UpdateVaultTitle();
                RenderFrame();
            }
            return;
        }
    }

    CloseClipboard();
}

static void OpenFileViaDialog(HWND hwnd)
{
    WCHAR fileBuf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"绝密文档与图片 (*.txt;*.md;*.json;*.key;*.pem;*.png;*.jpg;*.bmp)\0*.txt;*.md;*.json;*.env;*.key;*.pem;*.ini;*.log;*.csv;*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.gif\0所有文件 (*.*)\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        LoadFileIntoVault(fileBuf);
    }
}

static LRESULT CALLBACK VaultWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        if (g_v.sc && g_v.dev && g_v.ctx && wParam != SIZE_MINIMIZED) {
            UINT nw = LOWORD(lParam), nh = HIWORD(lParam);
            if (nw > 0 && nh > 0) {
                g_v.winW = nw;
                g_v.winH = nh;
                SafeRelease(g_v.bbRtv);
                if (SUCCEEDED(g_v.sc->ResizeBuffers(0, nw, nh, DXGI_FORMAT_UNKNOWN, g_v.scFlags))) {
                    ID3D11Texture2D* pBB = nullptr;
                    if (SUCCEEDED(g_v.sc->GetBuffer(0, IID_PPV_ARGS(&pBB))) && pBB) {
                        g_v.dev->CreateRenderTargetView(pBB, nullptr, &g_v.bbRtv);
                        pBB->Release();
                    }
                    RenderFrame();
                    UpdateVaultTitle();
                }
            }
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        RenderFrame();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        WCHAR filePath[MAX_PATH] = L"";
        if (DragQueryFileW(hDrop, 0, filePath, MAX_PATH)) {
            LoadFileIntoVault(filePath);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        g_v.scrollY -= (delta / WHEEL_DELTA) * 72;
        RenderFrame();
        UpdateVaultTitle();
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wParam == VK_ESCAPE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (ctrl && wParam == 'O') {
            OpenFileViaDialog(hwnd);
            return 0;
        }
        if (ctrl && wParam == 'V') {
            ImportFromClipboardAndClear(hwnd);
            return 0;
        }
        if (ctrl && shift && wParam == 'C') {
            WipeProtectedVram();
            LoadWelcomeScreen();
            return 0;
        }
        if (wParam == VK_DOWN)  { g_v.scrollY += 48; RenderFrame(); UpdateVaultTitle(); return 0; }
        if (wParam == VK_UP)    { g_v.scrollY -= 48; RenderFrame(); UpdateVaultTitle(); return 0; }
        if (wParam == VK_NEXT)  { g_v.scrollY += (int)(g_v.winH * 0.8f); RenderFrame(); UpdateVaultTitle(); return 0; }
        if (wParam == VK_PRIOR) { g_v.scrollY -= (int)(g_v.winH * 0.8f); RenderFrame(); UpdateVaultTitle(); return 0; }
        if (wParam == VK_HOME)  { g_v.scrollY = 0; RenderFrame(); UpdateVaultTitle(); return 0; }
        if (wParam == VK_END)   { g_v.scrollY = 999999; RenderFrame(); UpdateVaultTitle(); return 0; }
        return 0;
    }

    case WM_CLOSE:
        WipeProtectedVram();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ReleaseD3D11Vault(void)
{
    WipeProtectedVram();
    SafeRelease(g_v.cb);
    SafeRelease(g_v.samp);
    SafeRelease(g_v.ps);
    SafeRelease(g_v.vs);
    SafeRelease(g_v.bbRtv);
    SafeRelease(g_v.sc);
    SafeRelease(g_v.ctx);
    SafeRelease(g_v.dev);
}

// ============================================================================
// Built-in Self-Test Harness (--self-test -> WinSta0\Default)
// ============================================================================
static void PumpMs(DWORD ms)
{
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

static void ComputeCapStats(const DWORD* px, UINT pitchBytes, UINT w, UINT h,
    double& nonBlackPct, size_t& exactMagenta, size_t& uniqOut, DWORD first4[4])
{
    size_t total = (size_t)w * h, nb = 0;
    exactMagenta = 0;
    std::unordered_set<DWORD> uniq;
    size_t idx = 0;
    const BYTE* base = (const BYTE*)px;
    for (UINT y = 0; y < h; y++) {
        const DWORD* row = (const DWORD*)(base + (size_t)y * pitchBytes);
        for (UINT x = 0; x < w; x++) {
            DWORD v = row[x];
            if (idx < 4) first4[idx] = v;
            int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
            if (r > 4 || g > 4 || b > 4) nb++;
            if ((v & 0x00FFFFFFu) == 0x00FF0080u) exactMagenta++;
            if (uniq.size() < 8192) uniq.insert(v);
            idx++;
        }
    }
    nonBlackPct = total ? (100.0 * (double)nb / (double)total) : 0.0;
    uniqOut = uniq.size();
}

static int RunSelfTestOnDefault(const wchar_t* logPath)
{
    FILE* log = nullptr;
    if (_wfopen_s(&log, logPath, L"wb") != 0 || !log) return 10;
    auto Lf = [&](const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        vfprintf(log, fmt, ap); fflush(log);
        va_end(ap);
    };

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WCHAR desktopName[128] = L"?";
    DWORD need = 0;
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktopName, sizeof(desktopName), &need);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VaultWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kVaultClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kVaultClass, L"AegisVault Self-Test",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 120, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    GetWindowDisplayAffinity(hwnd, &g_v.wdaActual);

    RECT cr = {};
    GetClientRect(hwnd, &cr);
    UINT cw = (UINT)(cr.right - cr.left), ch = (UINT)(cr.bottom - cr.top);
    bool d3dOk = InitD3D11Vault(hwnd, cw, ch);

    // Upload magenta test secret into HW_PROTECTED VRAM and verify CPU scrub
    bool uploadOk = false;
    bool cpuZeroed = false;
    if (d3dOk) {
        std::vector<BYTE> secret((size_t)cw * ch * 4, 0);
        DWORD* dw = (DWORD*)secret.data();
        for (size_t i = 0; i < (size_t)cw * ch; i++) dw[i] = 0xFFFF0080u;
        uploadOk = UploadPixelsToProtectedVram(secret.data(), cw, ch, true);
        SecureZeroMemory(secret.data(), secret.size());
        bool anyNonZero = false;
        for (BYTE b : secret) { if (b != 0) { anyNonZero = true; break; } }
        cpuZeroed = !anyNonZero;
        SecureZeroVec(secret);
        g_v.cpuScrubbed = cpuZeroed;
        RenderFrame();
    }
    PumpMs(160);

    // 1. PrintWindow(PW_RENDERFULLCONTENT) test
    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    UINT ww = (UINT)(wr.right - wr.left), wh = (UINT)(wr.bottom - wr.top);
    double pwNonBlack = 100.0, bbNonBlack = 100.0;
    auto DoCapture = [&](bool useBitBlt, double& outPct) {
        UINT capW = useBitBlt ? ww : ww, capH = useBitBlt ? wh : wh;
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = (LONG)capW;
        bmi.bmiHeader.biHeight = -(LONG)capH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC hScr = GetDC(nullptr);
        HDC hMem = CreateCompatibleDC(hScr);
        HBITMAP hbm = CreateDIBSection(hMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hbm && bits) {
            memset(bits, 0, (size_t)capW * capH * 4);
            HGDIOBJ old = SelectObject(hMem, hbm);
            if (useBitBlt) BitBlt(hMem, 0, 0, capW, capH, hScr, wr.left, wr.top, SRCCOPY);
            else PrintWindow(hwnd, hMem, PW_RENDERFULLCONTENT);
            GdiFlush();
            size_t dummy1 = 0, dummy2 = 0; DWORD d4[4] = {};
            ComputeCapStats((const DWORD*)bits, capW * 4, capW, capH, outPct, dummy1, dummy2, d4);
            SelectObject(hMem, old);
            DeleteObject(hbm);
        }
        DeleteDC(hMem);
        ReleaseDC(nullptr, hScr);
    };
    DoCapture(false, pwNonBlack);
    DoCapture(true, bbNonBlack);

    // 2. Same-device GPU staging readback attack on g_v.protTex
    size_t stgExactMag = 999999, stgUniq = 0;
    double stgNonBlack = 100.0;
    DWORD stgFirst4[4] = {};
    HRESULT stgMapHr = E_FAIL;
    if (g_v.dev && g_v.ctx && g_v.protTex) {
        D3D11_TEXTURE2D_DESC sd = {};
        g_v.protTex->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D* pStg = nullptr;
        if (SUCCEEDED(g_v.dev->CreateTexture2D(&sd, nullptr, &pStg)) && pStg) {
            g_v.ctx->CopyResource(pStg, g_v.protTex);
            g_v.ctx->Flush();
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            stgMapHr = g_v.ctx->Map(pStg, 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(stgMapHr) && mapped.pData) {
                ComputeCapStats((const DWORD*)mapped.pData, mapped.RowPitch, sd.Width, sd.Height,
                    stgNonBlack, stgExactMag, stgUniq, stgFirst4);
                g_v.ctx->Unmap(pStg, 0);
            } else {
                stgExactMag = 0;
            }
            pStg->Release();
        }
    }

    // 3. Quick D3D12 ProtectedResourceSession readback fuse check
    HRESULT d12PrsHr = E_FAIL, d12CloseHr = E_FAIL, d12Rem = E_FAIL;
    int d12SessStatus = -1;
    {
        IDXGIFactory4* f4 = nullptr;
        CreateDXGIFactory2(0, IID_PPV_ARGS(&f4));
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0; f4 && f4->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d = {}; ad->GetDesc1(&d);
            if (d.VendorId == 0x1002) break;
            SafeRelease(ad);
        }
        ID3D12Device* db = nullptr; ID3D12Device4* d4 = nullptr;
        if (ad && SUCCEEDED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&db))) &&
            SUCCEEDED(db->QueryInterface(IID_PPV_ARGS(&d4)))) {
            D3D12_PROTECTED_RESOURCE_SESSION_DESC prd = {};
            ID3D12ProtectedResourceSession* prs = nullptr;
            d12PrsHr = d4->CreateProtectedResourceSession(&prd, IID_PPV_ARGS(&prs));
            if (SUCCEEDED(d12PrsHr) && prs) {
                d12SessStatus = (int)prs->GetSessionStatus();
                D3D12_HEAP_PROPERTIES hpD = { D3D12_HEAP_TYPE_DEFAULT }, hpR = { D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1,
                    DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
                D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 64 * 256, 1, 1, 1,
                    DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
                ID3D12Resource* pt = nullptr, *rb = nullptr;
                d4->CreateCommittedResource1(&hpD, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, prs, IID_PPV_ARGS(&pt));
                d4->CreateCommittedResource(&hpR, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
                ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
                d4->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
                d4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cl));
                if (pt && rb && al && cl) {
                    cl->Reset(al, nullptr);
                    D3D12_TEXTURE_COPY_LOCATION sLoc = { pt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0} };
                    D3D12_TEXTURE_COPY_LOCATION dLoc = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                    dLoc.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, 64, 64, 1, 256 };
                    cl->CopyTextureRegion(&dLoc, 0, 0, 0, &sLoc, nullptr);
                    d12CloseHr = cl->Close();
                    d12Rem = d4->GetDeviceRemovedReason();
                }
                SafeRelease(cl); SafeRelease(al); SafeRelease(rb); SafeRelease(pt); prs->Release();
            }
            d4->Release();
        }
        SafeRelease(db); SafeRelease(ad); SafeRelease(f4);
    }

    // 4. Unprotected A/B control check using exact same DrawQuadWithSrv shader
    size_t ctrlExactMag = 0, ctrlUniq = 0;
    double ctrlNonBlack = 0.0;
    DWORD ctrlFirst4[4] = {};
    if (g_v.dev && g_v.ctx) {
        const UINT CW = 64, CH = 64;
        std::vector<DWORD> mag(CW * CH, 0xFFFF0080u);
        D3D11_TEXTURE2D_DESC td = { CW, CH, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        D3D11_SUBRESOURCE_DATA init = { mag.data(), CW * 4, 0 };
        ID3D11Texture2D* pCSrc = nullptr, *pCDst = nullptr, *pCStg = nullptr;
        ID3D11ShaderResourceView* pCSrv = nullptr;
        ID3D11RenderTargetView* pCRtv = nullptr;
        g_v.dev->CreateTexture2D(&td, &init, &pCSrc);
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        g_v.dev->CreateTexture2D(&td, nullptr, &pCDst);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_v.dev->CreateTexture2D(&td, nullptr, &pCStg);
        if (pCSrc && pCDst && pCStg &&
            SUCCEEDED(g_v.dev->CreateShaderResourceView(pCSrc, nullptr, &pCSrv)) &&
            SUCCEEDED(g_v.dev->CreateRenderTargetView(pCDst, nullptr, &pCRtv))) {
            DrawQuadWithSrv(pCSrv, pCRtv, CW, CH, 0.0f, 0.0f, 1.0f, 1.0f);
            g_v.ctx->CopyResource(pCStg, pCDst);
            g_v.ctx->Flush();
            D3D11_MAPPED_SUBRESOURCE cm = {};
            if (SUCCEEDED(g_v.ctx->Map(pCStg, 0, D3D11_MAP_READ, 0, &cm)) && cm.pData) {
                ComputeCapStats((const DWORD*)cm.pData, cm.RowPitch, CW, CH, ctrlNonBlack, ctrlExactMag, ctrlUniq, ctrlFirst4);
                g_v.ctx->Unmap(pCStg, 0);
            }
        }
        SafeRelease(pCRtv); SafeRelease(pCSrv);
        SafeRelease(pCStg); SafeRelease(pCDst); SafeRelease(pCSrc);
    }

    UINT savedProtMisc = g_v.protTexMisc;
    HRESULT devRem = g_v.dev ? g_v.dev->GetDeviceRemovedReason() : E_FAIL;
    ReleaseD3D11Vault();
    DestroyWindow(hwnd);
    PumpMs(50);
    HWND leftover = FindWindowW(kVaultClass, nullptr);
    CoUninitialize();

    Lf("========================================================================\n");
    Lf("AegisVault Self-Test Verification (WinSta0\\%ls)\n", desktopName);
    Lf("========================================================================\n");
    Lf("[INIT] D3D11=%d UploadOk=%d cpuScrubbed=%d DevRem=0x%08lX\n", d3dOk ? 1 : 0, uploadOk ? 1 : 0, cpuZeroed ? 1 : 0, devRem);
    Lf("[FLAGS] SwapChainFlags=0x%X BackBufMisc=0x%X ProtTexMisc=0x%X WDA=0x%lX\n",
        g_v.scFlags, g_v.bbMisc, savedProtMisc, g_v.wdaActual);
    Lf("[CAPTURE] PrintWindow(PW_RENDERFULLCONTENT)=%.2f%% | BitBlt(screen)=%.2f%%\n", pwNonBlack, bbNonBlack);
    Lf("[VRAM_CONTROL] UnprotectedShader Copy->Staging: exactMagenta=%zu/4096 uniq=%zu first4=[%08lX %08lX %08lX %08lX]\n",
        ctrlExactMag, ctrlUniq, ctrlFirst4[0], ctrlFirst4[1], ctrlFirst4[2], ctrlFirst4[3]);
    Lf("[VRAM_ATTACK] CopyResource(Staging<-ProtTex) MapHr=0x%08lX | exactMagenta=%zu/%u uniq=%zu first4=[%08lX %08lX %08lX %08lX]\n",
        stgMapHr, stgExactMag, cw * ch, stgUniq, stgFirst4[0], stgFirst4[1], stgFirst4[2], stgFirst4[3]);
    Lf("[D3D12_FUSE] CreatePRS=0x%08lX SessStatus=%d | AttackerCopy CloseHr=0x%08lX DevRem=0x%08lX\n",
        d12PrsHr, d12SessStatus, d12CloseHr, d12Rem);
    Lf("[CLEANUP] Leftover=%p\n", (void*)leftover);

    bool pass = d3dOk && uploadOk && cpuZeroed && devRem == 0 &&
        (g_v.scFlags & DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED) &&
        (savedProtMisc & D3D11_RESOURCE_MISC_HW_PROTECTED) &&
        g_v.wdaActual == WDA_EXCLUDEFROMCAPTURE &&
        pwNonBlack < 0.01 && bbNonBlack < 0.01 && ctrlExactMag == 4096 && stgExactMag == 0 && leftover == nullptr;
    Lf("[VERDICT] %s\n", pass ? "ALL_PASS" : "FAILED");
    Lf("========================================================================\n");
    fclose(log);
    return pass ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc >= 3 && _wcsicmp(argv[1], L"--self-test-default") == 0) {
        int rc = RunSelfTestOnDefault(argv[2]);
        LocalFree(argv);
        return rc;
    }

    if (argc >= 2 && _wcsicmp(argv[1], L"--self-test") == 0) {
        WCHAR selfPath[MAX_PATH] = L"", logPath[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        wcscpy_s(logPath, selfPath);
        wchar_t* dot = wcsrchr(logPath, L'.');
        if (dot) wcscpy_s(dot, MAX_PATH - (dot - logPath), L"_selftest.log");
        else wcscat_s(logPath, L"_selftest.log");
        DeleteFileW(logPath);

        WCHAR cmdLine[1024] = L"";
        _snwprintf_s(cmdLine, _TRUNCATE, L"\"%ls\" --self-test-default \"%ls\"", selfPath, logPath);
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<LPWSTR>(L"WinSta0\\Default");
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(selfPath, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            printf("[FATAL] CreateProcessW failed err=%lu\n", GetLastError());
            LocalFree(argv);
            return 2;
        }
        WaitForSingleObject(pi.hProcess, 20000);
        DWORD rc = 1;
        GetExitCodeProcess(pi.hProcess, &rc);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        FILE* fp = nullptr;
        if (_wfopen_s(&fp, logPath, L"rb") == 0 && fp) {
            char buf[4096]; size_t n = 0;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) fwrite(buf, 1, n, stdout);
            fflush(stdout);
            fclose(fp);
        }
        DeleteFileW(logPath);
        LocalFree(argv);
        return (int)rc;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HardenVaultMitigations();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = VaultWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kVaultClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kVaultClass,
        L"AegisVault (初始化保护显存中...)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        if (argv) LocalFree(argv);
        CoUninitialize();
        return 1;
    }

    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    GetWindowDisplayAffinity(hwnd, &g_v.wdaActual);
    DragAcceptFiles(hwnd, TRUE);

    RECT cr = {};
    GetClientRect(hwnd, &cr);
    if (!InitD3D11Vault(hwnd, (UINT)(cr.right - cr.left), (UINT)(cr.bottom - cr.top))) {
        MessageBoxW(hwnd, L"无法初始化硬件保护显存管线，请确认显卡驱动正常。", L"AegisVault", MB_ICONERROR);
        ReleaseD3D11Vault();
        DestroyWindow(hwnd);
        if (argv) LocalFree(argv);
        CoUninitialize();
        return 2;
    }

    if (argc >= 2 && argv[1][0] != L'-') {
        if (!LoadFileIntoVault(argv[1])) {
            LoadWelcomeScreen();
        }
    } else {
        LoadWelcomeScreen();
    }
    if (argv) LocalFree(argv);

    ShowWindow(hwnd, nShow ? nShow : SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ReleaseD3D11Vault();
    CoUninitialize();
    return (int)msg.wParam;
}
