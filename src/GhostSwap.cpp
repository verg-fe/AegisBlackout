/*
 * GhostSwap.cpp (v2.1)
 * - Silent Background Engine (/SUBSYSTEM:WINDOWS, zero console box)
 * - Default Cooperative Shield Mode (lets AegisBlackout v4.1 handle global WDA=0x11 so Ctrl+Alt+Q captures cleanly)
 * - Optional Extreme Mode (--lock-vram): Global No-Whitelist DXGI SwapChain Upgrader (0x20 DISPLAY_ONLY zero-snow; optional --hw-protected 0x420)
 * - 0%-Flicker 0%-Idle-GPU Instant Privacy Blackout / Dimmer Veil (Ctrl+Alt+F9/F10/F11)
 *
 * Build:
 *   1) cl /nologo /c /O2 /GS- /GL- /EHs-c- /GR- /Oi- /guard:cf- /Fo:ghost_pic.obj ghost_pic.cpp
 *   2) cl /nologo /utf-8 /O2 /W3 /EHsc /MT GhostSwap.cpp ghost_pic.obj /Fe:GhostSwap.exe
 *        /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /INCREMENTAL:NO
 *        user32.lib gdi32.lib d3d11.lib dxgi.lib dcomp.lib advapi32.lib psapi.lib shell32.lib
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <dcomp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <algorithm>
#include <atomic>

#include "ghost_shared.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#ifndef PROCESS_SET_INFORMATION
#define PROCESS_SET_INFORMATION 0x0200
#endif

typedef LONG NTSTATUS_GS;
typedef NTSTATUS_GS (NTAPI *PFN_NtQueryInformationProcess)(
    HANDLE ProcessHandle, ULONG ProcessInformationClass,
    PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength);
typedef BOOL (WINAPI *PFN_IsWow64Process2)(
    HANDLE hProcess, USHORT* pProcessMachine, USHORT* pNativeMachine);
typedef BOOL (WINAPI *PFN_SetProcessValidCallTargets)(
    HANDLE hProcess, PVOID VirtualAddress, SIZE_T RegionSize,
    ULONG NumberOfOffsets, PCFG_CALL_TARGET_INFO OffsetInformation);

/* -------------------------------------------------------------------------- */
/* Globals & Config                                                           */
/* -------------------------------------------------------------------------- */

static FILE* g_logFile = nullptr;
static bool  g_hasConsoleOut = false;
static volatile LONG g_running = 1;
static BOOL  g_quietMode = FALSE;
static BOOL  g_includeShell = FALSE;
static BOOL  g_recreateExisting = FALSE;
static BOOL  g_hwProtected = FALSE;
static BOOL  g_lockVramMode = FALSE;
static BOOL  g_initialVeilOn = FALSE;

static std::atomic<bool> g_veilEnabled{ false };
static std::atomic<int>  g_veilPreset{ 0 };     /* 0=100% pure black, 1=85% dark, 2=65% dim */
static std::atomic<bool> g_veilRenderDirty{ false };
static DWORD g_veilThreadId = 0;

static HANDLE g_hScanWakeEvent = nullptr;
static HANDLE g_hStopEventLocal = nullptr;
static HANDLE g_hStopEventGlobal = nullptr;
static HANDLE g_hSingleInstanceMutex = nullptr;

static PFN_NtQueryInformationProcess  g_pNtQueryInformationProcess = nullptr;
static PFN_IsWow64Process2            g_pIsWow64Process2 = nullptr;
static PFN_SetProcessValidCallTargets g_pSetProcessValidCallTargets = nullptr;

static const float g_veilAlphas[3] = { 1.00f, 0.85f, 0.65f };
static const char* g_veilNames[3]  = { "100% 纯黑防拍", "85% 极暗防窥", "65% 低亮护眼" };

static void Logf(const char* fmt, ...)
{
    if (!g_logFile && !g_hasConsoleOut) return;
    va_list ap;
    va_start(ap, fmt);
    if (g_logFile) {
        vfprintf(g_logFile, fmt, ap);
        fflush(g_logFile);
    } else if (g_hasConsoleOut) {
        vprintf(fmt, ap);
        fflush(stdout);
    }
    va_end(ap);
}

static void AttachCliConsoleIfNeeded()
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD ft = (hOut && hOut != INVALID_HANDLE_VALUE) ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;
    if (ft == FILE_TYPE_PIPE || ft == FILE_TYPE_DISK || ft == FILE_TYPE_CHAR) {
        g_hasConsoleOut = true;
    } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        g_hasConsoleOut = true;
    }
    if (g_hasConsoleOut) {
        SetConsoleOutputCP(CP_UTF8);
        setvbuf(stdout, nullptr, _IONBF, 0);
    }
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        InterlockedExchange(&g_running, 0);
        if (g_hStopEventLocal) SetEvent(g_hStopEventLocal);
        if (g_hStopEventGlobal) SetEvent(g_hStopEventGlobal);
        if (g_hScanWakeEvent) SetEvent(g_hScanWakeEvent);
        if (g_veilThreadId != 0) PostThreadMessageW(g_veilThreadId, WM_QUIT, 0, 0);
        return TRUE;
    }
    return FALSE;
}

template <typename T>
static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static bool InitDynamicApis()
{
    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE hKb = GetModuleHandleW(L"kernelbase.dll");
    if (!hKb) hKb = LoadLibraryW(L"kernelbase.dll");

    if (hNt) {
        g_pNtQueryInformationProcess = (PFN_NtQueryInformationProcess)
            GetProcAddress(hNt, "NtQueryInformationProcess");
    }
    if (hK32) {
        g_pIsWow64Process2 = (PFN_IsWow64Process2)
            GetProcAddress(hK32, "IsWow64Process2");
    }
    if (hKb) {
        g_pSetProcessValidCallTargets = (PFN_SetProcessValidCallTargets)
            GetProcAddress(hKb, "SetProcessValidCallTargets");
    }
    return (g_pNtQueryInformationProcess != nullptr && g_pIsWow64Process2 != nullptr);
}

static bool EnableDebugPrivilege(DWORD* outErr = nullptr)
{
    HANDLE hTok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok)) {
        if (outErr) *outErr = GetLastError();
        return false;
    }
    TOKEN_PRIVILEGES tp = {};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        if (outErr) *outErr = GetLastError();
        CloseHandle(hTok);
        return false;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hTok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(hTok);
    if (outErr) *outErr = err;
    return (err == ERROR_SUCCESS);
}

static void GetStatusFilePath(wchar_t* outBuf, size_t cch)
{
    WCHAR exePath[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    WCHAR* slash = wcsrchr(exePath, L'\\');
    if (slash) *(slash + 1) = L'\0';
    _snwprintf_s(outBuf, cch, _TRUNCATE, L"%lsghostswap_status.txt", exePath);
}

/* -------------------------------------------------------------------------- */
/* Local .gpic Extraction & Dynamic DXGI VTable Discovery                     */
/* -------------------------------------------------------------------------- */

struct HookSlotSpec {
    ULONG_PTR   vtableRva;
    UINT        slotIndex;
    GhostHookId hookId;
    ULONG_PTR   origFnRva;
    const char* name;
};

struct LocalEngineInfo {
    ULONG_PTR localDxgiBase = 0;
    DWORD     localDxgiSize = 0;
    std::vector<BYTE> gpicBytes;
    SIZE_T    gpicAllocSize = 0;
    DWORD     hookOffsets[GHOOK_COUNT] = {};
    std::vector<HookSlotSpec> slots;
    std::vector<SIZE_T> patchSentinelOffsets;
};

static LocalEngineInfo g_engine;

static void AddSlotUnique(std::vector<HookSlotSpec>& out, ULONG_PTR vtblRva, UINT slotIdx,
    GhostHookId id, void* fnPtr, ULONG_PTR dxgiBase, DWORD dxgiSize, const char* name)
{
    ULONG_PTR fn = (ULONG_PTR)fnPtr;
    if (fn < dxgiBase || fn >= dxgiBase + dxgiSize) return;
    for (const auto& s : out) {
        if (s.vtableRva == vtblRva && s.slotIndex == slotIdx) return;
    }
    HookSlotSpec spec;
    spec.vtableRva = vtblRva;
    spec.slotIndex = slotIdx;
    spec.hookId = id;
    spec.origFnRva = fn - dxgiBase;
    spec.name = name;
    out.push_back(spec);
}

static bool DiscoverLocalDxgiAndGpic(bool verbose)
{
    HMODULE hSelf = GetModuleHandleW(nullptr);
    const BYTE* base = (const BYTE*)hSelf;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const IMAGE_SECTION_HEADER* gpicSec = nullptr;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".gpic", 5) == 0 && (sec[i].Name[5] == '\0')) {
            gpicSec = &sec[i];
            break;
        }
    }
    if (!gpicSec || gpicSec->Misc.VirtualSize == 0) {
        Logf("[FATAL] .gpic section not found\n");
        return false;
    }

    const BYTE* gpicStart = base + gpicSec->VirtualAddress;
    DWORD gpicLen = gpicSec->Misc.VirtualSize;
    g_engine.gpicBytes.assign(gpicStart, gpicStart + gpicLen);
    g_engine.gpicAllocSize = (gpicLen + 0xFFFu) & ~((SIZE_T)0xFFFu);

    void* hookAddrs[GHOOK_COUNT] = {
        (void*)&GhostHook_CreateSwapChain,
        (void*)&GhostHook_CreateSwapChainForHwnd,
        (void*)&GhostHook_CreateSwapChainForCoreWindow,
        (void*)&GhostHook_CreateSwapChainForComposition,
        (void*)&GhostHook_CreateSwapChainForCompSurface,
        (void*)&GhostHook_ResizeBuffers,
        (void*)&GhostHook_ResizeBuffers1,
        (void*)&GhostHook_Present,
        (void*)&GhostHook_Present1
    };

    for (int i = 0; i < GHOOK_COUNT; i++) {
        const BYTE* p = (const BYTE*)hookAddrs[i];
        if (p < gpicStart || p >= gpicStart + gpicLen) {
            Logf("[FATAL] Hook[%d] outside .gpic\n", i);
            return false;
        }
        DWORD off = (DWORD)(p - gpicStart);
        if ((off & 0xFu) != 0) {
            Logf("[FATAL] Hook[%d] offset 0x%lX NOT 16-byte aligned\n", i, off);
            return false;
        }
        g_engine.hookOffsets[i] = off;
    }

    g_engine.patchSentinelOffsets.clear();
    const UINT64 sentinel = GHOST_STUB_DATA_SENTINEL;
    for (SIZE_T i = 0; i + sizeof(UINT64) <= g_engine.gpicBytes.size(); i++) {
        if (*(const UINT64*)(g_engine.gpicBytes.data() + i) == sentinel) {
            g_engine.patchSentinelOffsets.push_back(i);
        }
    }
    if (g_engine.patchSentinelOffsets.size() < GHOOK_COUNT) {
        Logf("[FATAL] Found only %zu sentinel offsets in .gpic\n", g_engine.patchSentinelOffsets.size());
        return false;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hSelf;
    wc.lpszClassName = L"GhostSwapDiscWndV2";
    RegisterClassW(&wc);
    HWND hDummy = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, hSelf, nullptr);

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr) || !dev) {
        Logf("[FATAL] Local D3D11CreateDevice failed hr=0x%08lX\n", hr);
        if (hDummy) DestroyWindow(hDummy);
        return false;
    }

    HMODULE hDxgi = GetModuleHandleW(L"dxgi.dll");
    MODULEINFO mi = {};
    if (!hDxgi || !GetModuleInformation(GetCurrentProcess(), hDxgi, &mi, sizeof(mi))) {
        ctx->Release(); dev->Release();
        if (hDummy) DestroyWindow(hDummy);
        return false;
    }
    g_engine.localDxgiBase = (ULONG_PTR)mi.lpBaseOfDll;
    g_engine.localDxgiSize = mi.SizeOfImage;
    g_engine.slots.clear();

    auto inspectFactory = [&](IDXGIFactory* pf) {
        if (!pf) return;
        void** v = *(void***)pf;
        ULONG_PTR vAddr = (ULONG_PTR)v;
        if (vAddr >= g_engine.localDxgiBase && vAddr < g_engine.localDxgiBase + g_engine.localDxgiSize) {
            ULONG_PTR vRva = vAddr - g_engine.localDxgiBase;
            AddSlotUnique(g_engine.slots, vRva, 10, GHOOK_CREATE_SWAPCHAIN, v[10],
                g_engine.localDxgiBase, g_engine.localDxgiSize, "CreateSwapChain");
            IDXGIFactory2* pf2 = nullptr;
            if (SUCCEEDED(pf->QueryInterface(IID_PPV_ARGS(&pf2))) && pf2) {
                void** v2 = *(void***)pf2;
                ULONG_PTR v2Addr = (ULONG_PTR)v2;
                if (v2Addr >= g_engine.localDxgiBase && v2Addr < g_engine.localDxgiBase + g_engine.localDxgiSize) {
                    ULONG_PTR v2Rva = v2Addr - g_engine.localDxgiBase;
                    AddSlotUnique(g_engine.slots, v2Rva, 15, GHOOK_CREATE_FOR_HWND, v2[15],
                        g_engine.localDxgiBase, g_engine.localDxgiSize, "CreateSwapChainForHwnd");
                    AddSlotUnique(g_engine.slots, v2Rva, 16, GHOOK_CREATE_FOR_COREWINDOW, v2[16],
                        g_engine.localDxgiBase, g_engine.localDxgiSize, "CreateSwapChainForCoreWindow");
                    AddSlotUnique(g_engine.slots, v2Rva, 24, GHOOK_CREATE_FOR_COMPOSITION, v2[24],
                        g_engine.localDxgiBase, g_engine.localDxgiSize, "CreateSwapChainForComposition");
                }
                pf2->Release();
            }
            IDXGIFactoryMedia* pfm = nullptr;
            if (SUCCEEDED(pf->QueryInterface(IID_PPV_ARGS(&pfm))) && pfm) {
                void** vm = *(void***)pfm;
                ULONG_PTR vmAddr = (ULONG_PTR)vm;
                if (vmAddr >= g_engine.localDxgiBase && vmAddr < g_engine.localDxgiBase + g_engine.localDxgiSize) {
                    ULONG_PTR vmRva = vmAddr - g_engine.localDxgiBase;
                    AddSlotUnique(g_engine.slots, vmRva, 3, GHOOK_CREATE_FOR_COMP_SURFACE, vm[3],
                        g_engine.localDxgiBase, g_engine.localDxgiSize, "CreateSwapChainForCompSurface");
                }
                pfm->Release();
            }
        }
    };

    IDXGIFactory* f0 = nullptr; IDXGIFactory1* f1 = nullptr; IDXGIFactory2* f2 = nullptr; IDXGIFactory2* fDev = nullptr;
    CreateDXGIFactory(IID_PPV_ARGS(&f0));
    CreateDXGIFactory1(IID_PPV_ARGS(&f1));
    CreateDXGIFactory2(0, IID_PPV_ARGS(&f2));
    IDXGIDevice* dd = nullptr; IDXGIAdapter* adp = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dd))) && dd) {
        if (SUCCEEDED(dd->GetAdapter(&adp)) && adp) adp->GetParent(IID_PPV_ARGS(&fDev));
    }
    inspectFactory(f0); inspectFactory(f1); inspectFactory(f2); inspectFactory(fDev);

    auto inspectSC = [&](IDXGISwapChain* psc) {
        if (!psc) return;
        IDXGISwapChain3* psc3 = nullptr;
        void** v = *(void***)psc;
        if (SUCCEEDED(psc->QueryInterface(IID_PPV_ARGS(&psc3))) && psc3) v = *(void***)psc3;
        ULONG_PTR vAddr = (ULONG_PTR)v;
        if (vAddr >= g_engine.localDxgiBase && vAddr < g_engine.localDxgiBase + g_engine.localDxgiSize) {
            ULONG_PTR vRva = vAddr - g_engine.localDxgiBase;
            AddSlotUnique(g_engine.slots, vRva, 8, GHOOK_PRESENT, v[8],
                g_engine.localDxgiBase, g_engine.localDxgiSize, "Present");
            AddSlotUnique(g_engine.slots, vRva, 13, GHOOK_RESIZE_BUFFERS, v[13],
                g_engine.localDxgiBase, g_engine.localDxgiSize, "ResizeBuffers");
            AddSlotUnique(g_engine.slots, vRva, 22, GHOOK_PRESENT1, v[22],
                g_engine.localDxgiBase, g_engine.localDxgiSize, "Present1");
            AddSlotUnique(g_engine.slots, vRva, 39, GHOOK_RESIZE_BUFFERS1, v[39],
                g_engine.localDxgiBase, g_engine.localDxgiSize, "ResizeBuffers1");
        }
        if (psc3) psc3->Release();
    };

    if (fDev && hDummy) {
        DXGI_SWAP_CHAIN_DESC1 d = { 64,64,DXGI_FORMAT_B8G8R8A8_UNORM,FALSE,{1,0},DXGI_USAGE_RENDER_TARGET_OUTPUT,2,DXGI_SCALING_STRETCH,DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,DXGI_ALPHA_MODE_PREMULTIPLIED,0 };
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(fDev->CreateSwapChainForComposition(dev, &d, nullptr, &sc1)) && sc1) { inspectSC(sc1); sc1->Release(); }
        d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        if (SUCCEEDED(fDev->CreateSwapChainForHwnd(dev, hDummy, &d, nullptr, nullptr, &sc1)) && sc1) { inspectSC(sc1); sc1->Release(); }
    }

    SafeRelease(adp); SafeRelease(dd); SafeRelease(fDev); SafeRelease(f2); SafeRelease(f1); SafeRelease(f0);
    SafeRelease(ctx); SafeRelease(dev);
    if (hDummy) DestroyWindow(hDummy);
    UnregisterClassW(wc.lpszClassName, hSelf);

    if (verbose) {
        Logf("[ENGINE] .gpic size=%lu alloc=%zu sentinels=%zu slots=%zu mode=%s dxgi=0x%llX(+0x%lX)\n",
            gpicLen, g_engine.gpicAllocSize, g_engine.patchSentinelOffsets.size(), g_engine.slots.size(),
            g_hwProtected ? "0x420(HW_PROT)" : "0x20(DISPLAY_ONLY)",
            (unsigned long long)g_engine.localDxgiBase, g_engine.localDxgiSize);
    }
    return !g_engine.slots.empty();
}

/* -------------------------------------------------------------------------- */
/* Process Eligibility, Fileless PIC Attach & Clean Restore                   */
/* -------------------------------------------------------------------------- */

static bool IsSafetyExcludedExe(const wchar_t* exeName)
{
    if (!exeName || !exeName[0]) return true;
    static const wchar_t* const kSysCritical[] = {
        L"dwm.exe", L"csrss.exe", L"winlogon.exe", L"wininit.exe", L"smss.exe",
        L"services.exe", L"lsass.exe", L"fontdrvhost.exe", L"LogonUI.exe",
        L"sihost.exe", L"svchost.exe", L"RuntimeBroker.exe", L"dllhost.exe",
        L"conhost.exe", L"ApplicationFrameHost.exe", L"audiodg.exe",
        L"MsMpEng.exe", L"SecurityHealthService.exe", L"SecurityHealthSystray.exe",
        L"smartscreen.exe", L"CHXSmartScreen.exe", L"SystemSettings.exe"
    };
    for (const wchar_t* s : kSysCritical) if (_wcsicmp(exeName, s) == 0) return true;

    if (!g_includeShell) {
        static const wchar_t* const kShellExcl[] = {
            L"explorer.exe", L"StartMenuExperienceHost.exe", L"ShellExperienceHost.exe",
            L"ShellHost.exe", L"SearchHost.exe", L"TextInputHost.exe",
            L"LockApp.exe", L"ShellAppRuntime.exe"
        };
        for (const wchar_t* s : kShellExcl) if (_wcsicmp(exeName, s) == 0) return true;
    }

    static const wchar_t* const kSelfTools[] = {
        L"GhostSwap.exe", L"GhostSwap_v2.exe", L"ApexShield.exe", L"ApexShield_v4.exe",
        L"ApexShieldSvc.exe", L"VramVault.exe", L"AegisBlackout.exe", L"AegisBlackoutSvc.exe",
        L"AegisVault.exe", L"CaptureAuditSuite.exe", L"SbieSvc.exe", L"SbieCtrl.exe", L"SandMan.exe"
    };
    for (const wchar_t* s : kSelfTools) if (_wcsicmp(exeName, s) == 0) return true;
    if (_wcsnicmp(exeName, L"Sandboxie", 9) == 0) return true;

    static const wchar_t* const kAntiCheatPrefixes[] = {
        L"BEService", L"BEDaisy", L"EasyAntiCheat", L"vgc", L"vgtray",
        L"ACE-", L"TenProtect", L"SGuard", L"WeGame", L"RiotClient",
        L"VALORANT", L"LeagueClient", L"GameBar"
    };
    for (const wchar_t* p : kAntiCheatPrefixes) if (_wcsnicmp(exeName, p, wcslen(p)) == 0) return true;
    return false;
}

struct ProcInspectResult {
    bool      eligible = false;
    DWORD     pid = 0;
    std::wstring exeName;
    ULONG_PTR dxgiBase = 0;
    DWORD     dxgiSize = 0;
    BYTE      prot = 0;
    DWORD     acg = 0;
    DWORD     cig = 0;
    DWORD     cfg = 0;
    std::string reason;
};

static ProcInspectResult InspectProcessEligibility(DWORD pid, const wchar_t* exeName, bool enforceBlacklist)
{
    ProcInspectResult r;
    r.pid = pid;
    r.exeName = exeName ? exeName : L"?";

    if (pid <= 4 || pid == GetCurrentProcessId()) { r.reason = "self/system"; return r; }
    if (enforceBlacklist && IsSafetyExcludedExe(r.exeName.c_str())) { r.reason = "safety-blacklist"; return r; }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        char buf[48]; sprintf_s(buf, sizeof(buf), "open-fail(%lu)", GetLastError());
        r.reason = buf;
        return r;
    }

    USHORT procMach = 0, nativeMach = 0;
    if (!g_pIsWow64Process2 || !g_pIsWow64Process2(hProc, &procMach, &nativeMach) ||
        procMach != IMAGE_FILE_MACHINE_UNKNOWN) {
        r.reason = "wow64-32bit"; CloseHandle(hProc); return r;
    }

    ULONG retLen = 0; BYTE prot = 0;
    if (g_pNtQueryInformationProcess(hProc, 61, &prot, 1, &retLen) == 0) {
        r.prot = prot;
        if (prot != 0) { r.reason = "protected-ppl"; CloseHandle(hProc); return r; }
    }

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dcp = {};
    if (GetProcessMitigationPolicy(hProc, ProcessDynamicCodePolicy, &dcp, sizeof(dcp))) {
        r.acg = dcp.ProhibitDynamicCode;
        if (r.acg) { r.reason = "acg-enabled"; CloseHandle(hProc); return r; }
    }

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY bsp = {};
    if (GetProcessMitigationPolicy(hProc, ProcessSignaturePolicy, &bsp, sizeof(bsp))) r.cig = bsp.Flags;
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfgp = {};
    if (GetProcessMitigationPolicy(hProc, ProcessControlFlowGuardPolicy, &cfgp, sizeof(cfgp))) r.cfg = cfgp.Flags;

    HMODULE hMods[1024];
    DWORD cbNeeded = 0;
    if (EnumProcessModulesEx(hProc, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_64BIT)) {
        DWORD cnt = std::min<DWORD>(cbNeeded / sizeof(HMODULE), 1024);
        for (DWORD i = 0; i < cnt; i++) {
            WCHAR modName[MAX_PATH] = L"";
            if (GetModuleBaseNameW(hProc, hMods[i], modName, MAX_PATH) && _wcsicmp(modName, L"dxgi.dll") == 0) {
                MODULEINFO mi = {};
                if (GetModuleInformation(hProc, hMods[i], &mi, sizeof(mi))) {
                    r.dxgiBase = (ULONG_PTR)mi.lpBaseOfDll;
                    r.dxgiSize = mi.SizeOfImage;
                }
                break;
            }
        }
    }
    CloseHandle(hProc);

    if (r.dxgiBase == 0) { r.reason = "no-dxgi"; return r; }
    r.eligible = true;
    r.reason = "eligible";
    return r;
}

struct PatchedSlotRecord { ULONG_PTR slotAddr, origVal, newVal; };

struct AttachedTarget {
    DWORD        pid = 0;
    std::wstring exeName;
    HANDLE       hProc = nullptr;
    ULONG_PTR    remoteCode = 0;
    ULONG_PTR    remoteData = 0;
    SIZE_T       codeAllocSize = 0;
    DWORD        cig = 0;
    DWORD        cfg = 0;
    bool         cfgRegistered = false;
    bool         detached = false;
    GhostStubData lastTele = {};
    std::vector<PatchedSlotRecord> patchedSlots;
};

static std::vector<AttachedTarget> g_attached;
static std::map<DWORD, int> g_retryMap;

static bool IsPidTracked(DWORD pid)
{
    for (const auto& t : g_attached) if (t.pid == pid && !t.detached) return true;
    return false;
}

static bool AttachToTarget(const ProcInspectResult& info)
{
    if (!info.eligible) return false;
    if (IsPidTracked(info.pid)) return true;

    HANDLE hProc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
        PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION,
        FALSE, info.pid);
    if (!hProc) return false;

    struct PendingPatch { ULONG_PTR slotAddr, origVal; GhostHookId hookId; };
    std::vector<PendingPatch> pending;
    ULONG_PTR remoteOrigPtrs[GHOOK_COUNT] = {};

    for (const auto& s : g_engine.slots) {
        ULONG_PTR slotAddr = info.dxgiBase + s.vtableRva + (ULONG_PTR)s.slotIndex * sizeof(ULONG_PTR);
        ULONG_PTR curVal = 0; SIZE_T rd = 0;
        if (!ReadProcessMemory(hProc, (LPCVOID)slotAddr, &curVal, sizeof(curVal), &rd) || rd != sizeof(curVal)) continue;
        if (curVal < info.dxgiBase || curVal >= info.dxgiBase + info.dxgiSize) continue;
        if (remoteOrigPtrs[s.hookId] == 0) remoteOrigPtrs[s.hookId] = curVal;
        pending.push_back({ slotAddr, curVal, s.hookId });
    }
    if (pending.empty()) { CloseHandle(hProc); return false; }

    LPVOID pCode = VirtualAllocEx(hProc, nullptr, g_engine.gpicAllocSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    LPVOID pData = VirtualAllocEx(hProc, nullptr, sizeof(GhostStubData), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pCode || !pData) {
        if (pCode) VirtualFreeEx(hProc, pCode, 0, MEM_RELEASE);
        if (pData) VirtualFreeEx(hProc, pData, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    GhostStubData data = {};
    data.magic = GHOSTSWAP_MAGIC;
    data.version = GHOSTSWAP_VERSION;
    data.enabled = 1;
    data.stripFlagsMask = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    data.upgradeFlagsPrimary  = g_hwProtected ? 0x420u : 0x20u;
    data.upgradeFlagsFallback = g_hwProtected ? 0x400u : 0x20u;
    data.forceResetOnceForExisting = g_recreateExisting ? 1u : 0u;
    data.maxResetInjections = 8u;

    data.origCreateSwapChain = (void*)remoteOrigPtrs[GHOOK_CREATE_SWAPCHAIN];
    data.origCreateForHwnd = (void*)remoteOrigPtrs[GHOOK_CREATE_FOR_HWND];
    data.origCreateForCoreWindow = (void*)remoteOrigPtrs[GHOOK_CREATE_FOR_COREWINDOW];
    data.origCreateForComposition = (void*)remoteOrigPtrs[GHOOK_CREATE_FOR_COMPOSITION];
    data.origCreateForCompSurface = (void*)remoteOrigPtrs[GHOOK_CREATE_FOR_COMP_SURFACE];
    data.origResizeBuffers = (void*)remoteOrigPtrs[GHOOK_RESIZE_BUFFERS];
    data.origResizeBuffers1 = (void*)remoteOrigPtrs[GHOOK_RESIZE_BUFFERS1];
    data.origPresent = (void*)remoteOrigPtrs[GHOOK_PRESENT];
    data.origPresent1 = (void*)remoteOrigPtrs[GHOOK_PRESENT1];
    data.iidD3D11Texture2D = __uuidof(ID3D11Texture2D);
    data.iidDXGISwapChain1 = __uuidof(IDXGISwapChain1);

    SIZE_T wr = 0;
    if (!WriteProcessMemory(hProc, pData, &data, sizeof(data), &wr) || wr != sizeof(data)) {
        VirtualFreeEx(hProc, pCode, 0, MEM_RELEASE); VirtualFreeEx(hProc, pData, 0, MEM_RELEASE); CloseHandle(hProc);
        return false;
    }

    std::vector<BYTE> codeBuf = g_engine.gpicBytes;
    UINT64 targetDataAddr = (UINT64)(ULONG_PTR)pData;
    for (SIZE_T off : g_engine.patchSentinelOffsets) *(UINT64*)(codeBuf.data() + off) = targetDataAddr;

    if (!WriteProcessMemory(hProc, pCode, codeBuf.data(), codeBuf.size(), &wr) || wr != codeBuf.size()) {
        VirtualFreeEx(hProc, pCode, 0, MEM_RELEASE); VirtualFreeEx(hProc, pData, 0, MEM_RELEASE); CloseHandle(hProc);
        return false;
    }

    DWORD oldProt = 0;
    if (!VirtualProtectEx(hProc, pCode, g_engine.gpicAllocSize, PAGE_EXECUTE_READ, &oldProt)) {
        VirtualFreeEx(hProc, pCode, 0, MEM_RELEASE); VirtualFreeEx(hProc, pData, 0, MEM_RELEASE); CloseHandle(hProc);
        return false;
    }
    FlushInstructionCache(hProc, pCode, g_engine.gpicAllocSize);

    bool cfgRegOk = false;
    if (g_pSetProcessValidCallTargets) {
        CFG_CALL_TARGET_INFO cfgTargets[GHOOK_COUNT] = {};
        for (int i = 0; i < GHOOK_COUNT; i++) {
            cfgTargets[i].Offset = (ULONG_PTR)g_engine.hookOffsets[i];
            cfgTargets[i].Flags = CFG_CALL_TARGET_VALID;
        }
        BOOL bCfg = g_pSetProcessValidCallTargets(hProc, pCode, g_engine.gpicAllocSize, GHOOK_COUNT, cfgTargets);
        DWORD cfgErr = bCfg ? 0 : GetLastError();
        cfgRegOk = (bCfg != FALSE);
        if ((info.cfg & 1u) != 0 && !cfgRegOk) {
            Logf("[ATTACH-ABORT] PID=%lu (%ls) CFG=0x%lX SetProcessValidCallTargets err=%lu\n",
                info.pid, info.exeName.c_str(), info.cfg, cfgErr);
            VirtualFreeEx(hProc, pCode, 0, MEM_RELEASE); VirtualFreeEx(hProc, pData, 0, MEM_RELEASE); CloseHandle(hProc);
            return false;
        }
    }

    AttachedTarget rec;
    rec.pid = info.pid;
    rec.exeName = info.exeName;
    rec.hProc = hProc;
    rec.remoteCode = (ULONG_PTR)pCode;
    rec.remoteData = (ULONG_PTR)pData;
    rec.codeAllocSize = g_engine.gpicAllocSize;
    rec.cig = info.cig;
    rec.cfg = info.cfg;
    rec.cfgRegistered = cfgRegOk;
    rec.lastTele = data;

    for (const auto& pp : pending) {
        ULONG_PTR hookAddr = (ULONG_PTR)pCode + g_engine.hookOffsets[pp.hookId];
        DWORD oldVProt = 0;
        if (VirtualProtectEx(hProc, (LPVOID)pp.slotAddr, sizeof(ULONG_PTR), PAGE_READWRITE, &oldVProt)) {
            SIZE_T wv = 0;
            BOOL okW = WriteProcessMemory(hProc, (LPVOID)pp.slotAddr, &hookAddr, sizeof(hookAddr), &wv);
            DWORD dummy = 0;
            VirtualProtectEx(hProc, (LPVOID)pp.slotAddr, sizeof(ULONG_PTR), oldVProt, &dummy);
            if (okW && wv == sizeof(hookAddr)) rec.patchedSlots.push_back({ pp.slotAddr, pp.origVal, hookAddr });
        }
    }

    if (rec.patchedSlots.empty()) {
        VirtualFreeEx(hProc, pCode, 0, MEM_RELEASE); VirtualFreeEx(hProc, pData, 0, MEM_RELEASE); CloseHandle(hProc);
        return false;
    }

    g_attached.push_back(std::move(rec));
    if (!g_quietMode) {
        Logf("[ATTACHED] PID=%lu %-20ls slots=%zu CIG=0x%lX CFG=0x%lX(reg=%d) code=0x%llX data=0x%llX\n",
            info.pid, info.exeName.c_str(), g_attached.back().patchedSlots.size(),
            info.cig, info.cfg, cfgRegOk ? 1 : 0,
            (unsigned long long)(ULONG_PTR)pCode, (unsigned long long)(ULONG_PTR)pData);
    }
    return true;
}

static void DetachTarget(AttachedTarget& t)
{
    if (t.detached || !t.hProc) return;
    t.detached = true;

    DWORD exitCode = 0;
    if (GetExitCodeProcess(t.hProc, &exitCode) && exitCode == STILL_ACTIVE) {
        LONG zero = 0; SIZE_T wr = 0;
        WriteProcessMemory(t.hProc, (LPVOID)(t.remoteData + offsetof(GhostStubData, enabled)), &zero, sizeof(zero), &wr);
        for (const auto& ps : t.patchedSlots) {
            ULONG_PTR cur = 0; SIZE_T rd = 0;
            if (ReadProcessMemory(t.hProc, (LPCVOID)ps.slotAddr, &cur, sizeof(cur), &rd) &&
                rd == sizeof(cur) && cur == ps.newVal) {
                DWORD oldP = 0;
                if (VirtualProtectEx(t.hProc, (LPVOID)ps.slotAddr, sizeof(ULONG_PTR), PAGE_READWRITE, &oldP)) {
                    WriteProcessMemory(t.hProc, (LPVOID)ps.slotAddr, &ps.origVal, sizeof(ps.origVal), &wr);
                    DWORD dummy = 0;
                    VirtualProtectEx(t.hProc, (LPVOID)ps.slotAddr, sizeof(ULONG_PTR), oldP, &dummy);
                }
            }
        }
        SIZE_T rd = 0;
        ReadProcessMemory(t.hProc, (LPCVOID)t.remoteData, &t.lastTele, sizeof(GhostStubData), &rd);
    }
    CloseHandle(t.hProc);
    t.hProc = nullptr;
}

static void DetachAllTargets()
{
    for (auto& t : g_attached) DetachTarget(t);
}

static void PollAllTelemetry()
{
    for (auto& t : g_attached) {
        if (t.detached || !t.hProc) continue;
        DWORD ec = 0;
        if (!GetExitCodeProcess(t.hProc, &ec) || ec != STILL_ACTIVE) {
            t.detached = true;
            CloseHandle(t.hProc);
            t.hProc = nullptr;
            continue;
        }
        SIZE_T rd = 0;
        ReadProcessMemory(t.hProc, (LPCVOID)t.remoteData, &t.lastTele, sizeof(GhostStubData), &rd);
    }
}

static void ScanAndAttachEligible()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            DWORD pid = pe.th32ProcessID;
            if (IsPidTracked(pid)) continue;
            if (pid <= 4 || pid == GetCurrentProcessId()) continue;
            if (IsSafetyExcludedExe(pe.szExeFile)) continue;

            auto it = g_retryMap.find(pid);
            if (it != g_retryMap.end() && it->second >= 12) continue;

            ProcInspectResult info = InspectProcessEligibility(pid, pe.szExeFile, true);
            if (info.eligible) {
                if (AttachToTarget(info)) {
                    g_retryMap.erase(pid);
                } else {
                    g_retryMap[pid]++;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

/* -------------------------------------------------------------------------- */
/* Module B: 0%-Flicker 0%-Idle-GPU Instant Privacy Blackout Veil (0x20 DComp) */
/* -------------------------------------------------------------------------- */

struct VeilEngine {
    HWND                    hwnd = nullptr;
    ID3D11Device*           dev = nullptr;
    ID3D11DeviceContext*    ctx = nullptr;
    IDXGISwapChain1*        sc = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    IDCompositionDevice*    dcompDev = nullptr;
    IDCompositionTarget*    dcompTgt = nullptr;
    IDCompositionVisual*    dcompVis = nullptr;
    int vx = 0, vy = 0, vw = 0, vh = 0;
    UINT actualFlags = 0;
    UINT backBufMisc = 0;
    DWORD wda = 0;
    bool initialized = false;
};

static VeilEngine g_veil;
static ULONGLONG  g_lastHkTick[4] = {};

static void TriggerHotkeyAction(int actionId)
{
    ULONGLONG now = GetTickCount64();
    if (actionId >= 1 && actionId <= 3) {
        if (now - g_lastHkTick[actionId] < 240) return;
        g_lastHkTick[actionId] = now;
    }
    if (actionId == 1) {
        bool next = !g_veilEnabled.load();
        g_veilEnabled.store(next);
        g_veilRenderDirty.store(true);
    } else if (actionId == 2) {
        /* Ctrl+Alt+F10: quick toggle between 100% black (0) and 85% dark (1) */
        int cur = g_veilPreset.load();
        g_veilPreset.store(cur == 0 ? 1 : 0);
        g_veilRenderDirty.store(true);
    } else if (actionId == 3) {
        /* Ctrl+Alt+F11: cycle 100% -> 85% -> 65% */
        int next = (g_veilPreset.load() + 1) % 3;
        g_veilPreset.store(next);
        g_veilRenderDirty.store(true);
    }
}

static bool g_rawLC = false, g_rawRC = false, g_rawLA = false, g_rawRA = false, g_rawLS = false, g_rawRS = false;

static void HandleVeilRawInput(LPARAM lParam)
{
    UINT sz = 0;
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER)) != 0 || sz == 0) return;
    std::vector<BYTE> buf(sz);
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf.data(), &sz, sizeof(RAWINPUTHEADER)) != sz) return;
    const RAWINPUT* ri = (const RAWINPUT*)buf.data();
    if (ri->header.dwType != RIM_TYPEKEYBOARD || ri->header.hDevice == nullptr) return;

    const RAWKEYBOARD& kb = ri->data.keyboard;
    bool isBreak = (kb.Flags & RI_KEY_BREAK) != 0;
    bool isE0 = (kb.Flags & RI_KEY_E0) != 0;

    if (kb.VKey == VK_CONTROL) { if (isE0) g_rawRC = !isBreak; else g_rawLC = !isBreak; }
    else if (kb.VKey == VK_LCONTROL) g_rawLC = !isBreak;
    else if (kb.VKey == VK_RCONTROL) g_rawRC = !isBreak;
    else if (kb.VKey == VK_MENU) { if (isE0) g_rawRA = !isBreak; else g_rawLA = !isBreak; }
    else if (kb.VKey == VK_LMENU) g_rawLA = !isBreak;
    else if (kb.VKey == VK_RMENU) g_rawRA = !isBreak;
    else if (kb.VKey == VK_SHIFT) { if (kb.MakeCode == 0x36) g_rawRS = !isBreak; else g_rawLS = !isBreak; }
    else if (!isBreak) {
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) g_rawLC = g_rawRC = false;
        if ((GetAsyncKeyState(VK_MENU) & 0x8000) == 0) g_rawLA = g_rawRA = false;
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) g_rawLS = g_rawRS = false;

        bool ctrl = (g_rawLC || g_rawRC) && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
        bool alt  = (g_rawLA || g_rawRA) && ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
        bool sh   = (g_rawLS || g_rawRS) && ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
        if (ctrl && alt) {
            if (!sh && kb.VKey == VK_F9) TriggerHotkeyAction(1);
            else if (sh && kb.VKey == 'T') TriggerHotkeyAction(1);
            else if (!sh && kb.VKey == VK_F10) TriggerHotkeyAction(2);
            else if (!sh && kb.VKey == VK_F11) TriggerHotkeyAction(3);
        }
    }
}

static LRESULT CALLBACK VeilWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_HOTKEY:
        if (wParam == 1 || wParam == 4) TriggerHotkeyAction(1);
        else if (wParam == 2) TriggerHotkeyAction(2);
        else if (wParam == 3) TriggerHotkeyAction(3);
        return 0;
    case WM_INPUT:
        HandleVeilRawInput(lParam);
        return 0;
    case WM_DISPLAYCHANGE:
        g_veilRenderDirty.store(true);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void CALLBACK WinEventShowProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (event != EVENT_OBJECT_SHOW || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (!hwnd || !IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    if (g_hScanWakeEvent) SetEvent(g_hScanWakeEvent);
}

static void ReleaseVeilEngine()
{
    SafeRelease(g_veil.dcompVis);
    SafeRelease(g_veil.dcompTgt);
    SafeRelease(g_veil.dcompDev);
    SafeRelease(g_veil.rtv);
    SafeRelease(g_veil.sc);
    SafeRelease(g_veil.ctx);
    SafeRelease(g_veil.dev);
    if (g_veil.hwnd) { DestroyWindow(g_veil.hwnd); g_veil.hwnd = nullptr; }
    g_veil.initialized = false;
}

static bool InitVeilEngine(bool showInitially)
{
    g_veil.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_veil.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_veil.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_veil.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_veil.vw <= 0 || g_veil.vh <= 0) {
        g_veil.vx = 0; g_veil.vy = 0;
        g_veil.vw = GetSystemMetrics(SM_CXSCREEN);
        g_veil.vh = GetSystemMetrics(SM_CYSCREEN);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VeilWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GhostSwapBlackoutVeilWnd";
    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED |
                    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
    g_veil.hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"GhostSwapVeil",
        WS_POPUP, g_veil.vx, g_veil.vy, g_veil.vw, g_veil.vh,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_veil.hwnd) return false;

    SetWindowDisplayAffinity(g_veil.hwnd, WDA_EXCLUDEFROMCAPTURE);
    GetWindowDisplayAffinity(g_veil.hwnd, &g_veil.wda);

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &g_veil.dev, &fl, &g_veil.ctx);
    if (FAILED(hr) || !g_veil.dev) { ReleaseVeilEngine(); return false; }

    IDXGIDevice* dd = nullptr; IDXGIAdapter* adp = nullptr; IDXGIFactory2* fac2 = nullptr;
    hr = g_veil.dev->QueryInterface(IID_PPV_ARGS(&dd));
    if (SUCCEEDED(hr) && dd) hr = dd->GetAdapter(&adp);
    if (SUCCEEDED(hr) && adp) hr = adp->GetParent(IID_PPV_ARGS(&fac2));

    if (SUCCEEDED(hr) && fac2) {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = (UINT)g_veil.vw; d.Height = (UINT)g_veil.vh;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        const UINT flagsTry[] = { 0x20u, 0x0u };
        for (UINT f : flagsTry) {
            d.Flags = f;
            hr = fac2->CreateSwapChainForComposition(g_veil.dev, &d, nullptr, &g_veil.sc);
            if (SUCCEEDED(hr) && g_veil.sc) break;
        }
    }

    if (g_veil.sc) {
        DXGI_SWAP_CHAIN_DESC1 actual = {};
        g_veil.sc->GetDesc1(&actual);
        g_veil.actualFlags = actual.Flags;

        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(g_veil.sc->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
            D3D11_TEXTURE2D_DESC td = {}; bb->GetDesc(&td);
            g_veil.backBufMisc = td.MiscFlags;
            hr = g_veil.dev->CreateRenderTargetView(bb, nullptr, &g_veil.rtv);
            bb->Release();
        }
    }

    if (SUCCEEDED(hr) && dd && g_veil.rtv) {
        hr = DCompositionCreateDevice(dd, IID_PPV_ARGS(&g_veil.dcompDev));
        if (SUCCEEDED(hr) && g_veil.dcompDev) hr = g_veil.dcompDev->CreateTargetForHwnd(g_veil.hwnd, TRUE, &g_veil.dcompTgt);
        if (SUCCEEDED(hr) && g_veil.dcompDev) hr = g_veil.dcompDev->CreateVisual(&g_veil.dcompVis);
        if (SUCCEEDED(hr) && g_veil.dcompVis) hr = g_veil.dcompVis->SetContent(g_veil.sc);
        if (SUCCEEDED(hr) && g_veil.dcompTgt) hr = g_veil.dcompTgt->SetRoot(g_veil.dcompVis);
        if (SUCCEEDED(hr)) {
            const float rgba[4] = { 0.0f, 0.0f, 0.0f, showInitially ? g_veilAlphas[g_veilPreset.load()] : 0.0f };
            g_veil.ctx->ClearRenderTargetView(g_veil.rtv, rgba);
            g_veil.sc->Present(0, 0);
            hr = g_veil.dcompDev->Commit();
        }
    }

    SafeRelease(fac2); SafeRelease(adp); SafeRelease(dd);
    if (FAILED(hr) || !g_veil.rtv || !g_veil.dcompDev) { ReleaseVeilEngine(); return false; }

    g_veil.initialized = true;
    ShowWindow(g_veil.hwnd, showInitially ? SW_SHOWNOACTIVATE : SW_HIDE);
    return true;
}

static HRESULT RenderSolidVeilFrame(float alpha)
{
    if (!g_veil.initialized) return E_FAIL;
    const float rgba[4] = { 0.0f, 0.0f, 0.0f, alpha };
    g_veil.ctx->ClearRenderTargetView(g_veil.rtv, rgba);
    HRESULT hr = g_veil.sc->Present(0, 0);
    if (SUCCEEDED(hr) && g_veil.dcompDev) g_veil.dcompDev->Commit();
    return hr;
}

static DWORD WINAPI VeilThreadProc(LPVOID)
{
    g_veilThreadId = GetCurrentThreadId();
    if (!InitVeilEngine(g_veilEnabled.load())) {
        return 1;
    }

    RegisterHotKey(g_veil.hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F9);
    RegisterHotKey(g_veil.hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10);
    RegisterHotKey(g_veil.hwnd, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11);
    RegisterHotKey(g_veil.hwnd, 4, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'T');

    RAWINPUTDEVICE rid = { 0x01, 0x06, RIDEV_INPUTSINK, g_veil.hwnd };
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    HWINEVENTHOOK hWinShow = g_lockVramMode ? SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
        WinEventShowProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS) : nullptr;

    bool prevOn = g_veilEnabled.load();
    int  prevPreset = g_veilPreset.load();

    /* Event-driven 0%-CPU / 0%-GPU / 0%-flicker message loop */
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);

        bool curOn = g_veilEnabled.load();
        int curPreset = g_veilPreset.load();
        bool dirty = g_veilRenderDirty.exchange(false);
        if (curOn != prevOn || (curOn && (curPreset != prevPreset || dirty))) {
            prevOn = curOn;
            prevPreset = curPreset;
            if (curOn) {
                SetWindowDisplayAffinity(g_veil.hwnd, WDA_EXCLUDEFROMCAPTURE);
                SetWindowPos(g_veil.hwnd, HWND_TOPMOST, g_veil.vx, g_veil.vy, g_veil.vw, g_veil.vh,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
                HRESULT hr = RenderSolidVeilFrame(g_veilAlphas[curPreset]);
                if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                    ReleaseVeilEngine();
                    if (!InitVeilEngine(true)) break;
                    RegisterHotKey(g_veil.hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F9);
                    RegisterHotKey(g_veil.hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10);
                    RegisterHotKey(g_veil.hwnd, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11);
                    RegisterHotKey(g_veil.hwnd, 4, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'T');
                    RegisterRawInputDevices(&rid, 1, sizeof(rid));
                }
            } else {
                RenderSolidVeilFrame(0.0f);
                ShowWindow(g_veil.hwnd, SW_HIDE);
            }
        }
        if (InterlockedCompareExchange(&g_running, 0, 0) == 0) break;
    }

    if (hWinShow) UnhookWinEvent(hWinShow);
    UnregisterHotKey(g_veil.hwnd, 1);
    UnregisterHotKey(g_veil.hwnd, 2);
    UnregisterHotKey(g_veil.hwnd, 3);
    UnregisterHotKey(g_veil.hwnd, 4);
    ReleaseVeilEngine();
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Automated Self-Test, Dry-Run, Status & Clean Stop Controls                 */
/* -------------------------------------------------------------------------- */

static int RunSelfTest()
{
    Logf("========================================================================\n");
    Logf("GhostSwap v2.1 Self-Test: 0x20 Zero-Snow PIC Engine + Solid Blackout Veil\n");
    Logf("========================================================================\n");

    WCHAR desk[128] = L"?"; DWORD need = 0;
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desk, sizeof(desk), &need);
    Logf("[ENV] Desktop=%ls\n", desk);

    if (!DiscoverLocalDxgiAndGpic(true)) {
        Logf("[FAIL] DiscoverLocalDxgiAndGpic failed\n");
        return 1;
    }

    if (!InitVeilEngine(true)) {
        Logf("[FAIL] InitVeilEngine failed\n");
        return 2;
    }
    RenderSolidVeilFrame(1.0f);
    Logf("[VEIL] SwapChainFlags=0x%X BackBufMisc=0x%06X WDA=0x%lX Size=%dx%d\n",
        g_veil.actualFlags, g_veil.backBufMisc, g_veil.wda, g_veil.vw, g_veil.vh);

    /* PrintWindow check -> must be 0.00% */
    int pwNonZero = 0;
    {
        HDC hdcM = CreateCompatibleDC(nullptr);
        BITMAPINFO bmi = {}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = 256; bmi.bmiHeader.biHeight = -256;
        bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
        DWORD* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(hdcM, &bmi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        SelectObject(hdcM, hbm);
        memset(bits, 0, 256 * 256 * 4);
        BOOL pw = PrintWindow(g_veil.hwnd, hdcM, PW_RENDERFULLCONTENT);
        for (int i = 0; i < 65536; i++) if ((bits[i] & 0x00FFFFFFu) != 0) pwNonZero++;
        DeleteObject(hbm); DeleteDC(hdcM);
        Logf("  [CAPTURE] PrintWindow ok=%d nonBlack=%d/65536 (%.2f%%)\n", pw ? 1 : 0, pwNonZero, 100.0 * pwNonZero / 65536.0);
    }

    /* Same-device backbuffer readback -> must be uniform exact solid blackout (0 noise, uniq == 1) */
    int exactSolid = 0;
    size_t uniqCnt = 0;
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(g_veil.sc->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
            D3D11_TEXTURE2D_DESC td = {}; bb->GetDesc(&td);
            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(g_veil.dev->CreateRenderTargetView(bb, nullptr, &rtv)) && rtv) {
                const float solid[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                g_veil.ctx->ClearRenderTargetView(rtv, solid);
                g_veil.ctx->Flush();
                rtv->Release();
            }
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
            ID3D11Texture2D* stg = nullptr;
            if (SUCCEEDED(g_veil.dev->CreateTexture2D(&sd, nullptr, &stg)) && stg) {
                g_veil.ctx->CopyResource(stg, bb);
                g_veil.ctx->Flush();
                D3D11_MAPPED_SUBRESOURCE m = {};
                if (SUCCEEDED(g_veil.ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
                    std::set<DWORD> u;
                    for (UINT y = 0; y < 128 && y < td.Height; y++) {
                        const DWORD* row = (const DWORD*)((const BYTE*)m.pData + y * m.RowPitch);
                        for (UINT x = 0; x < 128 && x < td.Width; x++) {
                            DWORD v = row[x];
                            if (v == 0xFF000000u) exactSolid++;
                            u.insert(v);
                        }
                    }
                    uniqCnt = u.size();
                    g_veil.ctx->Unmap(stg, 0);
                }
                stg->Release();
            }
            bb->Release();
        }
        Logf("  [READBACK] VeilSolidBlack=0xFF000000 exact=%d/16384 uniq=%zu\n", exactSolid, uniqCnt);
    }

    ReleaseVeilEngine();

    int elig = 0, excl = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (InspectProcessEligibility(pe.th32ProcessID, pe.szExeFile, true).eligible) elig++;
                else excl++;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    Logf("[SCAN] Global No-Whitelist Audit: eligibleDxgi=%d excludedOrNoDxgi=%d\n", elig, excl);

    bool ok = (g_veil.actualFlags == 0x20u) && (g_veil.backBufMisc == 0) &&
              (g_veil.wda == WDA_EXCLUDEFROMCAPTURE) && (pwNonZero == 0) &&
              (exactSolid == 16384) && (uniqCnt == 1) && (elig > 0);
    Logf("[VERDICT] %s\n", ok ? "ALL_PASS" : "FAILED");
    return ok ? 0 : 1;
}

static int RunDryRun()
{
    Logf("========================================================================\n");
    Logf("GhostSwap v2.1 --dry-run (Global No-Whitelist x64 DXGI Process Audit)\n");
    Logf("========================================================================\n");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 1;
    int elig = 0, sysExcl = 0, acgOrProt = 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcInspectResult r = InspectProcessEligibility(pe.th32ProcessID, pe.szExeFile, true);
            if (r.eligible) {
                elig++;
                Logf("  [TARGET]   PID=%-6lu %-26ls dxgi=0x%llX CIG=0x%lX CFG=0x%lX\n",
                    r.pid, r.exeName.c_str(), (unsigned long long)r.dxgiBase, r.cig, r.cfg);
            } else if (r.reason == "safety-blacklist") {
                sysExcl++;
                Logf("  [EXCLUDED] PID=%-6lu %-26ls reason=%s\n", r.pid, r.exeName.c_str(), r.reason.c_str());
            } else if (r.reason == "acg-enabled" || r.reason == "protected-ppl") {
                acgOrProt++;
                Logf("  [SKIPPED]  PID=%-6lu %-26ls reason=%s\n", r.pid, r.exeName.c_str(), r.reason.c_str());
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    Logf("Summary: EligibleTargets=%d  SafetyBlacklisted=%d  Prot/ACGSkipped=%d\n", elig, sysExcl, acgOrProt);
    return 0;
}

static bool IsShieldRunning(DWORD* outPid = nullptr)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"AegisBlackout.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"ApexShield.exe") == 0) {
                found = true;
                if (outPid) *outPid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static void WriteStatusFileSnapshot(bool hasDebug)
{
    LONG totCreate = 0, totUp = 0, totResUp = 0, totVer020 = 0, totVer80000 = 0;
    int activeCnt = 0;
    if (g_lockVramMode) {
        PollAllTelemetry();
        for (const auto& t : g_attached) {
            if (!t.detached) activeCnt++;
            totCreate += t.lastTele.totalCreateCalls;
            totUp += t.lastTele.upgraded420 + t.lastTele.upgraded400;
            totResUp += t.lastTele.resizeUpgraded;
            totVer020 += t.lastTele.verifiedDisplayOnly;
            totVer80000 += t.lastTele.verifiedHwProtBackBuf;
        }
    }

    DWORD shieldPid = 0;
    bool shieldUp = IsShieldRunning(&shieldPid);

    WCHAR stPath[MAX_PATH] = L"", tmpPath[MAX_PATH] = L"";
    GetStatusFilePath(stPath, MAX_PATH);
    _snwprintf_s(tmpPath, _TRUNCATE, L"%ls.tmp", stPath);
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmpPath, L"wb") != 0 || !f) return;

    SYSTEMTIME lt = {}; GetLocalTime(&lt);
    fprintf(f, "========================================================================\n");
    fprintf(f, "  GhostSwap v2.1 后台无感运行状态 (%04u-%02u-%02u %02u:%02u:%02u)\n",
        lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
    fprintf(f, "========================================================================\n");
    fprintf(f, "  进程 PID         : %lu (%s)\n", GetCurrentProcessId(), hasDebug ? "管理员无感后台" : "当前用户后台");
    if (!g_lockVramMode) {
        fprintf(f, "  运行模式         : 协同护盾模式 (防截屏由 AegisBlackout v4.1 全局托管，保证 Ctrl+Alt+Q 自己清晰截屏)\n");
        if (shieldUp) {
            fprintf(f, "  AegisBlackout 护盾 : 运行中 (PID=%lu)\n", shieldPid);
        } else {
            fprintf(f, "  AegisBlackout 护盾 : 未检测到\n");
        }
    } else {
        fprintf(f, "  运行模式         : 极端显存焊死模式 (%s)\n",
            g_hwProtected ? "0x420 HW_PROTECTED" : "0x20 DISPLAY_ONLY，连 Ctrl+Alt+Q 也会被截黑");
    }
    fprintf(f, "  瞬间防偷拍黑幕   : %s | 档位: %s\n",
        g_veilEnabled.load() ? "开启 (ON)" : "待机 (OFF)", g_veilNames[g_veilPreset.load()]);
    fprintf(f, "  快捷键           : Ctrl+Alt+F9 开关黑幕 | F10 100%%/85%%切换 | F11 浓度循环 | Ctrl+Alt+Q 自己特权截屏\n");
    if (g_lockVramMode) {
        fprintf(f, "  已挂载活动进程数 : %d (累计挂载 %zu)\n", activeCnt, g_attached.size());
        fprintf(f, "  累计升级统计     : Create=%ld | Upgraded=%ld | ResizeUp=%ld | Verified0x20=%ld | Verified0x80000=%ld\n",
            totCreate, totUp, totResUp, totVer020, totVer80000);
        fprintf(f, "------------------------------------------------------------------------\n");
        for (const auto& t : g_attached) {
            if (t.detached) continue;
            const auto& s = t.lastTele;
            if (s.totalCreateCalls == 0 && s.resizeCalls == 0 && s.presentCalls == 0) continue;
            fprintf(f, "  PID=%-6lu %-20ls Create=%-3ld Up=%-3ld Ver0x20=%-3ld Res=%ld/%ld Pres=%-6ld Last=0x%lX->0x%lX\n",
                t.pid, t.exeName.c_str(), s.totalCreateCalls,
                s.upgraded420 + s.upgraded400, s.verifiedDisplayOnly,
                s.resizeUpgraded, s.resizeCalls, s.presentCalls,
                s.lastOrigFlags, s.lastFinalFlags);
        }
    }
    fprintf(f, "========================================================================\n");
    fclose(f);
    MoveFileExW(tmpPath, stPath, MOVEFILE_REPLACE_EXISTING);
}

static bool IsAnotherGhostSwapRunning(DWORD* outPid = nullptr)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    DWORD selfPid = GetCurrentProcessId();
    bool found = false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != selfPid &&
                (_wcsicmp(pe.szExeFile, L"GhostSwap.exe") == 0 ||
                 _wcsicmp(pe.szExeFile, L"GhostSwap_v2.exe") == 0 ||
                 _wcsicmp(pe.szExeFile, L"GhostSwap_v21.exe") == 0)) {
                found = true;
                if (outPid) *outPid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static int RunStatusCmd()
{
    DWORD otherPid = 0;
    bool alive = IsAnotherGhostSwapRunning(&otherPid);
    Logf("[STATUS] Running=%s (PID=%lu)\n", alive ? "YES" : "NO", otherPid);
    WCHAR stPath[MAX_PATH] = L"";
    GetStatusFilePath(stPath, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, stPath, L"rb") == 0 && f) {
        char buf[4096]; size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, g_logFile ? g_logFile : stdout);
        fclose(f);
    } else {
        Logf("[STATUS] No status snapshot written yet (%ls)\n", stPath);
    }
    return alive ? 0 : 1;
}

static int RunStopCmd()
{
    const wchar_t* evNames[] = { L"Global\\GhostSwap_CleanStop_v2", L"Local\\GhostSwap_CleanStop_v2" };
    int signaled = 0;
    for (const wchar_t* n : evNames) {
        HANDLE hEv = OpenEventW(EVENT_MODIFY_STATE, FALSE, n);
        if (hEv) {
            SetEvent(hEv);
            CloseHandle(hEv);
            signaled++;
        }
    }
    for (int i = 0; i < 30; i++) {
        if (!IsAnotherGhostSwapRunning()) {
            Logf("[STOP] Silent GhostSwap stopped cleanly (signaled=%d).\n", signaled);
            return 0;
        }
        Sleep(100);
    }
    Logf("[STOP] CleanStop signaled=%d, process still shutting down.\n", signaled);
    return 0;
}

static int RunStopLegacyCmd()
{
    DWORD selfPid = GetCurrentProcessId();
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID != selfPid && _wcsicmp(pe.szExeFile, L"GhostSwap.exe") == 0) {
                    pids.push_back(pe.th32ProcessID);
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (pids.empty()) {
        Logf("[STOP-LEGACY] No running GhostSwap.exe found.\n");
        return 0;
    }

    int signaled = 0;
    for (DWORD pid : pids) {
        FreeConsole();
        if (AttachConsole(pid)) {
            SetConsoleCtrlHandler(nullptr, TRUE);
            if (GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) signaled++;
            FreeConsole();
            SetConsoleCtrlHandler(nullptr, FALSE);
        }
    }
    AttachCliConsoleIfNeeded();

    for (int i = 0; i < 25; i++) {
        bool anyAlive = false;
        for (DWORD pid : pids) {
            HANDLE hp = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hp) {
                DWORD ec = 0;
                if (GetExitCodeProcess(hp, &ec) && ec == STILL_ACTIVE) anyAlive = true;
                CloseHandle(hp);
            }
        }
        if (!anyAlive) {
            Logf("[STOP-LEGACY] Legacy GhostSwap exited cleanly (ctrlC=%d).\n", signaled);
            return 0;
        }
        Sleep(100);
    }
    Logf("[STOP-LEGACY] Timeout waiting for legacy GhostSwap (ctrlC=%d).\n", signaled);
    return 1;
}

static void PrintDetailedTargetLogs()
{
    PollAllTelemetry();
    for (const auto& t : g_attached) {
        const auto& s = t.lastTele;
        Logf("  -> PID=%lu (%ls) active=%d Create=%ld [Primary=%ld Fallback=%ld Orig=%ld] Verified0x20=%ld VerifiedHwProt=%ld Resize=%ld/%ld Pres=%ld Last[Flags:0x%lX->0x%lX Misc:0x%lX Hr:0x%08lX %ldx%ld]\n",
            t.pid, t.exeName.c_str(), t.detached ? 0 : 1,
            s.totalCreateCalls, s.upgraded420, s.upgraded400, s.fallbackOrig,
            s.verifiedDisplayOnly, s.verifiedHwProtBackBuf,
            s.resizeUpgraded, s.resizeCalls, s.presentCalls,
            s.lastOrigFlags, s.lastFinalFlags, s.lastBackBufMisc, s.lastHr, s.lastW, s.lastH);
    }
}

static std::vector<DWORD> CollectDescendantPids(DWORD rootPid)
{
    std::vector<DWORD> pids; pids.push_back(rootPid);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    std::vector<std::pair<DWORD, DWORD>> edges;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) do { edges.push_back({ pe.th32ProcessID, pe.th32ParentProcessID }); } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    bool added = true;
    while (added) {
        added = false;
        for (const auto& e : edges) {
            if (std::find(pids.begin(), pids.end(), e.second) != pids.end() &&
                std::find(pids.begin(), pids.end(), e.first) == pids.end()) {
                pids.push_back(e.first); added = true;
            }
        }
    }
    return pids;
}

static HANDLE CreatePermissiveNamedObject(bool isMutex, const wchar_t* name, BOOL* outAlreadyExists = nullptr)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x001F0003;;;IU)", SDDL_REVISION_1, &pSD, nullptr)) {
        sa.lpSecurityDescriptor = pSD;
    }
    HANDLE h = isMutex
        ? CreateMutexW(sa.lpSecurityDescriptor ? &sa : nullptr, FALSE, name)
        : CreateEventW(sa.lpSecurityDescriptor ? &sa : nullptr, TRUE, FALSE, name);
    DWORD err = GetLastError();
    if (pSD) LocalFree(pSD);
    if (outAlreadyExists) *outAlreadyExists = (err == ERROR_ALREADY_EXISTS);
    return h;
}

/* -------------------------------------------------------------------------- */
/* wWinMain Entry Point (/SUBSYSTEM:WINDOWS)                                  */
/* -------------------------------------------------------------------------- */

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
    HMODULE hU32 = GetModuleHandleW(L"user32.dll");
    if (hU32) {
        auto pDpi = (PFN_SetProcessDpiAwarenessContext)GetProcAddress(hU32, "SetProcessDpiAwarenessContext");
        if (pDpi) pDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    InitDynamicApis();
    DWORD privErr = 0;
    bool hasDebug = EnableDebugPrivilege(&privErr);

    bool doSelfTest = false, doDryRun = false, doStatus = false, doStop = false, doStopLegacy = false;
    bool hasOnDefault = false, hasDuration = false, useDefaultDesktop = false;
    DWORD targetPid = 0, durationMs = 0;
    std::wstring readyEventName, launchCmdLine;

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--on-default") == 0 && i + 1 < argc) {
            hasOnDefault = true;
            if (_wfopen_s(&g_logFile, argv[++i], L"wb") != 0) return 10;
        } else if (_wcsicmp(argv[i], L"--self-test") == 0) { doSelfTest = true; }
        else if (_wcsicmp(argv[i], L"--dry-run") == 0) { doDryRun = true; }
        else if (_wcsicmp(argv[i], L"--status") == 0) { doStatus = true; }
        else if (_wcsicmp(argv[i], L"--stop") == 0) { doStop = true; }
        else if (_wcsicmp(argv[i], L"--stop-legacy") == 0) { doStopLegacy = true; }
        else if (_wcsicmp(argv[i], L"--pid") == 0 && i + 1 < argc) { targetPid = (DWORD)_wtoi(argv[++i]); }
        else if (_wcsicmp(argv[i], L"--duration-ms") == 0 && i + 1 < argc) { durationMs = (DWORD)_wtoi(argv[++i]); hasDuration = true; }
        else if (_wcsicmp(argv[i], L"--duration") == 0 && i + 1 < argc) { durationMs = (DWORD)(_wtoi(argv[++i]) * 1000); hasDuration = true; }
        else if (_wcsicmp(argv[i], L"--ready-event") == 0 && i + 1 < argc) { readyEventName = argv[++i]; }
        else if (_wcsicmp(argv[i], L"--launch") == 0 && i + 1 < argc) { launchCmdLine = argv[++i]; }
        else if (_wcsicmp(argv[i], L"--desktop-default") == 0) { useDefaultDesktop = true; }
        else if (_wcsicmp(argv[i], L"--recreate-existing") == 0) { g_recreateExisting = TRUE; }
        else if (_wcsicmp(argv[i], L"--include-shell") == 0) { g_includeShell = TRUE; }
        else if (_wcsicmp(argv[i], L"--hw-protected") == 0) { g_hwProtected = TRUE; g_lockVramMode = TRUE; }
        else if (_wcsicmp(argv[i], L"--lock-vram") == 0) { g_lockVramMode = TRUE; }
        else if (_wcsicmp(argv[i], L"--veil-on") == 0) { g_initialVeilOn = TRUE; }
        else if (_wcsicmp(argv[i], L"--quiet") == 0) { g_quietMode = TRUE; }
    }
    if (argv) LocalFree(argv);

    bool isCliMode = (doSelfTest || doDryRun || doStatus || doStop || doStopLegacy || targetPid != 0 || !launchCmdLine.empty());
    if (isCliMode && !hasOnDefault) {
        AttachCliConsoleIfNeeded();
    }

    if (doSelfTest && !hasOnDefault) {
        WCHAR desk[128] = L""; DWORD need = 0;
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desk, sizeof(desk), &need);
        if (_wcsicmp(desk, L"Default") != 0) {
            WCHAR selfPath[MAX_PATH] = L"", logPath[MAX_PATH] = L"", cmd[1024] = L"";
            GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
            wcscpy_s(logPath, selfPath); wcscat_s(logPath, L".selftest.log");
            DeleteFileW(logPath);
            _snwprintf_s(cmd, _TRUNCATE, L"\"%ls\" --self-test --on-default \"%ls\"", selfPath, logPath);
            STARTUPINFOW si = {}; si.cb = sizeof(si); si.lpDesktop = const_cast<LPWSTR>(L"WinSta0\\Default");
            PROCESS_INFORMATION pi = {};
            if (!CreateProcessW(selfPath, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 2;
            WaitForSingleObject(pi.hProcess, 25000);
            DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            FILE* fp = nullptr;
            if (_wfopen_s(&fp, logPath, L"rb") == 0 && fp) {
                char buf[4096]; size_t n = 0;
                while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
                    if (g_hasConsoleOut) fwrite(buf, 1, n, stdout);
                }
                fclose(fp); DeleteFileW(logPath);
            }
            return (int)ec;
        }
    }

    if (doStatus) return RunStatusCmd();
    if (doStop) return RunStopCmd();
    if (doStopLegacy) return RunStopLegacyCmd();
    if (doDryRun) return RunDryRun();
    if (doSelfTest) {
        int rc = RunSelfTest();
        if (g_logFile) fclose(g_logFile);
        return rc;
    }

    if (!launchCmdLine.empty() || targetPid != 0 || doSelfTest || g_lockVramMode) {
        if (!DiscoverLocalDxgiAndGpic(isCliMode)) {
            if (g_logFile) fclose(g_logFile);
            return 1;
        }
    }

    if (!launchCmdLine.empty()) {
        std::vector<WCHAR> cmdBuf(launchCmdLine.begin(), launchCmdLine.end()); cmdBuf.push_back(L'\0');
        STARTUPINFOW si = {}; si.cb = sizeof(si);
        if (useDefaultDesktop) si.lpDesktop = const_cast<LPWSTR>(L"WinSta0\\Default");
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            Logf("[FATAL] --launch CreateProcessW err=%lu\n", GetLastError());
            return 2;
        }
        DWORD rootPid = pi.dwProcessId;
        Logf("[LAUNCH] Started root PID=%lu\n", rootPid);
        CloseHandle(pi.hThread);
        ULONGLONG startTick = GetTickCount64(), lastLog = 0;
        if (!hasDuration) durationMs = 15000;
        while (InterlockedCompareExchange(&g_running, 0, 0) != 0) {
            std::vector<DWORD> tree = CollectDescendantPids(rootPid);
            for (DWORD pid : tree) {
                if (IsPidTracked(pid) || g_retryMap[pid] >= 12) continue;
                HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                WCHAR img[MAX_PATH] = L""; DWORD ilen = MAX_PATH;
                if (hp && QueryFullProcessImageNameW(hp, 0, img, &ilen)) {
                    const wchar_t* bn = wcsrchr(img, L'\\'); bn = bn ? bn + 1 : img;
                    ProcInspectResult info = InspectProcessEligibility(pid, bn, false);
                    if (info.eligible && !AttachToTarget(info)) g_retryMap[pid]++;
                }
                if (hp) CloseHandle(hp);
            }
            ULONGLONG now = GetTickCount64();
            if (now - lastLog >= 1500) { lastLog = now; PrintDetailedTargetLogs(); }
            if (hasDuration && (now - startTick) >= durationMs) break;
            if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0 && !hasDuration) break;
        }
        PrintDetailedTargetLogs();
        DetachAllTargets();
        CloseHandle(pi.hProcess);
        Logf("[LAUNCH] Cleanly detached.\n");
        if (g_logFile) fclose(g_logFile);
        return 0;
    }

    if (targetPid != 0) {
        ULONGLONG startTick = GetTickCount64();
        bool attached = false;
        while (InterlockedCompareExchange(&g_running, 0, 0) != 0 && !attached) {
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid);
            if (!hp) return 3;
            WCHAR img[MAX_PATH] = L""; DWORD ilen = MAX_PATH;
            QueryFullProcessImageNameW(hp, 0, img, &ilen); CloseHandle(hp);
            const wchar_t* bn = wcsrchr(img, L'\\'); bn = bn ? bn + 1 : img;
            ProcInspectResult info = InspectProcessEligibility(targetPid, bn, false);
            if (info.eligible) { attached = AttachToTarget(info); break; }
            if (GetTickCount64() - startTick > 5000) return 4;
            Sleep(15);
        }
        if (!attached) return 5;
        if (!readyEventName.empty()) {
            HANDLE hEv = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName.c_str());
            if (!hEv) hEv = CreateEventW(nullptr, TRUE, FALSE, readyEventName.c_str());
            if (hEv) { SetEvent(hEv); CloseHandle(hEv); }
        }
        ULONGLONG runStart = GetTickCount64();
        while (InterlockedCompareExchange(&g_running, 0, 0) != 0) {
            PollAllTelemetry();
            if (g_attached[0].detached) break;
            if (hasDuration && (GetTickCount64() - runStart) >= durationMs) break;
            Sleep(50);
        }
        PrintDetailedTargetLogs();
        DetachAllTargets();
        Logf("[PID-MODE] Cleanly detached PID=%lu.\n", targetPid);
        if (g_logFile) fclose(g_logFile);
        return 0;
    }

    /* Resident Silent Background Mode (zero console window!) */
    BOOL existsGlobal = FALSE, existsLocal = FALSE;
    g_hSingleInstanceMutex = CreatePermissiveNamedObject(true, L"Global\\GhostSwap_SingleInstance_v2", &existsGlobal);
    if (!g_hSingleInstanceMutex) {
        g_hSingleInstanceMutex = CreatePermissiveNamedObject(true, L"Local\\GhostSwap_SingleInstance_v2", &existsLocal);
    }
    if (existsGlobal || existsLocal) {
        if (g_hSingleInstanceMutex) CloseHandle(g_hSingleInstanceMutex);
        return 0; /* Already running silently */
    }

    g_hStopEventGlobal = CreatePermissiveNamedObject(false, L"Global\\GhostSwap_CleanStop_v2");
    g_hStopEventLocal  = CreatePermissiveNamedObject(false, L"Local\\GhostSwap_CleanStop_v2");
    g_hScanWakeEvent   = g_lockVramMode ? CreateEventW(nullptr, FALSE, TRUE, nullptr) : nullptr;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    g_veilEnabled.store(g_initialVeilOn != FALSE);
    HANDLE hVeilThread = CreateThread(nullptr, 0, VeilThreadProc, nullptr, 0, nullptr);

    ULONGLONG startTick = GetTickCount64();
    ULONGLONG lastStatusWrite = 0;

    while (InterlockedCompareExchange(&g_running, 0, 0) != 0) {
        if (g_lockVramMode) {
            ScanAndAttachEligible();
        }

        ULONGLONG now = GetTickCount64();
        if (now - lastStatusWrite >= 2000) {
            lastStatusWrite = now;
            WriteStatusFileSnapshot(hasDebug);
        }

        if (hasDuration && (now - startTick) >= durationMs) break;

        HANDLE waitHandles[3];
        DWORD nH = 0;
        if (g_hScanWakeEvent) waitHandles[nH++] = g_hScanWakeEvent;
        if (g_hStopEventLocal) waitHandles[nH++] = g_hStopEventLocal;
        if (g_hStopEventGlobal) waitHandles[nH++] = g_hStopEventGlobal;

        DWORD waitMs = g_lockVramMode ? 25 : 500;
        DWORD wr = WAIT_TIMEOUT;
        if (nH > 0) {
            wr = WaitForMultipleObjects(nH, waitHandles, FALSE, waitMs);
        } else {
            Sleep(waitMs);
        }
        if (g_hStopEventLocal && WaitForSingleObject(g_hStopEventLocal, 0) == WAIT_OBJECT_0) break;
        if (g_hStopEventGlobal && WaitForSingleObject(g_hStopEventGlobal, 0) == WAIT_OBJECT_0) break;
        (void)wr;
    }

    InterlockedExchange(&g_running, 0);
    if (g_veilThreadId != 0) PostThreadMessageW(g_veilThreadId, WM_QUIT, 0, 0);
    if (hVeilThread) { WaitForSingleObject(hVeilThread, 3000); CloseHandle(hVeilThread); }

    if (g_lockVramMode || !g_attached.empty()) {
        DetachAllTargets();
    }
    WriteStatusFileSnapshot(hasDebug);

    if (g_hScanWakeEvent) CloseHandle(g_hScanWakeEvent);
    if (g_hStopEventLocal) CloseHandle(g_hStopEventLocal);
    if (g_hStopEventGlobal) CloseHandle(g_hStopEventGlobal);
    if (g_hSingleInstanceMutex) CloseHandle(g_hSingleInstanceMutex);
    return 0;
}
