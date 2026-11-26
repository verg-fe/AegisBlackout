#pragma once
#ifndef GHOST_SHARED_H
#define GHOST_SHARED_H

#include <windows.h>
#include <stdint.h>

#define GHOSTSWAP_MAGIC   0x47535750u /* 'GSWP' */
#define GHOSTSWAP_VERSION 2u

/* Sentinel 64-bit placeholder embedded in .gpic and patched per-target to remoteData address */
#define GHOST_STUB_DATA_SENTINEL 0x1122334455667788ULL

#define GHOST_RING_SIZE 16u

enum GhostCallKind : uint32_t {
    GHOST_KIND_CREATE_LEGACY       = 1,
    GHOST_KIND_CREATE_HWND         = 2,
    GHOST_KIND_CREATE_COREWINDOW   = 3,
    GHOST_KIND_CREATE_COMPOSITION  = 4,
    GHOST_KIND_CREATE_COMP_SURFACE = 5,
    GHOST_KIND_RESIZE_BUFFERS      = 6,
    GHOST_KIND_RESIZE_BUFFERS1     = 7
};

#pragma pack(push, 8)
struct GhostTelemetryEntry {
    uint32_t kind;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t origFlags;
    uint32_t finalFlags;
    uint32_t backBufMisc;
    int32_t  hr;
};

struct GhostStubData {
    uint32_t magic;
    uint32_t version;
    volatile LONG enabled;

    uint32_t stripFlagsMask;          /* e.g. DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING (0x800) on fallback */
    uint32_t upgradeFlagsPrimary;     /* Default v2: 0x20 (DISPLAY_ONLY); opt-in --hw-protected: 0x420 */
    uint32_t upgradeFlagsFallback;    /* Default v2: 0x20; opt-in --hw-protected: 0x400 */
    uint32_t forceResetOnceForExisting;
    uint32_t maxResetInjections;

    /* Original DXGI method pointers in the target process */
    void* origCreateSwapChain;
    void* origCreateForHwnd;
    void* origCreateForCoreWindow;
    void* origCreateForComposition;
    void* origCreateForCompSurface;
    void* origResizeBuffers;
    void* origResizeBuffers1;
    void* origPresent;
    void* origPresent1;

    /* Embedded GUIDs so .gpic never references .rdata */
    GUID iidD3D11Texture2D;
    GUID iidDXGISwapChain1;

    /* Live atomic telemetry */
    volatile LONG totalCreateCalls;
    volatile LONG upgraded420;            /* Primary upgrades (0x20 in v2 or 0x420 if --hw-protected) */
    volatile LONG upgraded400;            /* Secondary fallback upgrades */
    volatile LONG fallbackOrig;
    volatile LONG resizeCalls;
    volatile LONG resizeUpgraded;
    volatile LONG verifiedHwProtBackBuf;  /* BackBuf MiscFlags & 0x80000 */
    volatile LONG verifiedDisplayOnly;    /* Actual SwapChain Desc1.Flags & 0x20 */
    volatile LONG presentCalls;
    volatile LONG deviceResetInjected;

    volatile LONG lastHr;
    volatile LONG lastOrigFlags;
    volatile LONG lastFinalFlags;
    volatile LONG lastBackBufMisc;
    volatile LONG lastFormat;
    volatile LONG lastW;
    volatile LONG lastH;

    volatile LONG ringWriteIdx;
    GhostTelemetryEntry ring[GHOST_RING_SIZE];
};
#pragma pack(pop)

enum GhostHookId : uint32_t {
    GHOOK_CREATE_SWAPCHAIN = 0,
    GHOOK_CREATE_FOR_HWND = 1,
    GHOOK_CREATE_FOR_COREWINDOW = 2,
    GHOOK_CREATE_FOR_COMPOSITION = 3,
    GHOOK_CREATE_FOR_COMP_SURFACE = 4,
    GHOOK_RESIZE_BUFFERS = 5,
    GHOOK_RESIZE_BUFFERS1 = 6,
    GHOOK_PRESENT = 7,
    GHOOK_PRESENT1 = 8,
    GHOOK_COUNT = 9
};

#ifdef __cplusplus
extern "C" {
#endif
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChain(
    void* This, void* pDevice, void* pDesc, void** ppSwapChain);
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChainForHwnd(
    void* This, void* pDevice, HWND hWnd, const void* pDesc,
    const void* pFullscreenDesc, void* pRestrictToOutput, void** ppSwapChain);
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChainForCoreWindow(
    void* This, void* pDevice, void* pWindow, const void* pDesc,
    void* pRestrictToOutput, void** ppSwapChain);
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChainForComposition(
    void* This, void* pDevice, const void* pDesc,
    void* pRestrictToOutput, void** ppSwapChain);
HRESULT STDMETHODCALLTYPE GhostHook_CreateSwapChainForCompSurface(
    void* This, void* pDevice, HANDLE hSurface, const void* pDesc,
    void* pRestrictToOutput, void** ppSwapChain);
HRESULT STDMETHODCALLTYPE GhostHook_ResizeBuffers(
    void* This, UINT BufferCount, UINT Width, UINT Height,
    UINT NewFormat, UINT SwapChainFlags);
HRESULT STDMETHODCALLTYPE GhostHook_ResizeBuffers1(
    void* This, UINT BufferCount, UINT Width, UINT Height,
    UINT NewFormat, UINT SwapChainFlags,
    const UINT* pCreationNodeMask, void* const* ppPresentQueue);
HRESULT STDMETHODCALLTYPE GhostHook_Present(
    void* This, UINT SyncInterval, UINT Flags);
HRESULT STDMETHODCALLTYPE GhostHook_Present1(
    void* This, UINT SyncInterval, UINT PresentFlags, const void* pPresentParameters);
#ifdef __cplusplus
}
#endif

#endif /* GHOST_SHARED_H */
