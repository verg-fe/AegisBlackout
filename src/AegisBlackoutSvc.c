#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <wchar.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

#define SERVICE_NAME            L"AegisBlackoutGuard"
#define EXIT_CODE_USER_SHUTDOWN 0x5A5A

static SERVICE_STATUS        g_svcStatus = { 0 };
static SERVICE_STATUS_HANDLE g_svcStatusHandle = NULL;
static HANDLE                g_hStopEvent = NULL;

static BOOL ResolveSiblingBinary(wchar_t* outExe, size_t exeCap, wchar_t* outDir, size_t dirCap)
{
    wchar_t modPath[MAX_PATH] = { 0 };
    DWORD len = GetModuleFileNameW(NULL, modPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return FALSE;

    wchar_t* slash = wcsrchr(modPath, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';

    if (wcscpy_s(outDir, dirCap, modPath) != 0) return FALSE;
    if (swprintf_s(outExe, exeCap, L"%ls\\AegisBlackout.exe", modPath) < 0) return FALSE;
    return TRUE;
}

static void ReportSvcState(DWORD currentState, DWORD win32ExitCode, DWORD waitHint)
{
    static DWORD checkPoint = 1;
    g_svcStatus.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_svcStatus.dwCurrentState     = currentState;
    g_svcStatus.dwWin32ExitCode    = win32ExitCode;
    g_svcStatus.dwWaitHint         = waitHint;

    if (currentState == SERVICE_START_PENDING) {
        g_svcStatus.dwControlsAccepted = 0;
    } else {
        g_svcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED) {
        g_svcStatus.dwCheckPoint = 0;
    } else {
        g_svcStatus.dwCheckPoint = checkPoint++;
    }

    if (g_svcStatusHandle) {
        SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
    }
}

static DWORD WINAPI SvcCtrlHandler(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
    (void)dwEventType; (void)lpEventData; (void)lpContext;
    switch (dwControl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportSvcState(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        if (g_hStopEvent) SetEvent(g_hStopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static HANDLE AcquireHighIlPrimaryTokenForSession(DWORD sessId)
{
    HANDLE hUserToken = NULL;
    if (!WTSQueryUserToken(sessId, &hUserToken) || !hUserToken) {
        return NULL;
    }

    HANDLE hLaunchToken = NULL;
    TOKEN_LINKED_TOKEN lt = { 0 };
    DWORD retLen = 0;

    if (GetTokenInformation(hUserToken, TokenLinkedToken, &lt, sizeof(lt), &retLen) && lt.LinkedToken) {
        DuplicateTokenEx(lt.LinkedToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hLaunchToken);
        CloseHandle(lt.LinkedToken);
    }
    if (!hLaunchToken) {
        DuplicateTokenEx(hUserToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hLaunchToken);
    }
    CloseHandle(hUserToken);

    if (hLaunchToken) {
        SetTokenInformation(hLaunchToken, TokenSessionId, &sessId, sizeof(sessId));
    }
    return hLaunchToken;
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    (void)argc; (void)argv;

    g_svcStatusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, SvcCtrlHandler, NULL);
    if (!g_svcStatusHandle) return;

    ReportSvcState(SERVICE_START_PENDING, NO_ERROR, 3000);

    wchar_t shieldExe[MAX_PATH] = { 0 };
    wchar_t shieldDir[MAX_PATH] = { 0 };
    if (!ResolveSiblingBinary(shieldExe, MAX_PATH, shieldDir, MAX_PATH)) {
        ReportSvcState(SERVICE_STOPPED, ERROR_PATH_NOT_FOUND, 0);
        return;
    }

    g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_hStopEvent) {
        ReportSvcState(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    ReportSvcState(SERVICE_RUNNING, NO_ERROR, 0);

    while (WaitForSingleObject(g_hStopEvent, 0) != WAIT_OBJECT_0) {
        DWORD sessId = WTSGetActiveConsoleSessionId();
        if (sessId == 0xFFFFFFFF) {
            WaitForSingleObject(g_hStopEvent, 1000);
            continue;
        }

        HANDLE hLaunchToken = AcquireHighIlPrimaryTokenForSession(sessId);
        if (!hLaunchToken) {
            WaitForSingleObject(g_hStopEvent, 1000);
            continue;
        }

        LPVOID pEnv = NULL;
        CreateEnvironmentBlock(&pEnv, hLaunchToken, FALSE);

        STARTUPINFOW si = { 0 };
        si.cb        = sizeof(si);
        si.lpDesktop = L"winsta0\\default";

        PROCESS_INFORMATION pi = { 0 };
        BOOL ok = CreateProcessAsUserW(
            hLaunchToken,
            shieldExe,
            NULL,
            NULL,
            NULL,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT | NORMAL_PRIORITY_CLASS,
            pEnv,
            shieldDir,
            &si,
            &pi
        );

        if (pEnv) DestroyEnvironmentBlock(pEnv);
        CloseHandle(hLaunchToken);

        if (!ok) {
            WaitForSingleObject(g_hStopEvent, 1500);
            continue;
        }

        CloseHandle(pi.hThread);
        HANDLE waitHandles[2] = { g_hStopEvent, pi.hProcess };
        DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (waitRes == WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 0);
            CloseHandle(pi.hProcess);
            break;
        } else if (waitRes == WAIT_OBJECT_0 + 1) {
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);

            if (exitCode == EXIT_CODE_USER_SHUTDOWN) {
                /* User intentionally exited via physical Ctrl+Alt+Shift+End */
                break;
            }
            /* Unexpected kill or crash: immediately self-heal respawn */
            WaitForSingleObject(g_hStopEvent, 150);
        } else {
            CloseHandle(pi.hProcess);
            WaitForSingleObject(g_hStopEvent, 500);
        }
    }

    CloseHandle(g_hStopEvent);
    g_hStopEvent = NULL;
    ReportSvcState(SERVICE_STOPPED, NO_ERROR, 0);
}

int wmain(void)
{
    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };
    StartServiceCtrlDispatcherW(dispatchTable);
    return 0;
}
