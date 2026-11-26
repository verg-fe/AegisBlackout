/*
 * ghost_pic.cpp - Position-Independent DXGI VTable Hook Code (.gpic section)
 * Compile ONLY with:
 *   cl /nologo /c /O2 /GS- /GL- /EHs-c- /GR- /Oi- /guard:cf- /Fo:ghost_pic.obj ghost_pic.cpp
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include "ghost_shared.h"

#pragma code_seg(".gpic")
#pragma optimize("t", on)

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain)(
    void* This, void* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(
    void* This, void* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForCoreWindow)(
    void* This, void* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForComposition)(
    void* This, void* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForCompSurface)(
    void* This, void* pDevice, HANDLE hSurface, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers)(
    void* This, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers1)(
    void* This, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags,
    const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(
    void* This, UINT SyncInterval, UINT Flags);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1)(
    void* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);

static __forceinline GhostStubData* PicGetData(void)
{
    volatile UINT64 addr = GHOST_STUB_DATA_SENTINEL;
    return (GhostStubData*)(ULONG_PTR)addr;
}

static __forceinline void PicCopyDwords(void* dst, const void* src, UINT dwordCount)
{
    volatile DWORD* d = (volatile DWORD*)dst;
    const DWORD* s = (const DWORD*)src;
    for (UINT i = 0; i < dwordCount; i++) {
        d[i] = s[i];
    }
}

static __forceinline void PicZeroDwords(void* dst, UINT dwordCount)
{
    volatile DWORD* d = (volatile DWORD*)dst;
    for (UINT i = 0; i < dwordCount; i++) {
        d[i] = 0;
    }
}

static __forceinline UINT PicInspectSwapChain(GhostStubData* d, IDXGISwapChain* sc, UINT* outActualFlags)
{
    if (!sc) return 0;

    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(d->iidDXGISwapChain1, (void**)&sc1)) && sc1) {
        DXGI_SWAP_CHAIN_DESC1 ad;
        PicZeroDwords(&ad, sizeof(ad) / 4);
        if (SUCCEEDED(sc1->GetDesc1(&ad))) {
            if (outActualFlags) *outActualFlags = ad.Flags;
            if ((ad.Flags & 0x20u) != 0) {
                _InterlockedIncrement(&d->verifiedDisplayOnly);
            }
        }
        sc1->Release();
    }

    ID3D11Texture2D* pTex = nullptr;
    HRESULT hr = sc->GetBuffer(0, d->iidD3D11Texture2D, (void**)&pTex);
    if (FAILED(hr) || !pTex) return 0;
    D3D11_TEXTURE2D_DESC td;
    PicZeroDwords(&td, sizeof(td) / 4);
    pTex->GetDesc(&td);
    UINT misc = td.MiscFlags;
    pTex->Release();
    if (misc & 0x80000u) {
        _InterlockedIncrement(&d->verifiedHwProtBackBuf);
    }
    return misc;
}

static __forceinline void PicRecordTelemetry(
    GhostStubData* d, uint32_t kind, uint32_t w, uint32_t h, uint32_t fmt,
    uint32_t origFlags, uint32_t finalFlags, uint32_t bbMisc, HRESULT hr)
{
    _InterlockedExchange(&d->lastHr, (LONG)hr);
    _InterlockedExchange(&d->lastOrigFlags, (LONG)origFlags);
    _InterlockedExchange(&d->lastFinalFlags, (LONG)finalFlags);
    _InterlockedExchange(&d->lastBackBufMisc, (LONG)bbMisc);
    _InterlockedExchange(&d->lastFormat, (LONG)fmt);
    _InterlockedExchange(&d->lastW, (LONG)w);
    _InterlockedExchange(&d->lastH, (LONG)h);

    LONG idx = _InterlockedIncrement(&d->ringWriteIdx) - 1;
    uint32_t slot = (uint32_t)idx & (GHOST_RING_SIZE - 1u);
    d->ring[slot].kind = kind;
    d->ring[slot].width = w;
    d->ring[slot].height = h;
    d->ring[slot].format = fmt;
    d->ring[slot].origFlags = origFlags;
    d->ring[slot].finalFlags = finalFlags;
    d->ring[slot].backBufMisc = bbMisc;
    d->ring[slot].hr = (int32_t)hr;
}

struct Desc1CallCtx {
    void* origFn;
    void* This;
    void* pDevice;
    void* argExtra;
    const void* argExtra2;
    IDXGIOutput* pRestrictToOutput;
    uint32_t kind;
};

static __forceinline HRESULT PicCallDesc1Raw(const Desc1CallCtx* c, const DXGI_SWAP_CHAIN_DESC1* pTryDesc, IDXGISwapChain1** ppOut)
{
    switch (c->kind) {
    case GHOST_KIND_CREATE_HWND:
        return ((PFN_CreateSwapChainForHwnd)c->origFn)(
            c->This, c->pDevice, (HWND)c->argExtra, pTryDesc,
            (const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*)c->argExtra2, c->pRestrictToOutput, ppOut);
    case GHOST_KIND_CREATE_COREWINDOW:
        return ((PFN_CreateSwapChainForCoreWindow)c->origFn)(
            c->This, c->pDevice, (IUnknown*)c->argExtra, pTryDesc, c->pRestrictToOutput, ppOut);
    case GHOST_KIND_CREATE_COMPOSITION:
        return ((PFN_CreateSwapChainForComposition)c->origFn)(
            c->This, c->pDevice, pTryDesc, c->pRestrictToOutput, ppOut);
    case GHOST_KIND_CREATE_COMP_SURFACE:
        return ((PFN_CreateSwapChainForCompSurface)c->origFn)(
            c->This, c->pDevice, (HANDLE)c->argExtra, pTryDesc, c->pRestrictToOutput, ppOut);
    default:
        return E_FAIL;
    }
}

static __forceinline HRESULT PicUpgradeDesc1(
    GhostStubData* d, const Desc1CallCtx* c,
    const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGISwapChain1** ppSwapChain)
{
    _InterlockedIncrement(&d->totalCreateCalls);

    if (!d->enabled || !pDesc || !ppSwapChain) {
        return PicCallDesc1Raw(c, pDesc, ppSwapChain);
    }

    DXGI_SWAP_CHAIN_DESC1 modDesc;
    PicCopyDwords(&modDesc, pDesc, sizeof(modDesc) / 4);

    UINT origFlags = pDesc->Flags;
    UINT finalFlags = origFlags;
    UINT primary = d->upgradeFlagsPrimary;
    UINT fallback = d->upgradeFlagsFallback;
    UINT stripMask = d->stripFlagsMask;

    /* Tier 1: Preserve existing flags + OR primary (0x20 default, or 0x420 if --hw-protected) */
    modDesc.Flags = origFlags | primary;
    HRESULT hr = PicCallDesc1Raw(c, &modDesc, ppSwapChain);
    if (SUCCEEDED(hr)) {
        finalFlags = modDesc.Flags;
        _InterlockedIncrement(&d->upgraded420);
        UINT bbMisc = PicInspectSwapChain(d, *ppSwapChain, &finalFlags);
        PicRecordTelemetry(d, c->kind, pDesc->Width, pDesc->Height, (uint32_t)pDesc->Format,
            origFlags, finalFlags, bbMisc, hr);
        return hr;
    }

    /* Tier 2: Strip conflicting flags (e.g. ALLOW_TEARING) + primary */
    if (stripMask && (origFlags & stripMask) != 0) {
        modDesc.Flags = (origFlags & ~stripMask) | primary;
        hr = PicCallDesc1Raw(c, &modDesc, ppSwapChain);
        if (SUCCEEDED(hr)) {
            finalFlags = modDesc.Flags;
            _InterlockedIncrement(&d->upgraded420);
            UINT bbMisc = PicInspectSwapChain(d, *ppSwapChain, &finalFlags);
            PicRecordTelemetry(d, c->kind, pDesc->Width, pDesc->Height, (uint32_t)pDesc->Format,
                origFlags, finalFlags, bbMisc, hr);
            return hr;
        }
    }

    /* Tier 3: Fallback flag (skip duplicate attempt if fallback == primary) */
    if (fallback != primary && fallback != 0) {
        modDesc.Flags = (origFlags & ~stripMask) | fallback;
        hr = PicCallDesc1Raw(c, &modDesc, ppSwapChain);
        if (SUCCEEDED(hr)) {
            finalFlags = modDesc.Flags;
            _InterlockedIncrement(&d->upgraded400);
            UINT bbMisc = PicInspectSwapChain(d, *ppSwapChain, &finalFlags);
            PicRecordTelemetry(d, c->kind, pDesc->Width, pDesc->Height, (uint32_t)pDesc->Format,
                origFlags, finalFlags, bbMisc, hr);
            return hr;
        }
    }

    /* Tier 4: Exact untouched original descriptor */
    hr = PicCallDesc1Raw(c, pDesc, ppSwapChain);
    _InterlockedIncrement(&d->fallbackOrig);
    UINT actualF = origFlags;
    UINT bbMisc = SUCCEEDED(hr) ? PicInspectSwapChain(d, *ppSwapChain, &actualF) : 0;
    PicRecordTelemetry(d, c->kind, pDesc->Width, pDesc->Height, (uint32_t)pDesc->Format,
        origFlags, actualF, bbMisc, hr);
    return hr;
}

extern "C" {

__declspec(noinline)
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChain(
    void* This, void* pDevice, void* pDescRaw, void** ppSwapChain)
{
    GhostStubData* d = PicGetData();
    PFN_CreateSwapChain orig = (PFN_CreateSwapChain)d->origCreateSwapChain;
    DXGI_SWAP_CHAIN_DESC* pDesc = (DXGI_SWAP_CHAIN_DESC*)pDescRaw;
    IDXGISwapChain** ppOut = (IDXGISwapChain**)ppSwapChain;

    _InterlockedIncrement(&d->totalCreateCalls);
    if (!d->enabled || !pDesc || !ppOut) {
        return orig(This, pDevice, pDesc, ppOut);
    }

    DXGI_SWAP_CHAIN_DESC modDesc;
    PicCopyDwords(&modDesc, pDesc, sizeof(modDesc) / 4);
    UINT origFlags = pDesc->Flags;
    UINT primary = d->upgradeFlagsPrimary;
    UINT fallback = d->upgradeFlagsFallback;
    UINT stripMask = d->stripFlagsMask;

    modDesc.Flags = origFlags | primary;
    HRESULT hr = orig(This, pDevice, &modDesc, ppOut);
    if (SUCCEEDED(hr)) {
        UINT actualF = modDesc.Flags;
        _InterlockedIncrement(&d->upgraded420);
        UINT bbMisc = PicInspectSwapChain(d, *ppOut, &actualF);
        PicRecordTelemetry(d, GHOST_KIND_CREATE_LEGACY, pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
            (uint32_t)pDesc->BufferDesc.Format, origFlags, actualF, bbMisc, hr);
        return hr;
    }

    if (stripMask && (origFlags & stripMask) != 0) {
        modDesc.Flags = (origFlags & ~stripMask) | primary;
        hr = orig(This, pDevice, &modDesc, ppOut);
        if (SUCCEEDED(hr)) {
            UINT actualF = modDesc.Flags;
            _InterlockedIncrement(&d->upgraded420);
            UINT bbMisc = PicInspectSwapChain(d, *ppOut, &actualF);
            PicRecordTelemetry(d, GHOST_KIND_CREATE_LEGACY, pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
                (uint32_t)pDesc->BufferDesc.Format, origFlags, actualF, bbMisc, hr);
            return hr;
        }
    }

    if (fallback != primary && fallback != 0) {
        modDesc.Flags = (origFlags & ~stripMask) | fallback;
        hr = orig(This, pDevice, &modDesc, ppOut);
        if (SUCCEEDED(hr)) {
            UINT actualF = modDesc.Flags;
            _InterlockedIncrement(&d->upgraded400);
            UINT bbMisc = PicInspectSwapChain(d, *ppOut, &actualF);
            PicRecordTelemetry(d, GHOST_KIND_CREATE_LEGACY, pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
                (uint32_t)pDesc->BufferDesc.Format, origFlags, actualF, bbMisc, hr);
            return hr;
        }
    }

    hr = orig(This, pDevice, pDesc, ppOut);
    _InterlockedIncrement(&d->fallbackOrig);
    UINT actualF = origFlags;
    UINT bbMisc = SUCCEEDED(hr) ? PicInspectSwapChain(d, *ppOut, &actualF) : 0;
    PicRecordTelemetry(d, GHOST_KIND_CREATE_LEGACY, pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
        (uint32_t)pDesc->BufferDesc.Format, origFlags, actualF, bbMisc, hr);
    return hr;
}

__declspec(noinline)
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChainForHwnd(
    void* This, void* pDevice, HWND hWnd, const void* pDesc,
    const void* pFullscreenDesc, void* pRestrictToOutput, void** ppSwapChain)
{
    GhostStubData* d = PicGetData();
    Desc1CallCtx c;
    c.origFn = d->origCreateForHwnd;
    c.This = This;
    c.pDevice = pDevice;
    c.argExtra = (void*)hWnd;
    c.argExtra2 = pFullscreenDesc;
    c.pRestrictToOutput = (IDXGIOutput*)pRestrictToOutput;
    c.kind = GHOST_KIND_CREATE_HWND;
    return PicUpgradeDesc1(d, &c, (const DXGI_SWAP_CHAIN_DESC1*)pDesc, (IDXGISwapChain1**)ppSwapChain);
}

__declspec(noinline)
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChainForCoreWindow(
    void* This, void* pDevice, void* pWindow, const void* pDesc,
    void* pRestrictToOutput, void** ppSwapChain)
{
    GhostStubData* d = PicGetData();
    Desc1CallCtx c;
    c.origFn = d->origCreateForCoreWindow;
    c.This = This;
    c.pDevice = pDevice;
    c.argExtra = pWindow;
    c.argExtra2 = nullptr;
    c.pRestrictToOutput = (IDXGIOutput*)pRestrictToOutput;
    c.kind = GHOST_KIND_CREATE_COREWINDOW;
    return PicUpgradeDesc1(d, &c, (const DXGI_SWAP_CHAIN_DESC1*)pDesc, (IDXGISwapChain1**)ppSwapChain);
}

__declspec(noinline)
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChainForComposition(
    void* This, void* pDevice, const void* pDesc,
    void* pRestrictToOutput, void** ppSwapChain)
{
    GhostStubData* d = PicGetData();
    Desc1CallCtx c;
    c.origFn = d->origCreateForComposition;
    c.This = This;
    c.pDevice = pDevice;
    c.argExtra = nullptr;
    c.argExtra2 = nullptr;
    c.pRestrictToOutput = (IDXGIOutput*)pRestrictToOutput;
    c.kind = GHOST_KIND_CREATE_COMPOSITION;
    return PicUpgradeDesc1(d, &c, (const DXGI_SWAP_CHAIN_DESC1*)pDesc, (IDXGISwapChain1**)ppSwapChain);
}

__declspec(noinline)
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChainForCompSurface(
    void* This, void* pDevice, HANDLE hSurface, const void* pDesc,
    void* pRestrictToOutput, void** ppSwapChain)
{
    GhostStubData* d = PicGetData();
    Desc1CallCtx c;
    c.origFn = d->origCreateForCompSurface;
    c.This = This;
    c.pDevice = pDevice;
    c.argExtra = (void*)hSurface;
    c.argExtra2 = nullptr;
    c.pRestrictToOutput = (IDXGIOutput*)pRestrictToOutput;
    c.kind = GHOST_KIND_CREATE_COMP_SURFACE;
    return PicUpgradeDesc1(d, &c, (const DXGI_SWAP_CHAIN_DESC1*)pDesc, (IDXGISwapChain1**)ppSwapChain);
}

__declspec(noinline)
HRESULT STDMETHODCALLTYPE GhostHook_ResizeBuffers(
    void* This, UINT BufferCount, UINT Width, UINT Height,
    UINT NewFormat, UINT SwapChainFlags)
{
    GhostStubData* d = PicGetData();
    PFN_ResizeBuffers orig = (PFN_ResizeBuffers)d->origResizeBuffers;
    DXGI_FORMAT fmt = (DXGI_FORMAT)NewFormat;

    _InterlockedIncrement(&d->resizeCalls);
    if (!d->enabled) {
        return orig(This, BufferCount, Width, Height, fmt, SwapChainFlags);
    }

    UINT primary = d->upgradeFlagsPrimary;
    UINT fallback = d->upgradeFlagsFallback;
    UINT stripMask = d->stripFlagsMask;

    UINT finalFlags = SwapChainFlags | primary;
    HRESULT hr = orig(This, BufferCount, Width, Height, fmt, finalFlags);
    if (FAILED(hr) && stripMask && (SwapChainFlags & stripMask) != 0) {
        finalFlags = (SwapChainFlags & ~stripMask) | primary;
        hr = orig(This, BufferCount, Width, Height, fmt, finalFlags);
    }
    if (FAILED(hr) && fallback != primary && fallback != 0) {
        finalFlags = (SwapChainFlags & ~stripMask) | fallback;
        hr = orig(This, BufferCount, Width, Height, fmt, finalFlags);
    }
    if (SUCCEEDED(hr)) {
        _InterlockedIncrement(&d->resizeUpgraded);
        UINT actualF = finalFlags;
        UINT bbMisc = PicInspectSwapChain(d, (IDXGISwapChain*)This, &actualF);
        PicRecordTelemetry(d, GHOST_KIND_RESIZE_BUFFERS, Width, Height, NewFormat,
            SwapChainFlags, actualF, bbMisc, hr);
        return hr;
    }

    hr = orig(This, BufferCount, Width, Height, fmt, SwapChainFlags);
    UINT actualF = SwapChainFlags;
    UINT bbMisc = SUCCEEDED(hr) ? PicInspectSwapChain(d, (IDXGISwapChain*)This, &actualF) : 0;
    PicRecordTelemetry(d, GHOST_KIND_RESIZE_BUFFERS, Width, Height, NewFormat,
        SwapChainFlags, actualF, bbMisc, hr);
    return hr;
}

__declspec(noinline)
HRESULT STDMETHODCALLTYPE GhostHook_ResizeBuffers1(
    void* This, UINT BufferCount, UINT Width, UINT Height,
    UINT NewFormat, UINT SwapChainFlags,
    const UINT* pCreationNodeMask, void* const* ppPresentQueue)
{
    GhostStubData* d = PicGetData();
    PFN_ResizeBuffers1 orig = (PFN_ResizeBuffers1)d->origResizeBuffers1;
    DXGI_FORMAT fmt = (DXGI_FORMAT)NewFormat;
    IUnknown* const* queues = (IUnknown* const*)ppPresentQueue;

    _InterlockedIncrement(&d->resizeCalls);
    if (!d->enabled) {
        return orig(This, BufferCount, Width, Height, fmt, SwapChainFlags, pCreationNodeMask, queues);
    }

    UINT primary = d->upgradeFlagsPrimary;
    UINT fallback = d->upgradeFlagsFallback;
    UINT stripMask = d->stripFlagsMask;

    UINT finalFlags = SwapChainFlags | primary;
    HRESULT hr = orig(This, BufferCount, Width, Height, fmt, finalFlags, pCreationNodeMask, queues);
    if (FAILED(hr) && stripMask && (SwapChainFlags & stripMask) != 0) {
        finalFlags = (SwapChainFlags & ~stripMask) | primary;
        hr = orig(This, BufferCount, Width, Height, fmt, finalFlags, pCreationNodeMask, queues);
    }
    if (FAILED(hr) && fallback != primary && fallback != 0) {
        finalFlags = (SwapChainFlags & ~stripMask) | fallback;
        hr = orig(This, BufferCount, Width, Height, fmt, finalFlags, pCreationNodeMask, queues);
    }
    if (SUCCEEDED(hr)) {
        _InterlockedIncrement(&d->resizeUpgraded);
        UINT actualF = finalFlags;
        UINT bbMisc = PicInspectSwapChain(d, (IDXGISwapChain*)This, &actualF);
        PicRecordTelemetry(d, GHOST_KIND_RESIZE_BUFFERS1, Width, Height, NewFormat,
            SwapChainFlags, actualF, bbMisc, hr);
        return hr;
    }

    hr = orig(This, BufferCount, Width, Height, fmt, SwapChainFlags, pCreationNodeMask, queues);
    UINT actualF = SwapChainFlags;
    UINT bbMisc = SUCCEEDED(hr) ? PicInspectSwapChain(d, (IDXGISwapChain*)This, &actualF) : 0;
    PicRecordTelemetry(d, GHOST_KIND_RESIZE_BUFFERS1, Width, Height, NewFormat,
        SwapChainFlags, actualF, bbMisc, hr);
    return hr;
}

__declspec(noinline)
HRESULT STDMETHODCALLTYPE GhostHook_Present(
    void* This, UINT SyncInterval, UINT Flags)
{
    GhostStubData* d = PicGetData();
    _InterlockedIncrement(&d->presentCalls);
    if (d->enabled && d->forceResetOnceForExisting && !(Flags & DXGI_PRESENT_TEST)) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(((IDXGISwapChain*)This)->QueryInterface(d->iidDXGISwapChain1, (void**)&sc1)) && sc1) {
            DXGI_SWAP_CHAIN_DESC1 scDesc;
            PicZeroDwords(&scDesc, sizeof(scDesc) / 4);
            BOOL needReset = SUCCEEDED(sc1->GetDesc1(&scDesc)) && ((scDesc.Flags & d->upgradeFlagsPrimary) != d->upgradeFlagsPrimary);
            sc1->Release();
            if (needReset) {
                LONG cur = d->deviceResetInjected;
                if (cur < (LONG)d->maxResetInjections) {
                    if (_InterlockedCompareExchange(&d->deviceResetInjected, cur + 1, cur) == cur) {
                        return DXGI_ERROR_DEVICE_RESET;
                    }
                }
            }
        }
    }
    return ((PFN_Present)d->origPresent)(This, SyncInterval, Flags);
}

__declspec(noinline)
HRESULT STDMETHODCALLTYPE GhostHook_Present1(
    void* This, UINT SyncInterval, UINT PresentFlags, const void* pPresentParameters)
{
    GhostStubData* d = PicGetData();
    _InterlockedIncrement(&d->presentCalls);
    if (d->enabled && d->forceResetOnceForExisting && !(PresentFlags & DXGI_PRESENT_TEST)) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(((IDXGISwapChain*)This)->QueryInterface(d->iidDXGISwapChain1, (void**)&sc1)) && sc1) {
            DXGI_SWAP_CHAIN_DESC1 scDesc;
            PicZeroDwords(&scDesc, sizeof(scDesc) / 4);
            BOOL needReset = SUCCEEDED(sc1->GetDesc1(&scDesc)) && ((scDesc.Flags & d->upgradeFlagsPrimary) != d->upgradeFlagsPrimary);
            sc1->Release();
            if (needReset) {
                LONG cur = d->deviceResetInjected;
                if (cur < (LONG)d->maxResetInjections) {
                    if (_InterlockedCompareExchange(&d->deviceResetInjected, cur + 1, cur) == cur) {
                        return DXGI_ERROR_DEVICE_RESET;
                    }
                }
            }
        }
    }
    return ((PFN_Present1)d->origPresent1)(This, SyncInterval, PresentFlags, (const DXGI_PRESENT_PARAMETERS*)pPresentParameters);
}

} /* extern "C" */
