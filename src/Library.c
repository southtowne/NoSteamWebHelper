// need Win10 or the power throttling and memory priority calls below won't even be declared
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <wtsapi32.h>
#include <winternl.h>

#define MAX_HELPERS 64
#define TRIM_INTERVAL_MS 1000

static DWORD WINAPI ThreadProc(LPVOID lpParameter);

static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static NOTIFYICONDATAW _ = {.cbSize = sizeof(NOTIFYICONDATAW),
                                .uCallbackMessage = WM_USER,
                                .uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP,
                                .szTip = L"Steam WebHelper"};
    static UINT $ = WM_NULL;

    switch (uMsg)
    {
    case WM_CREATE:
        // explorer.exe can restart (crash, user kills it, whatever) and wipes the tray, so
        // we listen for the broadcast it sends after coming back and re-add our icon
        $ = RegisterWindowMessageW(L"TaskbarCreated");
        _.hWnd = hWnd;
        _.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        Shell_NotifyIconW(NIM_ADD, &_);
        break;

    case WM_USER:
        if (lParam == WM_RBUTTONDOWN)
        {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, FALSE, L"On");
            AppendMenuW(hMenu, MF_STRING, TRUE, L"Off");
            SetForegroundWindow(hWnd);

            POINT _ = {};
            GetCursorPos(&_);
            RegSetKeyValueW(
                HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", L"RunningAppID", REG_DWORD,
                &((DWORD){TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD, _.x, _.y,
                                         0, hWnd, NULL)
                              ? TRUE
                              : FALSE}),
                sizeof(DWORD));

            DestroyMenu(hMenu);
        }
        break;

    default:
        if (uMsg == $)
            Shell_NotifyIconW(NIM_ADD, &_);
        break;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// two builds come out of this one file. the default just deprioritizes and trims webhelper
// while a game is running (overlay keeps working), this build just kills it outright instead.
// pick whichever you actually want at compile time with -DKILL_MODE, there's no runtime switch
#ifdef KILL_MODE

static VOID Park(HANDLE hProcess)
{
    TerminateProcess(hProcess, EXIT_SUCCESS);
}

static VOID Unpark(HANDLE hProcess)
{
    // nothing to restore, the process is dead. steam spawns a fresh one when it needs it
    (VOID)hProcess;
}

#else

static volatile LONG g_epoch = 0;

typedef struct
{
    HANDLE h[MAX_HELPERS];
    DWORD n;
    LONG epoch;
} TRIM_CTX;

static VOID Park(HANDLE hProcess)
{
    SetPriorityClass(hProcess, BELOW_NORMAL_PRIORITY_CLASS);
    SetProcessInformation(hProcess, ProcessPowerThrottling,
                           &((PROCESS_POWER_THROTTLING_STATE){PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                                               PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
                                                               PROCESS_POWER_THROTTLING_EXECUTION_SPEED}),
                           sizeof(PROCESS_POWER_THROTTLING_STATE));
    SetProcessInformation(hProcess, ProcessMemoryPriority, &((MEMORY_PRIORITY_INFORMATION){MEMORY_PRIORITY_VERY_LOW}),
                           sizeof(MEMORY_PRIORITY_INFORMATION));
    SetProcessWorkingSetSize(hProcess, (SIZE_T)-1, (SIZE_T)-1);
}

static VOID Unpark(HANDLE hProcess)
{
    // webhelper always starts out normal priority with throttling off, so we can just hardcode
    // the restore instead of having to remember what it was before we touched it
    SetPriorityClass(hProcess, NORMAL_PRIORITY_CLASS);
    SetProcessInformation(hProcess, ProcessPowerThrottling,
                           &((PROCESS_POWER_THROTTLING_STATE){PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                                               PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0}),
                           sizeof(PROCESS_POWER_THROTTLING_STATE));
    SetProcessInformation(hProcess, ProcessMemoryPriority, &((MEMORY_PRIORITY_INFORMATION){MEMORY_PRIORITY_NORMAL}),
                           sizeof(MEMORY_PRIORITY_INFORMATION));
}

// keeps trimming the parked processes' working sets while the game runs. the epoch check is
// how it knows to stop, when the game exits the main loop bumps g_epoch and this thread notices
// on its next wakeup and bails before touching handles that are about to get closed
static DWORD WINAPI RetrimProc(LPVOID lpParameter)
{
    TRIM_CTX *ctx = (TRIM_CTX *)lpParameter;

    while (ctx->epoch == g_epoch)
    {
        Sleep(TRIM_INTERVAL_MS);
        if (ctx->epoch != g_epoch)
            break;
        for (DWORD i = {}; i < ctx->n; i++)
            SetProcessWorkingSetSize(ctx->h[i], (SIZE_T)-1, (SIZE_T)-1);
    }
    HeapFree(GetProcessHeap(), 0, ctx);
    return EXIT_SUCCESS;
}

#endif

// this used to be the tail end of WinEventProc itself, blocking forever inside the raw hook
// callback. that's the kind of thing that can blow the stack if steam ever re-enters the
// callback for another matching window while the first call is still parked in here (the
// upstream NoSteamWebHelper fork hit exactly this crash on newer steam builds), so now it's
// its own thread and WinEventProc just kicks it off and returns right away
static DWORD WINAPI MonitorProc(LPVOID lpParameter)
{
    (VOID)lpParameter;

    HKEY hKey = NULL;
    RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", REG_OPTION_NON_VOLATILE, KEY_NOTIFY | KEY_QUERY_VALUE,
                  &hKey);

    HANDLE hParked[MAX_HELPERS] = {};
    DWORD cParked = 0;
    BOOL bParked = FALSE;

    while (!RegNotifyChangeKeyValue(hKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, NULL, FALSE))
    {
        BOOL _ = FALSE;
        RegGetValueW(hKey, NULL, L"RunningAppID", RRF_RT_REG_DWORD, NULL, (PVOID)&_, &((DWORD){sizeof(BOOL)}));

        if (_ && !bParked)
        {
            WTS_PROCESS_INFOW *pProcessInfo = {};
            DWORD $ = {};
            WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pProcessInfo, &$);

            DWORD pid[MAX_HELPERS], parent[MAX_HELPERS], n = 0;
            for (DWORD i = {}; i < $ && n < MAX_HELPERS; i++)
                if (CompareStringOrdinal(pProcessInfo[i].pProcessName, -1, L"steamwebhelper.exe", -1, TRUE) ==
                    CSTR_EQUAL)
                {
                    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pProcessInfo[i].ProcessId);
                    PROCESS_BASIC_INFORMATION pbi = {};
                    NtQueryInformationProcess(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), NULL);
                    CloseHandle(hProcess);

                    pid[n] = pProcessInfo[i].ProcessId;
                    parent[n] = (DWORD)pbi.InheritedFromUniqueProcessId;
                    n++;
                }
            WTSFreeMemory(pProcessInfo);

            // not every webhelper process is a direct child of steam.exe, some are spawned by
            // other webhelper processes (sandboxed renderers and the like), so we walk the
            // whole chain instead of just checking the immediate parent. n is small (a handful
            // of processes), so the extra passes here cost nothing
            BOOL rooted[MAX_HELPERS] = {};
            for (DWORD i = {}; i < n; i++)
                rooted[i] = parent[i] == GetCurrentProcessId();
            for (DWORD pass = {}; pass < n; pass++)
                for (DWORD i = {}; i < n; i++)
                    if (!rooted[i])
                        for (DWORD j = {}; j < n; j++)
                            if (rooted[j] && parent[i] == pid[j])
                                rooted[i] = TRUE;

            for (DWORD i = {}; i < n && cParked < MAX_HELPERS; i++)
                if (rooted[i])
                {
                    // PROCESS_TERMINATE is only used by the kill mode build, harmless to
                    // request it here too since this file is shared between both builds
                    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION |
                                                       PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                                   FALSE, pid[i]);
                    if (hProcess)
                    {
                        Park(hProcess);
                        hParked[cParked++] = hProcess;
                    }
                }
            bParked = TRUE;

#ifndef KILL_MODE
            TRIM_CTX *ctx = HeapAlloc(GetProcessHeap(), 0, sizeof(TRIM_CTX));
            if (ctx)
            {
                for (DWORD i = {}; i < cParked; i++)
                    ctx->h[i] = hParked[i];
                ctx->n = cParked;
                ctx->epoch = InterlockedIncrement(&g_epoch);
                CloseHandle(CreateThread(NULL, 0, RetrimProc, ctx, 0, NULL));
            }
#endif
        }
        else if (!_ && bParked)
        {
#ifndef KILL_MODE
            InterlockedIncrement(&g_epoch);
#endif
            for (DWORD i = {}; i < cParked; i++)
            {
                Unpark(hParked[i]);
                CloseHandle(hParked[i]);
            }
            cParked = 0;
            bParked = FALSE;
        }
    }
    return EXIT_SUCCESS;
}

// fires for every window steam.exe creates. we only care about the moment the real steam
// window shows up (vguiPopupWindow with an actual title, not one of the dozens of throwaway
// popups steam spawns constantly), that's our signal steam is up and we can start watching it.
// this has to stay quick and return promptly, see the comment on MonitorProc above for why
static VOID CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                                  DWORD dwEventThread, DWORD dwmsEventTime)
{
    WCHAR szClassName[16] = {};
    GetClassNameW(hwnd, szClassName, 16);

    if (CompareStringOrdinal(L"vguiPopupWindow", -1, szClassName, -1, FALSE) != CSTR_EQUAL ||
        GetWindowTextLengthW(hwnd) < 1)
        return;

    CloseHandle(CreateThread(NULL, 0, ThreadProc, (LPVOID)FALSE, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, MonitorProc, NULL, 0, NULL));
}

static DWORD WINAPI ThreadProc(LPVOID lpParameter)
{
    if (lpParameter)
        SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, WinEventProc, GetCurrentProcessId(), (DWORD){},
                        WINEVENT_OUTOFCONTEXT);
    else
        CreateWindowExW(
            WS_EX_LEFT | WS_EX_LTRREADING,
            (LPCWSTR)(ULONG_PTR)RegisterClassW(&((WNDCLASSW){
                .lpszClassName = L" ", .hInstance = GetModuleHandleW(NULL), .lpfnWndProc = (WNDPROC)WndProc})),
            NULL, WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);

    MSG _ = {};
    while (GetMessageW(&_, NULL, (UINT){}, (UINT){}))
    {
        TranslateMessage(&_);
        DispatchMessageW(&_);
    }
    return EXIT_SUCCESS;
}

// steam.exe isn't the only thing in its own install directory that can end up probing for
// umpdc.dll through the same search order trick we're relying on. steamwebhelper.exe lives
// right there too, so without this check every single webhelper process would also load us
// and spin up its own copy of all this, watching itself for no reason
static BOOL IsSteamExe(VOID)
{
    WCHAR path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!len || len >= MAX_PATH)
        return FALSE;

    WCHAR *name = path;
    for (WCHAR *p = path; *p; p++)
        if (*p == L'\\')
            name = p + 1;

    return CompareStringOrdinal(name, -1, L"steam.exe", -1, TRUE) == CSTR_EQUAL;
}

// this is why the output has to be named umpdc.dll. steam.exe probes for the real umpdc.dll
// (a system32 power management dll it doesn't actually need) using the normal windows dll
// search order, which checks the app's own directory first. drop our dll there under that
// name and steam loads this instead, no hooking or injection required
//
// built with -nostdlib so there's no CRT to initialize, which means DllMainCRTStartup is both
// our DllMain and our actual entry point. keep this fast and non-blocking, loader lock rules
// still apply same as any other DllMain
BOOL WINAPI DllMainCRTStartup(HINSTANCE hLibModule, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hLibModule);
        if (IsSteamExe())
            CloseHandle(CreateThread(NULL, 0, ThreadProc, (LPVOID)TRUE, 0, NULL));
    }
    return TRUE;
}
