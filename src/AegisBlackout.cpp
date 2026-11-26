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
#include <tlhelp32.h>
#include <psapi.h>
#include <wintrust.h>
#include <softpub.h>
#include <stdio.h>
#include <stdlib.h>
#include <share.h>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

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
#define WM_APP_SECURITY_ALERT  (WM_APP + 2)
#define TIMER_WATCHDOG_ID      1
#define TIMER_HWDRM_HEARTBEAT  2
#define TIMER_CLOAK_SWEEP_ID   3
#define TIMER_ALERT_HIDE_ID    4
#ifndef QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC
#define QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC 0x00000001
#endif

typedef LONG NTSTATUS_AEGIS;
typedef NTSTATUS_AEGIS(NTAPI* PFN_NtQueueApcThreadEx2)(
    HANDLE ThreadHandle,
    HANDLE ReserveHandle,
    ULONG  ApcFlags,
    PVOID  ApcRoutine,
    PVOID  ApcArgument1,
    PVOID  ApcArgument2,
    PVOID  ApcArgument3
);
typedef BOOL(WINAPI* PFN_IsWow64Process2)(HANDLE, USHORT*, USHORT*);

static BOOL GetExeRelativePathW(const WCHAR* fileName, WCHAR* outPath, size_t maxChars)
{
    if (!fileName || !outPath || maxChars == 0) return FALSE;
    DWORD len = GetModuleFileNameW(NULL, outPath, (DWORD)maxChars);
    if (len == 0 || len >= maxChars) return FALSE;
    WCHAR* slash = wcsrchr(outPath, L'\\');
    if (slash) {
        *(slash + 1) = L'\0';
    } else {
        outPath[0] = L'\0';
    }
    return wcscat_s(outPath, maxChars, fileName) == 0;
}

/* Startup-only diagnostic logger (never writes during runtime screenshot events) */
static void StartupLog(const char* line, BOOL overwrite)
{
    WCHAR logPath[MAX_PATH] = { 0 };
    if (!GetExeRelativePathW(L"aegis_init.log", logPath, MAX_PATH)) return;

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
static HWND      g_hAlertWnd = NULL;
static HWINEVENTHOOK g_hWinEventFg     = NULL;
static HWINEVENTHOOK g_hWinEventShow   = NULL;
static HWINEVENTHOOK g_hWinEventLoc    = NULL;
static HWINEVENTHOOK g_hWinEventCreate = NULL;

static int g_vx = 0, g_vy = 0, g_vw = 3840, g_vh = 2160;
static volatile BOOL g_bInternalUnmasking = FALSE;
static void RefreshVirtualScreenBounds(void);

/* -------------------------------------------------------------------------- */
/* v4.2 Real-Time Security Alert Engine (Log + Dedup + IPC + Red HUD Banner)  */
/* -------------------------------------------------------------------------- */
#define MAX_ALERT_DEDUP 64

struct AlertDedupEntry {
    char      key[192];
    ULONGLONG lastTick;
};

static CRITICAL_SECTION g_alertCs;
static BOOL             g_alertCsInit = FALSE;
static AlertDedupEntry  g_alertDedup[MAX_ALERT_DEDUP] = {};
static int              g_alertDedupNext = 0;
static WCHAR            g_alertBannerText[280] = L"";
static HANDLE           g_hAlertEvent = NULL;
static HANDLE           g_hSentinelStopEv = NULL;
static HANDLE           g_hSentinelThread = NULL;
static volatile BOOL    g_bSentinelActive = FALSE;

static void UpperCaseWStr(WCHAR* s)
{
    if (!s) return;
    for (; *s; ++s) {
        if (*s >= L'a' && *s <= L'z') *s = (WCHAR)(*s - L'a' + L'A');
    }
}

static void EnsureAlertEventCreated(void)
{
    if (g_hAlertEvent) return;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    PSECURITY_DESCRIPTOR pSd = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &pSd, NULL)) {
        sa.lpSecurityDescriptor = pSd;
    }
    g_hAlertEvent = CreateEventW(&sa, FALSE, FALSE, L"Local\\AegisBlackout_SecurityAlert_Ev");
    if (pSd) LocalFree(pSd);
}

static void EmitSecurityAlert(
    const char* alertType,
    DWORD pid,
    const WCHAR* exeName,
    const char* detailUtf8,
    const char* dedupKey)
{
    if (!g_alertCsInit) return;
    ULONGLONG now = GetTickCount64();

    EnterCriticalSection(&g_alertCs);
    if (dedupKey && dedupKey[0]) {
        for (int i = 0; i < MAX_ALERT_DEDUP; i++) {
            if (g_alertDedup[i].key[0] &&
                _stricmp(g_alertDedup[i].key, dedupKey) == 0 &&
                (now - g_alertDedup[i].lastTick) < 30000ULL) {
                LeaveCriticalSection(&g_alertCs);
                return;
            }
        }
        strncpy_s(g_alertDedup[g_alertDedupNext].key, dedupKey, _TRUNCATE);
        g_alertDedup[g_alertDedupNext].lastTick = now;
        g_alertDedupNext = (g_alertDedupNext + 1) % MAX_ALERT_DEDUP;
    }

    char exeUtf8[128] = "unknown";
    if (exeName && exeName[0]) {
        WideCharToMultiByte(CP_UTF8, 0, exeName, -1, exeUtf8, sizeof(exeUtf8) - 1, NULL, NULL);
    }

    WCHAR alertLogPath[MAX_PATH] = L"";
    if (GetExeRelativePathW(L"aegis_security_alerts.log", alertLogPath, MAX_PATH)) {
        SYSTEMTIME lt = {};
        GetLocalTime(&lt);
        FILE* f = _wfsopen(alertLogPath, L"ab", _SH_DENYNO);
        if (f) {
            fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u] [%s] PID=%lu EXE=%s %s\n",
                lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond,
                alertType ? alertType : "ALERT",
                pid, exeUtf8, detailUtf8 ? detailUtf8 : "");
            fflush(f);
            fclose(f);
        }
    }

    WCHAR wType[48] = L"ALERT";
    WCHAR wDetail[160] = L"";
    if (alertType) MultiByteToWideChar(CP_UTF8, 0, alertType, -1, wType, 47);
    if (detailUtf8) MultiByteToWideChar(CP_UTF8, 0, detailUtf8, -1, wDetail, 159);
    _snwprintf_s(g_alertBannerText, _TRUNCATE,
        L"[AegisBlackout \u5B89\u5168\u544A\u8B66] [%ls] %ls (PID %lu): %ls",
        wType, (exeName && exeName[0]) ? exeName : L"unknown", pid, wDetail);
    LeaveCriticalSection(&g_alertCs);

    if (g_hCurtain) {
        PostMessageW(g_hCurtain, WM_APP_SECURITY_ALERT, 0, 0);
    }
}

static LRESULT CALLBACK AlertWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);

        HBRUSH bgBrush = CreateSolidBrush(RGB(48, 10, 14));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        HPEN pen = CreatePen(PS_SOLID, 2, RGB(239, 68, 68));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        WCHAR msgBuf[280] = L"";
        if (g_alertCsInit) {
            EnterCriticalSection(&g_alertCs);
            wcsncpy_s(msgBuf, g_alertBannerText, _TRUNCATE);
            LeaveCriticalSection(&g_alertCs);
        }

        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        if (dpi <= 0) dpi = 96;
        HFONT font = CreateFontW(-MulDiv(12, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        HGDIOBJ oldFont = font ? SelectObject(hdc, font) : NULL;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(254, 226, 226));
        RECT rcText = { rc.left + 14, rc.top + 4, rc.right - 14, rc.bottom - 4 };
        DrawTextW(hdc, msgBuf, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (oldFont) SelectObject(hdc, oldFont);
        if (font) DeleteObject(font);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (g_hAlertWnd == hwnd) g_hAlertWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowOrRefreshAlertHud(void)
{
    if (!g_hCurtain || !g_hInst) return;
    RefreshVirtualScreenBounds();
    if (!g_hAlertWnd) {
        WCHAR alertCls[64];
        wsprintfW(alertCls, L"AegisAlertHud_%08X", RandU32());
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc   = AlertWndProc;
        wc.hInstance     = g_hInst;
        wc.lpszClassName = alertCls;
        RegisterClassW(&wc);

        g_hAlertWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
            wc.lpszClassName,
            L"",
            WS_POPUP,
            g_vx, g_vy + 20, 900, 54,
            NULL, NULL, g_hInst, NULL
        );
        if (!g_hAlertWnd) return;
        SetLayeredWindowAttributes(g_hAlertWnd, 0, 238, LWA_ALPHA);
        SetWindowDisplayAffinity(g_hAlertWnd, WDA_MONITOR);
    }

    int hudW = std::min(1080, std::max(420, g_vw - 40));
    int hudX = g_vx + (g_vw - hudW) / 2;
    int hudY = g_vy + 20;
    SetWindowDisplayAffinity(g_hAlertWnd, WDA_MONITOR);
    SetWindowPos(g_hAlertWnd, HWND_TOPMOST, hudX, hudY, hudW, 54, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hAlertWnd, NULL, TRUE);
    KillTimer(g_hCurtain, TIMER_ALERT_HIDE_ID);
    SetTimer(g_hCurtain, TIMER_ALERT_HIDE_ID, 8000, NULL);
}

static void CheckExternalAlertIpcFile(void)
{
    WCHAR lastAlertPath[MAX_PATH] = L"";
    if (!GetExeRelativePathW(L"aegis_last_alert.txt", lastAlertPath, MAX_PATH)) return;
    FILE* f = _wfsopen(lastAlertPath, L"rb", _SH_DENYNO);
    if (!f) return;
    char line[512] = "";
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return;
    }
    fclose(f);

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    /* Expected format from GhostSwap: TYPE|PID|EXE|DETAIL|DEDUP */
    char* p1 = strchr(line, '|');
    char* p2 = p1 ? strchr(p1 + 1, '|') : NULL;
    char* p3 = p2 ? strchr(p2 + 1, '|') : NULL;
    char* p4 = p3 ? strchr(p3 + 1, '|') : NULL;
    if (p1 && p2 && p3) {
        *p1 = '\0'; *p2 = '\0'; *p3 = '\0';
        if (p4) *p4 = '\0';
        DWORD pid = (DWORD)strtoul(p1 + 1, NULL, 10);
        WCHAR wExe[128] = L"unknown";
        MultiByteToWideChar(CP_UTF8, 0, p2 + 1, -1, wExe, 127);
        const char* detail = p3 + 1;
        const char* dedup = (p4 && p4[1]) ? (p4 + 1) : detail;
        EmitSecurityAlert(line, pid, wExe, detail, dedup);
    }
}

/* -------------------------------------------------------------------------- */
/* v4.2 Surgical Target Process Anti-Inject DACL Lock & Clean Restoration     */
/* -------------------------------------------------------------------------- */
#define MAX_HARDENED_PIDS 128
struct HardenedProcEntry {
    DWORD                pid;
    PSECURITY_DESCRIPTOR origSd;
};
static CRITICAL_SECTION  g_daclCs;
static BOOL              g_daclCsInit = FALSE;
static HardenedProcEntry g_hardenedProcs[MAX_HARDENED_PIDS] = {};
static int               g_hardenedCount = 0;
static DWORD             g_daclSkipPids[MAX_HARDENED_PIDS] = {};
static int               g_daclSkipCount = 0;
static volatile LONG     g_daclHardenedTotal = 0;

static BOOL IsDaclLockExcludedExe(const WCHAR* upperPath)
{
    if (!upperPath || !upperPath[0]) return TRUE;
    static const wchar_t* exclPatterns[] = {
        L"CSRSS.EXE", L"DWM.EXE", L"WINLOGON.EXE", L"LSASS.EXE", L"SMSS.EXE",
        L"SERVICES.EXE", L"SVCHOST.EXE", L"SIHOST.EXE", L"CTFMON.EXE",
        L"STARTMENUEXPERIENCEHOST.EXE", L"SEARCHHOST.EXE", L"TEXTINPUTHOST.EXE",
        L"SHELLEXPERIENCEHOST.EXE", L"LOCKAPP.EXE", L"SCREENCLIPPINGHOST.EXE",
        L"SNIPPINGTOOL.EXE", L"MSMPENG.EXE", L"SECURITYHEALTHSYSTRAY.EXE",
        L"SBIE", L"SANDMAN.EXE", L"EASYANTICHEAT", L"BATTLEYE",
        L"VGC.EXE", L"GAMEGUARD", L"\\STEAMLIBRARY\\", L"STEAM.EXE", L"EPICGAMESLAUNCHER.EXE",
        L"WEGAME", L"TENPROTECT", L"ACE-",
        L"GHOSTSWAP", L"AEGISBLACKOUT",
        L"EXPLORER.EXE", L"CONHOST.EXE", L"OPENCONSOLE.EXE", L"WINDOWSTERMINAL.EXE",
        L"CMD.EXE", L"POWERSHELL.EXE", L"PWSH.EXE",
        L"CODE.EXE", L"DEVENV.EXE", L"MSBUILD.EXE",
        L"CL.EXE", L"LINK.EXE", L"SANDBOXIE_"
    };
    for (size_t i = 0; i < sizeof(exclPatterns) / sizeof(exclPatterns[0]); i++) {
        if (wcsstr(upperPath, exclPatterns[i]) != NULL) return TRUE;
    }
    return FALSE;
}

static BOOL IsPidDaclAttempted(DWORD pid)
{
    for (int i = 0; i < g_hardenedCount; i++) {
        if (g_hardenedProcs[i].pid == pid) return TRUE;
    }
    for (int i = 0; i < g_daclSkipCount; i++) {
        if (g_daclSkipPids[i] == pid) return TRUE;
    }
    return FALSE;
}

static void RecordDaclSkipPid(DWORD pid)
{
    if (g_daclSkipCount < MAX_HARDENED_PIDS) {
        g_daclSkipPids[g_daclSkipCount++] = pid;
    }
}

static void HardenTargetProcessDacl(DWORD pid, const WCHAR* upperPath)
{
    if (!g_daclCsInit || pid <= 4 || pid == GetCurrentProcessId()) return;
    if (IsDaclLockExcludedExe(upperPath)) return;

    EnterCriticalSection(&g_daclCs);
    BOOL seen = IsPidDaclAttempted(pid);
    LeaveCriticalSection(&g_daclCs);
    if (seen) return;

    HANDLE hProc = OpenProcess(READ_CONTROL | WRITE_DAC | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        EnterCriticalSection(&g_daclCs);
        RecordDaclSkipPid(pid);
        LeaveCriticalSection(&g_daclCs);
        return;
    }

    PACL pOldDacl = NULL;
    PSECURITY_DESCRIPTOR pOldSd = NULL;
    DWORD res = GetSecurityInfo(hProc, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
        NULL, NULL, &pOldDacl, NULL, &pOldSd);
    if (res != ERROR_SUCCESS || !pOldDacl || !pOldSd) {
        if (pOldSd) LocalFree(pOldSd);
        CloseHandle(hProc);
        EnterCriticalSection(&g_daclCs);
        RecordDaclSkipPid(pid);
        LeaveCriticalSection(&g_daclCs);
        return;
    }

    BYTE sysSidBuf[SECURITY_MAX_SID_SIZE] = {};
    BYTE adminSidBuf[SECURITY_MAX_SID_SIZE] = {};
    DWORD sysSidLen = sizeof(sysSidBuf), adminSidLen = sizeof(adminSidBuf);
    PSID pSysSid = CreateWellKnownSid(WinLocalSystemSid, NULL, sysSidBuf, &sysSidLen) ? (PSID)sysSidBuf : NULL;
    PSID pAdminSid = CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, adminSidBuf, &adminSidLen) ? (PSID)adminSidBuf : NULL;

    ACL_SIZE_INFORMATION asi = {};
    if (!GetAclInformation(pOldDacl, &asi, sizeof(asi), AclSizeInformation)) {
        LocalFree(pOldSd);
        CloseHandle(hProc);
        return;
    }

    DWORD newAclSize = asi.AclBytesInUse + 160;
    PACL pNewDacl = (PACL)LocalAlloc(LPTR, newAclSize);
    if (!pNewDacl || !InitializeAcl(pNewDacl, newAclSize, ACL_REVISION)) {
        if (pNewDacl) LocalFree(pNewDacl);
        LocalFree(pOldSd);
        CloseHandle(hProc);
        return;
    }

    const DWORD kStripMask =
        PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_VM_READ |
        PROCESS_CREATE_THREAD | PROCESS_DUP_HANDLE | PROCESS_SET_INFORMATION |
        WRITE_DAC | WRITE_OWNER;

    BOOL hasSys = FALSE, hasAdmin = FALSE, buildOk = TRUE;
    for (DWORD i = 0; i < asi.AceCount && buildOk; i++) {
        LPVOID pAce = NULL;
        if (!GetAce(pOldDacl, i, &pAce) || !pAce) continue;
        ACE_HEADER* hdr = (ACE_HEADER*)pAce;
        if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            ACCESS_ALLOWED_ACE* allowAce = (ACCESS_ALLOWED_ACE*)pAce;
            PSID aceSid = (PSID)&allowAce->SidStart;
            DWORD mask = allowAce->Mask;
            if (pSysSid && EqualSid(aceSid, pSysSid)) {
                mask |= PROCESS_ALL_ACCESS;
                hasSys = TRUE;
            } else if (pAdminSid && EqualSid(aceSid, pAdminSid)) {
                mask |= PROCESS_ALL_ACCESS;
                hasAdmin = TRUE;
            } else {
                if (mask & GENERIC_ALL) {
                    mask |= (PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
                }
                mask &= ~(kStripMask | GENERIC_ALL | GENERIC_WRITE);
                if (mask == 0) continue;
            }
            if (!AddAccessAllowedAceEx(pNewDacl, ACL_REVISION, hdr->AceFlags, mask, aceSid)) {
                buildOk = FALSE;
            }
        } else {
            if (!AddAce(pNewDacl, ACL_REVISION, MAXDWORD, pAce, hdr->AceSize)) {
                buildOk = FALSE;
            }
        }
    }

    if (buildOk && pSysSid && !hasSys) {
        if (!AddAccessAllowedAce(pNewDacl, ACL_REVISION, PROCESS_ALL_ACCESS, pSysSid)) buildOk = FALSE;
    }
    if (buildOk && pAdminSid && !hasAdmin) {
        if (!AddAccessAllowedAce(pNewDacl, ACL_REVISION, PROCESS_ALL_ACCESS, pAdminSid)) buildOk = FALSE;
    }

    DWORD setRes = ERROR_GEN_FAILURE;
    if (buildOk) {
        setRes = SetSecurityInfo(hProc, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
            NULL, NULL, pNewDacl, NULL);
    }

    LocalFree(pNewDacl);
    CloseHandle(hProc);

    EnterCriticalSection(&g_daclCs);
    if (setRes == ERROR_SUCCESS && g_hardenedCount < MAX_HARDENED_PIDS) {
        g_hardenedProcs[g_hardenedCount].pid = pid;
        g_hardenedProcs[g_hardenedCount].origSd = pOldSd;
        g_hardenedCount++;
        _InterlockedIncrement(&g_daclHardenedTotal);
        pOldSd = NULL;
    } else {
        RecordDaclSkipPid(pid);
    }
    LeaveCriticalSection(&g_daclCs);

    if (pOldSd) LocalFree(pOldSd);
}

static void PruneDeadHardenedDacls(const std::vector<DWORD>& alivePids)
{
    if (!g_daclCsInit) return;
    EnterCriticalSection(&g_daclCs);
    int w = 0;
    for (int i = 0; i < g_hardenedCount; i++) {
        bool alive = std::find(alivePids.begin(), alivePids.end(), g_hardenedProcs[i].pid) != alivePids.end();
        if (alive) {
            g_hardenedProcs[w++] = g_hardenedProcs[i];
        } else {
            if (g_hardenedProcs[i].origSd) LocalFree(g_hardenedProcs[i].origSd);
        }
    }
    g_hardenedCount = w;

    int sw = 0;
    for (int i = 0; i < g_daclSkipCount; i++) {
        if (std::find(alivePids.begin(), alivePids.end(), g_daclSkipPids[i]) != alivePids.end()) {
            g_daclSkipPids[sw++] = g_daclSkipPids[i];
        }
    }
    g_daclSkipCount = sw;
    LeaveCriticalSection(&g_daclCs);
}

static void RestoreAllTargetDacls(void)
{
    if (!g_daclCsInit) return;
    EnterCriticalSection(&g_daclCs);
    for (int i = 0; i < g_hardenedCount; i++) {
        DWORD pid = g_hardenedProcs[i].pid;
        PSECURITY_DESCRIPTOR pSd = g_hardenedProcs[i].origSd;
        if (pSd) {
            HANDLE hProc = OpenProcess(WRITE_DAC, FALSE, pid);
            if (hProc) {
                BOOL present = FALSE, defaulted = FALSE;
                PACL pOrigDacl = NULL;
                if (GetSecurityDescriptorDacl(pSd, &present, &pOrigDacl, &defaulted) && present) {
                    SetSecurityInfo(hProc, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                        NULL, NULL, pOrigDacl, NULL);
                }
                CloseHandle(hProc);
            }
            LocalFree(pSd);
            g_hardenedProcs[i].origSd = NULL;
        }
    }
    g_hardenedCount = 0;
    g_daclSkipCount = 0;
    LeaveCriticalSection(&g_daclCs);
}

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

/* v4.2 Zero-Injection Per-Window Special-APC Cloak + WDA Tamper Sentinel state */
#define MAX_CLOAKED_HWNDS 256
#define MAX_FAILED_HWNDS  256
#define MAX_PENDING_CLOAK 32
struct CloakedEntry {
    HWND      hwnd;
    DWORD     pid;
    DWORD     tid;
    DWORD     appliedAff;
    int       dropCount;
    ULONGLONG lastDropTick;
    ULONGLONG dropAlertArmedTick;
    ULONGLONG reapplyGraceUntil;
};
struct PendingCloakEntry {
    HWND      hwnd;
    DWORD     pid;
    DWORD     tid;
    ULONGLONG queuedTick;
    BOOL      triedFallback;
    int       apcSentCount;
};
struct FailedEntry {
    HWND  hwnd;
    DWORD pid;
    int   retries;
    BOOL  apcDeliveredEver;
    BOOL  hijackAlerted;
};
static CloakedEntry      g_cloaked[MAX_CLOAKED_HWNDS];
static int               g_cloakedCount = 0;
static FailedEntry       g_failedEntries[MAX_FAILED_HWNDS];
static int               g_failedCount = 0;
static PendingCloakEntry g_pendingCloak[MAX_PENDING_CLOAK];
static int               g_pendingCount = 0;
static PFN_NtQueueApcThreadEx2 g_pNtQueueApcThreadEx2 = NULL;
static HMODULE                 g_hWin32uLocal         = NULL;
static PVOID                   g_pNtUserSetWDA        = NULL;
static PFN_IsWow64Process2     g_pIsWow64Process2     = NULL;
static BOOL      g_bPerWndCloakReady = FALSE;
static BOOL      g_bCloakSweeping    = FALSE;

static CRITICAL_SECTION g_monitoredCs;
static BOOL             g_monitoredCsInit = FALSE;
static DWORD            g_monitoredSeeds[MAX_CLOAKED_HWNDS] = {};
static int              g_monitoredSeedCount = 0;

static void GetExeBaseNameForPid(DWORD pid, WCHAR* outBase, size_t maxChars)
{
    if (!outBase || maxChars == 0) return;
    wcscpy_s(outBase, maxChars, L"unknown");
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return;
    WCHAR img[MAX_PATH] = L"";
    DWORD imgLen = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, img, &imgLen)) {
        const WCHAR* slash = wcsrchr(img, L'\\');
        const WCHAR* base = slash ? (slash + 1) : img;
        wcsncpy_s(outBase, maxChars, base, _TRUNCATE);
    }
    CloseHandle(hProc);
}

static void PublishMonitoredSeeds(void)
{
    if (!g_monitoredCsInit) return;
    EnterCriticalSection(&g_monitoredCs);
    g_monitoredSeedCount = 0;
    for (int i = 0; i < g_cloakedCount; i++) {
        DWORD pid = g_cloaked[i].pid;
        if (pid <= 4) continue;
        BOOL dup = FALSE;
        for (int j = 0; j < g_monitoredSeedCount; j++) {
            if (g_monitoredSeeds[j] == pid) { dup = TRUE; break; }
        }
        if (!dup && g_monitoredSeedCount < MAX_CLOAKED_HWNDS) {
            g_monitoredSeeds[g_monitoredSeedCount++] = pid;
        }
    }
    LeaveCriticalSection(&g_monitoredCs);
}

static void InitPerWindowCloak(void)
{
    HMODULE hNt   = GetModuleHandleW(L"ntdll.dll");
    HMODULE hK32  = GetModuleHandleW(L"kernel32.dll");
    g_hWin32uLocal = LoadLibraryW(L"win32u.dll");
    if (hNt)            g_pNtQueueApcThreadEx2 = (PFN_NtQueueApcThreadEx2)GetProcAddress(hNt, "NtQueueApcThreadEx2");
    if (g_hWin32uLocal) g_pNtUserSetWDA        = (PVOID)GetProcAddress(g_hWin32uLocal, "NtUserSetWindowDisplayAffinity");
    if (hK32)           g_pIsWow64Process2     = (PFN_IsWow64Process2)GetProcAddress(hK32, "IsWow64Process2");
    g_bPerWndCloakReady = (g_pNtQueueApcThreadEx2 != NULL && g_hWin32uLocal != NULL && g_pNtUserSetWDA != NULL && g_pIsWow64Process2 != NULL);
}

static BOOL VerifyWin32uMappedAtSameBase(DWORD pid)
{
    if (!g_hWin32uLocal || !g_pNtUserSetWDA) return FALSE;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;

    BOOL matched = FALSE;
    BOOL hasSbieDll = FALSE;
    MODULEENTRY32W me = { 0 };
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"SbieDll.dll") == 0) {
                hasSbieDll = TRUE;
                matched = FALSE;
                break;
            }
            if (_wcsicmp(me.szModule, L"win32u.dll") == 0) {
                const BYTE* targetAddr = (const BYTE*)g_pNtUserSetWDA;
                if (me.modBaseAddr == (BYTE*)g_hWin32uLocal &&
                    targetAddr >= me.modBaseAddr &&
                    targetAddr < (me.modBaseAddr + me.modBaseSize)) {
                    matched = TRUE;
                }
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return (!hasSbieDll && matched);
}

static BOOL QueueWindowAffinityApc(HWND hwnd, DWORD tid, DWORD targetAff)
{
    if (!g_bPerWndCloakReady || !hwnd || tid == 0) return FALSE;
    HANDLE hThread = OpenThread(THREAD_SET_CONTEXT, FALSE, tid);
    if (!hThread) return FALSE;
    NTSTATUS_AEGIS st = g_pNtQueueApcThreadEx2(
        hThread,
        NULL,
        QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC,
        g_pNtUserSetWDA,
        (PVOID)hwnd,
        (PVOID)(ULONG_PTR)targetAff,
        NULL
    );
    CloseHandle(hThread);
    if (st == 0) {
        PostMessageW(hwnd, WM_NULL, 0, 0);
        return TRUE;
    }
    return FALSE;
}

static CloakedEntry* FindCloakedEntry(HWND hwnd)
{
    for (int i = 0; i < g_cloakedCount; i++) {
        if (g_cloaked[i].hwnd == hwnd) return &g_cloaked[i];
    }
    return NULL;
}

static BOOL IsKnownCloaked(HWND hwnd)
{
    return FindCloakedEntry(hwnd) != NULL;
}

static BOOL IsFailedHwnd(HWND hwnd)
{
    for (int i = 0; i < g_failedCount; i++) {
        if (g_failedEntries[i].hwnd == hwnd && g_failedEntries[i].retries >= 3) return TRUE;
    }
    return FALSE;
}

static BOOL IsRemoteWin32uWdaTampered(DWORD pid)
{
    if (!g_pNtUserSetWDA || pid <= 4 || pid == GetCurrentProcessId()) return FALSE;
    HANDLE hProc = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return FALSE;
    BYTE remoteBytes[16] = {};
    SIZE_T nRead = 0;
    BOOL ok = ReadProcessMemory(hProc, g_pNtUserSetWDA, remoteBytes, 16, &nRead);
    CloseHandle(hProc);
    if (!ok || nRead != 16) return FALSE;
    return memcmp(remoteBytes, g_pNtUserSetWDA, 16) != 0;
}

static void RecordFailedHwnd(HWND hwnd, DWORD pid = 0, BOOL apcDelivered = FALSE)
{
    for (int i = 0; i < g_failedCount; i++) {
        if (g_failedEntries[i].hwnd == hwnd) {
            g_failedEntries[i].retries++;
            if (pid != 0) g_failedEntries[i].pid = pid;
            if (apcDelivered) g_failedEntries[i].apcDeliveredEver = TRUE;
            DWORD aff = 0;
            DWORD targetPid = g_failedEntries[i].pid;
            if (targetPid == 0) GetWindowThreadProcessId(hwnd, &targetPid);
            if (g_failedEntries[i].retries >= 3 &&
                g_failedEntries[i].apcDeliveredEver &&
                !g_failedEntries[i].hijackAlerted &&
                IsWindow(hwnd) && IsWindowVisible(hwnd) &&
                GetWindowDisplayAffinity(hwnd, &aff) && aff == 0 &&
                IsRemoteWin32uWdaTampered(targetPid)) {
                g_failedEntries[i].hijackAlerted = TRUE;
                WCHAR exeBase[128] = L"unknown";
                GetExeBaseNameForPid(targetPid, exeBase, 128);
                char detail[160], dedup[192];
                sprintf_s(detail, sizeof(detail), "HWND=0x%llX WDA remained 0 after %d APCs",
                    (unsigned long long)(ULONG_PTR)hwnd, g_failedEntries[i].retries);
                sprintf_s(dedup, sizeof(dedup), "WDA_STUB_%lu_%llX",
                    targetPid, (unsigned long long)(ULONG_PTR)hwnd);
                EmitSecurityAlert("WDA_STUB_HIJACK", targetPid, exeBase, detail, dedup);
            }
            return;
        }
    }
    if (g_failedCount < MAX_FAILED_HWNDS) {
        g_failedEntries[g_failedCount].hwnd = hwnd;
        g_failedEntries[g_failedCount].pid = pid;
        g_failedEntries[g_failedCount].retries = 1;
        g_failedEntries[g_failedCount].apcDeliveredEver = apcDelivered;
        g_failedEntries[g_failedCount].hijackAlerted = FALSE;
        g_failedCount++;
    }
}

static void RecordCloakedHwnd(HWND hwnd, DWORD pid, DWORD tid, DWORD aff)
{
    for (int i = 0; i < g_failedCount; i++) {
        if (g_failedEntries[i].hwnd == hwnd) {
            g_failedEntries[i] = g_failedEntries[g_failedCount - 1];
            g_failedCount--;
            break;
        }
    }
    for (int i = 0; i < g_cloakedCount; i++) {
        if (g_cloaked[i].hwnd == hwnd) {
            g_cloaked[i].pid = pid;
            g_cloaked[i].tid = tid;
            g_cloaked[i].appliedAff = aff;
            g_cloaked[i].dropAlertArmedTick = 0;
            return;
        }
    }
    if (g_cloakedCount < MAX_CLOAKED_HWNDS) {
        g_cloaked[g_cloakedCount].hwnd = hwnd;
        g_cloaked[g_cloakedCount].pid  = pid;
        g_cloaked[g_cloakedCount].tid  = tid;
        g_cloaked[g_cloakedCount].appliedAff = aff;
        g_cloaked[g_cloakedCount].dropCount = 0;
        g_cloaked[g_cloakedCount].lastDropTick = 0;
        g_cloaked[g_cloakedCount].dropAlertArmedTick = 0;
        g_cloaked[g_cloakedCount].reapplyGraceUntil = 0;
        g_cloakedCount++;
    }
}

static void HandleCloakedWindowAffinityCheck(CloakedEntry* e)
{
    if (!e || g_bInternalUnmasking) return;
    if (!IsWindow(e->hwnd) || !IsWindowVisible(e->hwnd) || IsIconic(e->hwnd)) {
        e->dropCount = 0;
        e->dropAlertArmedTick = 0;
        return;
    }
    RECT rc = {};
    if (!GetWindowRect(e->hwnd, &rc) || (rc.right - rc.left) < 120 || (rc.bottom - rc.top) < 80) {
        e->dropCount = 0;
        e->dropAlertArmedTick = 0;
        return;
    }

    DWORD aff = 0;
    if (!GetWindowDisplayAffinity(e->hwnd, &aff)) return;
    if (aff != 0) {
        e->dropAlertArmedTick = 0;
        return;
    }

    ULONGLONG now = GetTickCount64();
    DWORD tid = e->tid;
    if (tid == 0) {
        DWORD pidTmp = 0;
        tid = GetWindowThreadProcessId(e->hwnd, &pidTmp);
    }
    BOOL apcSent = (tid != 0) ? QueueWindowAffinityApc(e->hwnd, tid, e->appliedAff ? e->appliedAff : WDA_EXCLUDEFROMCAPTURE) : FALSE;

    if (now < e->reapplyGraceUntil) {
        return;
    }

    if (e->lastDropTick == 0 || (now - e->lastDropTick) > 2500ULL) {
        e->dropCount = 1;
        e->lastDropTick = now;
        e->dropAlertArmedTick = now;
    } else if ((now - e->lastDropTick) >= 35ULL) {
        e->dropCount++;
        e->lastDropTick = now;
    }

    BOOL win32uTampered = (e->dropCount >= 2) ? IsRemoteWin32uWdaTampered(e->pid) : FALSE;
    BOOL sustainedFight = (e->dropCount >= 5 && e->dropAlertArmedTick != 0 && (now - e->dropAlertArmedTick) >= 350ULL);
    if (sustainedFight || (e->dropCount >= 2 && win32uTampered)) {
        WCHAR exeBase[128] = L"unknown";
        GetExeBaseNameForPid(e->pid, exeBase, 128);
        char detail[160], dedup[192];
        sprintf_s(detail, sizeof(detail), "HWND=0x%llX dropped WDA->0 (drops=%d)",
            (unsigned long long)(ULONG_PTR)e->hwnd, e->dropCount);
        sprintf_s(dedup, sizeof(dedup), "WDA_TAMPER_%lu_%llX",
            e->pid, (unsigned long long)(ULONG_PTR)e->hwnd);
        EmitSecurityAlert("WDA_TAMPER", e->pid, exeBase, detail, dedup);

        if (apcSent && (win32uTampered || e->dropCount >= 10)) {
            char detailStub[160], dedupStub[192];
            sprintf_s(detailStub, sizeof(detailStub), "HWND=0x%llX failed WDA reapply after %d APCs",
                (unsigned long long)(ULONG_PTR)e->hwnd, e->dropCount);
            sprintf_s(dedupStub, sizeof(dedupStub), "WDA_STUB_%lu_%llX",
                e->pid, (unsigned long long)(ULONG_PTR)e->hwnd);
            EmitSecurityAlert("WDA_STUB_HIJACK", e->pid, exeBase, detailStub, dedupStub);
        }
    }
}

static BOOL IsSafeTargetWindow(HWND hwnd, DWORD* outPid, DWORD* outTid)
{
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return FALSE;
    if (hwnd == g_hCurtain || hwnd == g_hCanvasWnd || hwnd == g_hInputWnd || hwnd == g_hAlertWnd) return FALSE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return FALSE;

    RECT rc = { 0 };
    if (!GetWindowRect(hwnd, &rc)) return FALSE;
    LONG w = rc.right - rc.left;
    LONG h = rc.bottom - rc.top;
    if (w < 16 || h < 16) return FALSE;
    if ((LONGLONG)w * (LONGLONG)h < 1024LL) return FALSE;

    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid <= 4 || pid == GetCurrentProcessId() || tid == 0) return FALSE;

    WCHAR cls[128] = L"";
    GetClassNameW(hwnd, cls, 127);
    if (wcsncmp(cls, L"Sandbox:", 8) == 0) {
        RecordFailedHwnd(hwnd);
        return FALSE;
    }

    static const wchar_t* skipClasses[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"Windows.UI.Core.CoreWindow", L"NotifyIconOverflowWindow",
        L"TaskListThumbnailWnd", L"MultitaskingViewFrame", L"Xaml_WindowedPopupClass",
        L"ForegroundStaging", L"EdgeUiInputTopWndClass", L"ImmersiveLauncher",
        L"LivePreview", L"tooltips_class32", L"SysShadow"
    };
    for (size_t i = 0; i < sizeof(skipClasses) / sizeof(skipClasses[0]); i++) {
        if (_wcsicmp(cls, skipClasses[i]) == 0) {
            RecordFailedHwnd(hwnd);
            return FALSE;
        }
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return FALSE;

    USHORT procMach = 0, nativeMach = 0;
    BOOL wowOk = g_pIsWow64Process2 && g_pIsWow64Process2(hProc, &procMach, &nativeMach);
    if (!wowOk || procMach != IMAGE_FILE_MACHINE_UNKNOWN) {
        CloseHandle(hProc);
        RecordFailedHwnd(hwnd);
        return FALSE;
    }

    WCHAR img[MAX_PATH] = L"";
    DWORD imgLen = MAX_PATH;
    BOOL hasImg = QueryFullProcessImageNameW(hProc, 0, img, &imgLen);
    CloseHandle(hProc);
    if (!hasImg) return FALSE;

    UpperCaseWStr(img);

    static const wchar_t* skipImages[] = {
        L"CSRSS.EXE", L"DWM.EXE", L"WINLOGON.EXE", L"LSASS.EXE", L"SMSS.EXE",
        L"SERVICES.EXE", L"SVCHOST.EXE", L"SIHOST.EXE", L"CTFMON.EXE",
        L"STARTMENUEXPERIENCEHOST.EXE", L"SEARCHHOST.EXE", L"TEXTINPUTHOST.EXE",
        L"SHELLEXPERIENCEHOST.EXE", L"LOCKAPP.EXE", L"SCREENCLIPPINGHOST.EXE",
        L"SNIPPINGTOOL.EXE", L"MSMPENG.EXE", L"SECURITYHEALTHSYSTRAY.EXE",
        L"SBIE", L"SANDMAN.EXE", L"EASYANTICHEAT", L"BATTLEYE",
        L"VGC.EXE", L"GAMEGUARD", L"\\STEAMLIBRARY\\", L"STEAM.EXE", L"EPICGAMESLAUNCHER.EXE",
        L"WEGAME", L"TENPROTECT", L"ACE-",
        L"GHOSTSWAP"
    };
    for (size_t i = 0; i < sizeof(skipImages) / sizeof(skipImages[0]); i++) {
        if (wcsstr(img, skipImages[i])) {
            RecordFailedHwnd(hwnd);
            return FALSE;
        }
    }

    if (!VerifyWin32uMappedAtSameBase(pid)) {
        RecordFailedHwnd(hwnd);
        return FALSE;
    }

    *outPid = pid;
    *outTid = tid;
    return TRUE;
}

static int FindPendingCloak(HWND hwnd)
{
    for (int i = 0; i < g_pendingCount; i++) {
        if (g_pendingCloak[i].hwnd == hwnd) return i;
    }
    return -1;
}

static void UpsertPendingCloak(HWND hwnd, DWORD pid, DWORD tid, ULONGLONG tick, BOOL triedFallback)
{
    int idx = FindPendingCloak(hwnd);
    if (idx >= 0) {
        g_pendingCloak[idx].pid = pid;
        g_pendingCloak[idx].tid = tid;
        g_pendingCloak[idx].queuedTick = tick;
        g_pendingCloak[idx].triedFallback = triedFallback;
        g_pendingCloak[idx].apcSentCount++;
        return;
    }
    if (g_pendingCount < MAX_PENDING_CLOAK) {
        g_pendingCloak[g_pendingCount].hwnd = hwnd;
        g_pendingCloak[g_pendingCount].pid = pid;
        g_pendingCloak[g_pendingCount].tid = tid;
        g_pendingCloak[g_pendingCount].queuedTick = tick;
        g_pendingCloak[g_pendingCount].triedFallback = triedFallback;
        g_pendingCloak[g_pendingCount].apcSentCount = 1;
        g_pendingCount++;
    }
}

static void RemovePendingCloak(int idx)
{
    if (idx < 0 || idx >= g_pendingCount) return;
    g_pendingCloak[idx] = g_pendingCloak[g_pendingCount - 1];
    g_pendingCount--;
}

static void TryCloakSingleWindow(HWND hwnd)
{
    if (!g_bPerWndCloakReady || g_bInternalUnmasking || g_bCloakSweeping) return;
    if (!hwnd || hwnd == g_hCurtain || hwnd == g_hCanvasWnd || hwnd == g_hInputWnd || hwnd == g_hAlertWnd) return;

    CloakedEntry* existing = FindCloakedEntry(hwnd);
    if (existing) {
        HandleCloakedWindowAffinityCheck(existing);
        return;
    }
    if (IsFailedHwnd(hwnd)) return;

    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;

    RECT rc = { 0 };
    if (!GetWindowRect(hwnd, &rc)) return;
    LONG w = rc.right - rc.left;
    LONG h = rc.bottom - rc.top;
    if (w < 16 || h < 16 || (LONGLONG)w * (LONGLONG)h < 1024LL) return;

    DWORD aff = 0;
    if (!GetWindowDisplayAffinity(hwnd, &aff)) return;
    if (aff != 0) {
        int pidx = FindPendingCloak(hwnd);
        if (pidx >= 0) {
            RecordCloakedHwnd(hwnd, g_pendingCloak[pidx].pid, g_pendingCloak[pidx].tid, aff);
            RemovePendingCloak(pidx);
            PublishMonitoredSeeds();
        } else if (aff == WDA_EXCLUDEFROMCAPTURE) {
            DWORD pid = 0, tid = 0;
            if (IsSafeTargetWindow(hwnd, &pid, &tid)) {
                RecordCloakedHwnd(hwnd, pid, tid, aff);
                PublishMonitoredSeeds();
            }
        }
        return;
    }

    int pidx = FindPendingCloak(hwnd);
    ULONGLONG now = GetTickCount64();
    if (pidx >= 0 && (now - g_pendingCloak[pidx].queuedTick) < 40) return;

    DWORD pid = 0, tid = 0;
    if (!IsSafeTargetWindow(hwnd, &pid, &tid)) return;

    if (QueueWindowAffinityApc(hwnd, tid, WDA_EXCLUDEFROMCAPTURE)) {
        UpsertPendingCloak(hwnd, pid, tid, now, FALSE);
    } else {
        RecordFailedHwnd(hwnd, pid, FALSE);
        if (pidx >= 0) RemovePendingCloak(pidx);
    }
}

static void SweepAndCloakWindows(void)
{
    if (!g_bPerWndCloakReady || g_bInternalUnmasking || g_bCloakSweeping) return;
    g_bCloakSweeping = TRUE;

    /* Prune closed handles */
    int w = 0;
    for (int i = 0; i < g_cloakedCount; i++) {
        if (IsWindow(g_cloaked[i].hwnd)) g_cloaked[w++] = g_cloaked[i];
    }
    g_cloakedCount = w;

    int fw = 0;
    for (int i = 0; i < g_failedCount; i++) {
        if (IsWindow(g_failedEntries[i].hwnd)) g_failedEntries[fw++] = g_failedEntries[i];
    }
    g_failedCount = fw;

    ULONGLONG now = GetTickCount64();
    for (int i = g_pendingCount - 1; i >= 0; i--) {
        HWND ph = g_pendingCloak[i].hwnd;
        if (!IsWindow(ph)) {
            RemovePendingCloak(i);
            continue;
        }
        DWORD aff = 0;
        if (GetWindowDisplayAffinity(ph, &aff) && aff != 0) {
            RecordCloakedHwnd(ph, g_pendingCloak[i].pid, g_pendingCloak[i].tid, aff);
            RemovePendingCloak(i);
        } else if ((now - g_pendingCloak[i].queuedTick) >= 25) {
            if (!g_pendingCloak[i].triedFallback) {
                if (QueueWindowAffinityApc(ph, g_pendingCloak[i].tid, WDA_MONITOR)) {
                    g_pendingCloak[i].triedFallback = TRUE;
                    g_pendingCloak[i].queuedTick = now;
                    g_pendingCloak[i].apcSentCount++;
                } else {
                    RecordFailedHwnd(ph, g_pendingCloak[i].pid, g_pendingCloak[i].apcSentCount > 0);
                    RemovePendingCloak(i);
                }
            } else {
                RecordFailedHwnd(ph, g_pendingCloak[i].pid, g_pendingCloak[i].apcSentCount > 0);
                RemovePendingCloak(i);
            }
        }
    }

    struct Cand { HWND hwnd; DWORD pid; DWORD tid; };
    struct SweepCtx {
        Cand newCands[64];
        int  newCount;
    } ctx = {};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        SweepCtx* c = (SweepCtx*)lp;
        CloakedEntry* existing = FindCloakedEntry(hwnd);
        if (existing) {
            HandleCloakedWindowAffinityCheck(existing);
            return TRUE;
        }
        if (IsFailedHwnd(hwnd) || FindPendingCloak(hwnd) >= 0) return TRUE;

        DWORD pid = 0, tid = 0;
        if (!IsSafeTargetWindow(hwnd, &pid, &tid)) return TRUE;

        DWORD aff = 0;
        if (!GetWindowDisplayAffinity(hwnd, &aff)) return TRUE;
        if (aff == WDA_EXCLUDEFROMCAPTURE) {
            /* Adopt already-cloaked safe windows across service restarts / hard-kill upgrades */
            RecordCloakedHwnd(hwnd, pid, tid, aff);
            return TRUE;
        }
        if (aff != 0) return TRUE;

        if (QueueWindowAffinityApc(hwnd, tid, WDA_EXCLUDEFROMCAPTURE)) {
            if (c->newCount < 64) {
                c->newCands[c->newCount].hwnd = hwnd;
                c->newCands[c->newCount].pid  = pid;
                c->newCands[c->newCount].tid  = tid;
                c->newCount++;
            }
        } else {
            RecordFailedHwnd(hwnd, pid, FALSE);
        }
        return TRUE;
    }, (LPARAM)&ctx);

    if (ctx.newCount > 0) {
        Sleep(20);
        BOOL anyFallback = FALSE;
        for (int i = 0; i < ctx.newCount; i++) {
            DWORD aff = 0;
            if (GetWindowDisplayAffinity(ctx.newCands[i].hwnd, &aff) && aff == WDA_EXCLUDEFROMCAPTURE) {
                RecordCloakedHwnd(ctx.newCands[i].hwnd, ctx.newCands[i].pid, ctx.newCands[i].tid, WDA_EXCLUDEFROMCAPTURE);
            } else if (QueueWindowAffinityApc(ctx.newCands[i].hwnd, ctx.newCands[i].tid, WDA_MONITOR)) {
                anyFallback = TRUE;
            } else {
                RecordFailedHwnd(ctx.newCands[i].hwnd, ctx.newCands[i].pid, TRUE);
            }
        }
        if (anyFallback) {
            Sleep(20);
            for (int i = 0; i < ctx.newCount; i++) {
                if (IsKnownCloaked(ctx.newCands[i].hwnd)) continue;
                DWORD aff = 0;
                if (GetWindowDisplayAffinity(ctx.newCands[i].hwnd, &aff) && aff != 0) {
                    RecordCloakedHwnd(ctx.newCands[i].hwnd, ctx.newCands[i].pid, ctx.newCands[i].tid, aff);
                } else {
                    RecordFailedHwnd(ctx.newCands[i].hwnd, ctx.newCands[i].pid, TRUE);
                }
            }
        }
    }

    PublishMonitoredSeeds();
    g_bCloakSweeping = FALSE;
}

static void RestoreAllCloakedWindows(BOOL permanent)
{
    if (!g_bPerWndCloakReady || (g_cloakedCount == 0 && g_pendingCount == 0)) {
        if (permanent) {
            g_cloakedCount = 0;
            g_pendingCount = 0;
        }
        return;
    }
    BOOL queuedAny = FALSE;
    for (int i = 0; i < g_cloakedCount; i++) {
        if (IsWindow(g_cloaked[i].hwnd)) {
            if (QueueWindowAffinityApc(g_cloaked[i].hwnd, g_cloaked[i].tid, WDA_NONE)) {
                queuedAny = TRUE;
            }
        }
    }
    for (int i = 0; i < g_pendingCount; i++) {
        if (IsWindow(g_pendingCloak[i].hwnd)) {
            if (QueueWindowAffinityApc(g_pendingCloak[i].hwnd, g_pendingCloak[i].tid, WDA_NONE)) {
                queuedAny = TRUE;
            }
        }
    }
    if (queuedAny) {
        ULONGLONG t0 = GetTickCount64();
        while ((GetTickCount64() - t0) < 150) {
            BOOL anyLeft = FALSE;
            for (int i = 0; i < g_cloakedCount; i++) {
                if (IsWindow(g_cloaked[i].hwnd) && IsWindowVisible(g_cloaked[i].hwnd)) {
                    DWORD aff = 0;
                    if (GetWindowDisplayAffinity(g_cloaked[i].hwnd, &aff) && aff != 0) {
                        anyLeft = TRUE;
                        break;
                    }
                }
            }
            if (!anyLeft) {
                for (int i = 0; i < g_pendingCount; i++) {
                    if (IsWindow(g_pendingCloak[i].hwnd) && IsWindowVisible(g_pendingCloak[i].hwnd)) {
                        DWORD aff = 0;
                        if (GetWindowDisplayAffinity(g_pendingCloak[i].hwnd, &aff) && aff != 0) {
                            anyLeft = TRUE;
                            break;
                        }
                    }
                }
            }
            if (!anyLeft) break;
            Sleep(5);
        }
    }
    if (permanent) {
        g_cloakedCount = 0;
        g_pendingCount = 0;
        PublishMonitoredSeeds();
    }
}

static void ReapplyAllCloakedWindows(void)
{
    if (!g_bPerWndCloakReady) return;
    ULONGLONG graceUntil = GetTickCount64() + 500ULL;
    for (int i = 0; i < g_cloakedCount; i++) {
        g_cloaked[i].dropCount = 0;
        g_cloaked[i].dropAlertArmedTick = 0;
        g_cloaked[i].reapplyGraceUntil = graceUntil;
        if (IsWindow(g_cloaked[i].hwnd)) {
            QueueWindowAffinityApc(g_cloaked[i].hwnd, g_cloaked[i].tid, g_cloaked[i].appliedAff);
        }
    }
}

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

/* -------------------------------------------------------------------------- */
/* v4.2 CoW + Inline-Hook + Unsigned-DLL + GhostSwap IPC Background Sentinel  */
/* -------------------------------------------------------------------------- */
struct SentinelWatchTarget {
    WCHAR modName[32];
    char  funcName[48];
    DWORD rva;
    BYTE  cleanBytes[16];
};

static std::vector<SentinelWatchTarget> g_watchTargets;

static void AddWatchTarget(HMODULE hMod, const WCHAR* modName, const char* funcName, const void* funcAddr)
{
    if (!hMod || !modName || !funcName || !funcAddr) return;
    MODULEINFO mi = {};
    if (!GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi))) return;
    const BYTE* base = (const BYTE*)mi.lpBaseOfDll;
    const BYTE* addr = (const BYTE*)funcAddr;
    if (addr < base || (addr + 16) > (base + mi.SizeOfImage)) return;

    DWORD rva = (DWORD)(addr - base);
    for (const auto& t : g_watchTargets) {
        if (_wcsicmp(t.modName, modName) == 0 && t.rva == rva) return;
    }

    SentinelWatchTarget item = {};
    wcsncpy_s(item.modName, modName, _TRUNCATE);
    strncpy_s(item.funcName, funcName, _TRUNCATE);
    item.rva = rva;
    memcpy(item.cleanBytes, addr, 16);
    g_watchTargets.push_back(item);
}

static void InitSentinelWatchTargets(void)
{
    g_watchTargets.clear();

    if (g_hWin32uLocal && g_pNtUserSetWDA) {
        AddWatchTarget(g_hWin32uLocal, L"win32u.dll", "NtUserSetWindowDisplayAffinity", g_pNtUserSetWDA);
    }

    HMODULE hDxgi = GetModuleHandleW(L"dxgi.dll");
    if (!hDxgi) hDxgi = LoadLibraryW(L"dxgi.dll");
    if (hDxgi) {
        AddWatchTarget(hDxgi, L"dxgi.dll", "CreateDXGIFactory",  (const void*)GetProcAddress(hDxgi, "CreateDXGIFactory"));
        AddWatchTarget(hDxgi, L"dxgi.dll", "CreateDXGIFactory1", (const void*)GetProcAddress(hDxgi, "CreateDXGIFactory1"));
        AddWatchTarget(hDxgi, L"dxgi.dll", "CreateDXGIFactory2", (const void*)GetProcAddress(hDxgi, "CreateDXGIFactory2"));

        if (g_pSwapChain) {
            void** scVt = *(void***)g_pSwapChain;
            if (scVt) {
                AddWatchTarget(hDxgi, L"dxgi.dll", "IDXGISwapChain::Present",       scVt[8]);
                AddWatchTarget(hDxgi, L"dxgi.dll", "IDXGISwapChain::GetBuffer",     scVt[9]);
                AddWatchTarget(hDxgi, L"dxgi.dll", "IDXGISwapChain::ResizeBuffers", scVt[13]);
                AddWatchTarget(hDxgi, L"dxgi.dll", "IDXGISwapChain1::Present1",     scVt[22]);
            }
        }

        IDXGIFactory2* fac2 = nullptr;
        typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
        auto pCreateFac1 = (PFN_CreateDXGIFactory1)GetProcAddress(hDxgi, "CreateDXGIFactory1");
        if (pCreateFac1 && SUCCEEDED(pCreateFac1(IID_PPV_ARGS(&fac2))) && fac2) {
            void** facVt = *(void***)fac2;
            if (facVt) {
                AddWatchTarget(hDxgi, L"dxgi.dll", "IDXGIFactory2::CreateSwapChainForHwnd",        facVt[15]);
                AddWatchTarget(hDxgi, L"dxgi.dll", "IDXGIFactory2::CreateSwapChainForComposition", facVt[24]);
            }
            fac2->Release();
        }
    }

    HMODULE hD3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!hD3d11) hD3d11 = LoadLibraryW(L"d3d11.dll");
    if (hD3d11) {
        AddWatchTarget(hD3d11, L"d3d11.dll", "D3D11CreateDevice",             (const void*)GetProcAddress(hD3d11, "D3D11CreateDevice"));
        AddWatchTarget(hD3d11, L"d3d11.dll", "D3D11CreateDeviceAndSwapChain", (const void*)GetProcAddress(hD3d11, "D3D11CreateDeviceAndSwapChain"));

        if (g_pD3DCtx) {
            void** ctxVt = *(void***)g_pD3DCtx;
            if (ctxVt) {
                AddWatchTarget(hD3d11, L"d3d11.dll", "ID3D11DeviceContext::Map",                   ctxVt[14]);
                AddWatchTarget(hD3d11, L"d3d11.dll", "ID3D11DeviceContext::CopySubresourceRegion", ctxVt[46]);
                AddWatchTarget(hD3d11, L"d3d11.dll", "ID3D11DeviceContext::CopyResource",          ctxVt[47]);
            }
        }
    }
}

static LONG VerifyFileAuthenticode(const WCHAR* filePath, std::unordered_map<std::wstring, LONG>& cache)
{
    WCHAR upper[MAX_PATH] = L"";
    wcsncpy_s(upper, filePath, _TRUNCATE);
    UpperCaseWStr(upper);
    std::wstring key(upper);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    WINTRUST_FILE_INFO fileData = {};
    fileData.cbStruct = sizeof(fileData);
    fileData.pcwszFilePath = filePath;

    GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wt = {};
    wt.cbStruct = sizeof(wt);
    wt.dwUIChoice = WTD_UI_NONE;
    wt.fdwRevocationChecks = WTD_REVOKE_NONE;
    wt.dwUnionChoice = WTD_CHOICE_FILE;
    wt.pFile = &fileData;
    wt.dwStateAction = WTD_STATEACTION_VERIFY;
    wt.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    LONG status = WinVerifyTrust(NULL, &policyGuid, &wt);
    wt.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &policyGuid, &wt);

    if (cache.size() > 1024) cache.clear();
    cache[key] = status;
    return status;
}

static void InspectTargetProcessIntegrityAndModules(
    DWORD pid,
    const WCHAR* winDirUpper,
    size_t winDirLen,
    std::unordered_map<std::wstring, LONG>& trustCache)
{
    HANDLE hProcLim = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcLim) return;

    USHORT procMach = 0, nativeMach = 0;
    BOOL wowOk = g_pIsWow64Process2 && g_pIsWow64Process2(hProcLim, &procMach, &nativeMach);
    WCHAR exePath[MAX_PATH] = L"";
    DWORD exeLen = MAX_PATH;
    BOOL gotPath = QueryFullProcessImageNameW(hProcLim, 0, exePath, &exeLen);
    CloseHandle(hProcLim);
    if (!wowOk || procMach != IMAGE_FILE_MACHINE_UNKNOWN || !gotPath) return;

    WCHAR exePathUpper[MAX_PATH] = L"";
    wcsncpy_s(exePathUpper, exePath, _TRUNCATE);
    UpperCaseWStr(exePathUpper);

    const WCHAR* slashBase = wcsrchr(exePath, L'\\');
    const WCHAR* exeBase = slashBase ? (slashBase + 1) : exePath;

    /* Skip our own shield / GhostSwap even if accidentally seeded */
    if (_wcsicmp(exeBase, L"AegisBlackout.exe") == 0 ||
        _wcsicmp(exeBase, L"AegisBlackoutSvc.exe") == 0 ||
        wcsstr(exePathUpper, L"GHOSTSWAP") != NULL) {
        return;
    }

    /* 1. Surgical Anti-Inject DACL Lock */
    HardenTargetProcessDacl(pid, exePathUpper);

    /* Extract executable directory (including trailing backslash) for module origin filtering */
    WCHAR exeDirUpper[MAX_PATH] = L"";
    wcsncpy_s(exeDirUpper, exePathUpper, _TRUNCATE);
    WCHAR* dirEnd = wcsrchr(exeDirUpper, L'\\');
    if (dirEnd) dirEnd[1] = L'\0';
    size_t exeDirLen = wcslen(exeDirUpper);

    /* 2. Module snapshot + CoW / 16-byte prologue check + unsigned external DLL check */
    HANDLE modSnap = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; attempt++) {
        modSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (modSnap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(10);
    }
    if (modSnap == INVALID_HANDLE_VALUE) return;

    const BYTE* rWin32u = NULL; DWORD szWin32u = 0;
    const BYTE* rDxgi   = NULL; DWORD szDxgi   = 0;
    const BYTE* rD3d11  = NULL; DWORD szD3d11  = 0;

    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    if (Module32FirstW(modSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"win32u.dll") == 0) {
                rWin32u = me.modBaseAddr; szWin32u = me.modBaseSize;
            } else if (_wcsicmp(me.szModule, L"dxgi.dll") == 0) {
                rDxgi = me.modBaseAddr; szDxgi = me.modBaseSize;
            } else if (_wcsicmp(me.szModule, L"d3d11.dll") == 0) {
                rD3d11 = me.modBaseAddr; szD3d11 = me.modBaseSize;
            }

            /* Skip main .exe and check only external .dll modules */
            WCHAR modUpper[MAX_PATH] = L"";
            wcsncpy_s(modUpper, me.szExePath, _TRUNCATE);
            UpperCaseWStr(modUpper);
            size_t modLen = wcslen(modUpper);
            if (modLen > 4 && _wcsicmp(modUpper + modLen - 4, L".DLL") == 0) {
                BOOL inWinDir = (winDirLen > 0 && _wcsnicmp(modUpper, winDirUpper, winDirLen) == 0);
                BOOL inExeDir = (exeDirLen > 0 && _wcsnicmp(modUpper, exeDirUpper, exeDirLen) == 0);
                if (!inWinDir && !inExeDir) {
                    LONG trustSt = VerifyFileAuthenticode(me.szExePath, trustCache);
                    if (trustSt != ERROR_SUCCESS) {
                        char dllUtf8[MAX_PATH * 2] = "";
                        WideCharToMultiByte(CP_UTF8, 0, me.szExePath, -1, dllUtf8, sizeof(dllUtf8) - 1, NULL, NULL);
                        char detail[640], dedup[192];
                        sprintf_s(detail, sizeof(detail), "trust=0x%08lX dll=%s", (unsigned long)trustSt, dllUtf8);
                        sprintf_s(dedup, sizeof(dedup), "UDLL_%lu_%ls", pid, me.szModule);
                        EmitSecurityAlert("UNSIGNED_DLL", pid, exeBase, detail, dedup);
                    }
                }
            }
        } while (Module32NextW(modSnap, &me));
    }
    CloseHandle(modSnap);

    if (g_watchTargets.empty()) return;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return;

    for (const auto& t : g_watchTargets) {
        const BYTE* rBase = NULL;
        DWORD rSize = 0;
        if (_wcsicmp(t.modName, L"win32u.dll") == 0) {
            rBase = rWin32u; rSize = szWin32u;
        } else if (_wcsicmp(t.modName, L"dxgi.dll") == 0) {
            rBase = rDxgi; rSize = szDxgi;
        } else if (_wcsicmp(t.modName, L"d3d11.dll") == 0) {
            rBase = rD3d11; rSize = szD3d11;
        }
        if (!rBase || t.rva + 16 > rSize) continue;

        const BYTE* rAddr = rBase + t.rva;
        BYTE remoteBytes[16] = {};
        SIZE_T nRead = 0;
        if (!ReadProcessMemory(hProc, rAddr, remoteBytes, 16, &nRead) || nRead != 16) continue;

        if (memcmp(remoteBytes, t.cleanBytes, 16) != 0) {
            PSAPI_WORKING_SET_EX_INFORMATION ws = {};
            ws.VirtualAddress = (PVOID)((ULONG_PTR)rAddr & ~(ULONG_PTR)0xFFFULL);
            int cow = 0;
            if (QueryWorkingSetEx(hProc, &ws, sizeof(ws))) {
                if (ws.VirtualAttributes.Valid && !ws.VirtualAttributes.Shared) cow = 1;
            }
            char detail[220], dedup[192];
            sprintf_s(detail, sizeof(detail),
                "%ls!%s CoW=%d bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
                t.modName, t.funcName, cow,
                remoteBytes[0], remoteBytes[1], remoteBytes[2], remoteBytes[3],
                remoteBytes[4], remoteBytes[5], remoteBytes[6], remoteBytes[7]);
            sprintf_s(dedup, sizeof(dedup), "IHOOK_%lu_%s", pid, t.funcName);
            EmitSecurityAlert("INLINE_HOOK", pid, exeBase, detail, dedup);
        }
    }

    CloseHandle(hProc);
}

static DWORD WINAPI SentinelThreadProc(LPVOID param)
{
    (void)param;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    WCHAR winDirUpper[MAX_PATH] = L"";
    UINT wlen = GetWindowsDirectoryW(winDirUpper, MAX_PATH - 2);
    if (wlen > 0 && wlen < MAX_PATH - 2) {
        if (winDirUpper[wlen - 1] != L'\\') {
            winDirUpper[wlen++] = L'\\';
            winDirUpper[wlen] = L'\0';
        }
        UpperCaseWStr(winDirUpper);
    } else {
        wcscpy_s(winDirUpper, L"C:\\WINDOWS\\");
        wlen = (UINT)wcslen(winDirUpper);
    }

    std::unordered_map<std::wstring, LONG> trustCache;
    ULONGLONG lastScanTick = 0;

    HANDLE waitHandles[2] = { g_hSentinelStopEv, g_hAlertEvent };
    DWORD waitCount = g_hAlertEvent ? 2 : 1;

    while (true) {
        ULONGLONG now = GetTickCount64();
        if (now - lastScanTick >= 1500ULL) {
            lastScanTick = now;

            std::vector<DWORD> seeds;
            if (g_monitoredCsInit) {
                EnterCriticalSection(&g_monitoredCs);
                seeds.assign(g_monitoredSeeds, g_monitoredSeeds + g_monitoredSeedCount);
                LeaveCriticalSection(&g_monitoredCs);
            }

            struct ProcNode {
                DWORD pid;
                DWORD ppid;
                WCHAR exeName[64];
            };
            std::vector<ProcNode> allProcs;
            std::vector<DWORD> alivePids;

            HANDLE pSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (pSnap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe = {};
                pe.dwSize = sizeof(pe);
                if (Process32FirstW(pSnap, &pe)) {
                    do {
                        alivePids.push_back(pe.th32ProcessID);
                        ProcNode n = {};
                        n.pid = pe.th32ProcessID;
                        n.ppid = pe.th32ParentProcessID;
                        wcsncpy_s(n.exeName, pe.szExeFile, _TRUNCATE);
                        allProcs.push_back(n);
                    } while (Process32NextW(pSnap, &pe));
                }
                CloseHandle(pSnap);
            }

            PruneDeadHardenedDacls(alivePids);

            /* Expand monitored PIDs: cloaked parents + same-exe child processes (2 hops, e.g. Chrome GPU) */
            std::vector<DWORD> monitored = seeds;
            for (int hop = 0; hop < 2; hop++) {
                size_t curSize = monitored.size();
                for (size_t si = 0; si < curSize; si++) {
                    DWORD parentPid = monitored[si];
                    const WCHAR* parentExe = NULL;
                    for (const auto& p : allProcs) {
                        if (p.pid == parentPid) { parentExe = p.exeName; break; }
                    }
                    if (!parentExe) continue;
                    for (const auto& c : allProcs) {
                        if (c.ppid == parentPid && _wcsicmp(c.exeName, parentExe) == 0) {
                            if (std::find(monitored.begin(), monitored.end(), c.pid) == monitored.end()) {
                                monitored.push_back(c.pid);
                            }
                        }
                    }
                }
            }

            for (DWORD mpid : monitored) {
                if (WaitForSingleObject(g_hSentinelStopEv, 0) == WAIT_OBJECT_0) break;
                InspectTargetProcessIntegrityAndModules(mpid, winDirUpper, (size_t)wlen, trustCache);
            }
        }

        DWORD wr = WaitForMultipleObjects(waitCount, waitHandles, FALSE, 1500);
        if (wr == WAIT_OBJECT_0 || wr == WAIT_FAILED) {
            break;
        }
        if (waitCount == 2 && wr == WAIT_OBJECT_0 + 1) {
            CheckExternalAlertIpcFile();
        }
    }

    return 0;
}

static void StartSentinelSystem(void)
{
    EnsureAlertEventCreated();
    InitSentinelWatchTargets();
    g_hSentinelStopEv = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_hSentinelStopEv) {
        g_hSentinelThread = CreateThread(NULL, 0, SentinelThreadProc, NULL, 0, NULL);
        g_bSentinelActive = (g_hSentinelThread != NULL);
    }
}

static void StopSentinelSystem(void)
{
    if (g_hSentinelStopEv) SetEvent(g_hSentinelStopEv);
    if (g_hSentinelThread) {
        WaitForSingleObject(g_hSentinelThread, 2000);
        CloseHandle(g_hSentinelThread);
        g_hSentinelThread = NULL;
    }
    if (g_hSentinelStopEv) {
        CloseHandle(g_hSentinelStopEv);
        g_hSentinelStopEv = NULL;
    }
    if (g_hAlertEvent) {
        CloseHandle(g_hAlertEvent);
        g_hAlertEvent = NULL;
    }
    g_bSentinelActive = FALSE;
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
        if (prev != g_hCanvasWnd && prev != g_hAlertWnd && IsWindowVisible(prev)) {
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
        if (g_hAlertWnd && IsWindowVisible(g_hAlertWnd)) {
            SetWindowDisplayAffinity(g_hAlertWnd, WDA_MONITOR);
            SetWindowPos(g_hAlertWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
    }
}

static void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
    (void)hHook; (void)dwEventThread; (void)dwmsEventTime; (void)idChild;
    if (g_bInternalUnmasking || !g_hCurtain) return;
    if (idObject != OBJID_WINDOW || !hwnd) return;
    if (hwnd == g_hCurtain || hwnd == g_hCanvasWnd || hwnd == g_hAlertWnd) return;

    if (event == EVENT_SYSTEM_FOREGROUND ||
        (event == EVENT_OBJECT_SHOW && (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))) {
        EnsureCurtainIntegrity();
    }
    if (event == EVENT_SYSTEM_FOREGROUND || event == EVENT_OBJECT_CREATE ||
        event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_LOCATIONCHANGE) {
        TryCloakSingleWindow(hwnd);
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
    if (g_hAlertWnd && IsWindowVisible(g_hAlertWnd)) {
        ShowWindow(g_hAlertWnd, SW_HIDE);
    }
    RestoreAllCloakedWindows(FALSE);
    if (g_bHwDrmActive && g_pDCompVisual && g_pDCompDev) {
        g_pDCompVisual->SetContent(nullptr);
        g_pDCompDev->Commit();
    }
    ShowWindow(g_hCurtain, SW_HIDE);
    DwmFlush();
    DwmFlush();
    DwmFlush();

    BitBlt(g_hdcPristine, 0, 0, g_vw, g_vh, hdcScreen, g_vx, g_vy, SRCCOPY | CAPTUREBLT);

    /* Immediately restore HW-DRM + WDA_MONITOR + Per-Window Cloaks BEFORE building preview buffer */
    ReapplyAllCloakedWindows();
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
    g_bInternalUnmasking = TRUE;
    StopSentinelSystem();
    RestoreAllTargetDacls();
    RestoreAllCloakedWindows(TRUE);
    if (g_hWinEventFg)     { UnhookWinEvent(g_hWinEventFg);     g_hWinEventFg = NULL; }
    if (g_hWinEventShow)   { UnhookWinEvent(g_hWinEventShow);   g_hWinEventShow = NULL; }
    if (g_hWinEventLoc)    { UnhookWinEvent(g_hWinEventLoc);    g_hWinEventLoc = NULL; }
    if (g_hWinEventCreate) { UnhookWinEvent(g_hWinEventCreate); g_hWinEventCreate = NULL; }
    if (g_hCurtain) {
        KillTimer(g_hCurtain, TIMER_WATCHDOG_ID);
        KillTimer(g_hCurtain, TIMER_HWDRM_HEARTBEAT);
        KillTimer(g_hCurtain, TIMER_CLOAK_SWEEP_ID);
        KillTimer(g_hCurtain, TIMER_ALERT_HIDE_ID);
    }
    ReleaseHwDrmOverlay();
    ReleaseAndScrubSnapshots();
    if (g_hAlertWnd)  { DestroyWindow(g_hAlertWnd);  g_hAlertWnd = NULL; }
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

    case WM_APP_SECURITY_ALERT:
        ShowOrRefreshAlertHud();
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
        } else if (wParam == TIMER_CLOAK_SWEEP_ID) {
            SweepAndCloakWindows();
        } else if (wParam == TIMER_ALERT_HIDE_ID) {
            KillTimer(hwnd, TIMER_ALERT_HIDE_ID);
            if (g_hAlertWnd) ShowWindow(g_hAlertWnd, SW_HIDE);
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
    InitializeCriticalSection(&g_alertCs);     g_alertCsInit = TRUE;
    InitializeCriticalSection(&g_daclCs);      g_daclCsInit = TRUE;
    InitializeCriticalSection(&g_monitoredCs); g_monitoredCsInit = TRUE;

    StartupLog("AegisBlackout v4.2.0 startup begin (DPI Aware v2, 24x7 HwDRM + Global Special-APC WDA=0x11 + Anti-Inject DACL + CoW/InlineHook/WDA/UnsignedDLL Sentinel, Ctrl+Alt+Q)", TRUE);

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

    /* 3. Active Topmost Watchdog (100ms) + 24x7 HW-DRM Heartbeat (1000ms) + Per-Window Special-APC Cloak (50ms) + Sentinel */
    InitPerWindowCloak();
    SweepAndCloakWindows();
    StartSentinelSystem();
    SetTimer(g_hCurtain, TIMER_WATCHDOG_ID, 100, NULL);
    SetTimer(g_hCurtain, TIMER_CLOAK_SWEEP_ID, 50, NULL);
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
    g_hWinEventLoc = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
    );
    g_hWinEventCreate = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
    );

    char cloakLog[160];
    sprintf_s(cloakLog, sizeof(cloakLog),
        "PerWndCloak: active=%d apcReady=%d cloaked=%d targetAff=0x%X",
        g_bPerWndCloakReady ? 1 : 0,
        g_bPerWndCloakReady ? 1 : 0,
        g_cloakedCount,
        (unsigned)WDA_EXCLUDEFROMCAPTURE);
    StartupLog(cloakLog, FALSE);

    char sentLog[160];
    sprintf_s(sentLog, sizeof(sentLog),
        "Sentinel: active=%d watchTargets=%zu alertEvent=%d",
        g_bSentinelActive ? 1 : 0, g_watchTargets.size(), g_hAlertEvent ? 1 : 0);
    StartupLog(sentLog, FALSE);

    char summary[160];
    sprintf_s(summary, sizeof(summary),
        "Ready: WDA=%d RawInput=%d WinEvent=(%p,%p,%p) Bounds=(%d,%d,%dx%d)",
        affOk, rawOk, (void*)g_hWinEventFg, (void*)g_hWinEventShow, (void*)g_hWinEventLoc, g_vx, g_vy, g_vw, g_vh);
    StartupLog(summary, FALSE);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopSentinelSystem();
    RestoreAllTargetDacls();
    ReleaseHwDrmOverlay();
    ReleaseAndScrubSnapshots();
    if (g_monitoredCsInit) DeleteCriticalSection(&g_monitoredCs);
    if (g_daclCsInit)      DeleteCriticalSection(&g_daclCs);
    if (g_alertCsInit)     DeleteCriticalSection(&g_alertCs);
    CloseHandle(hMutex);
    if (hDesk) CloseDesktop(hDesk);
    return (int)msg.wParam;
}
