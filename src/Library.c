// need Win10 or the power throttling and memory priority calls below won't even be declared
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <wtsapi32.h>
#include <winternl.h>

#define MAX_HELPERS 64

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

#endif

// snapshots every steamwebhelper.exe descended from steam.exe right now and parks each one.
// returns how many it opened into hOut, caller closes those handles whenever it's done with
// them. called every time MonitorProc sees the app is running, not just the first time, so
// it also doubles as the re-sweep that catches anything steam brings back later
static DWORD ParkDescendants(HANDLE *hOut)
{
    WTS_PROCESS_INFOW *pProcessInfo = {};
    DWORD $ = {};
    WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pProcessInfo, &$);

    DWORD pid[MAX_HELPERS], parent[MAX_HELPERS], n = 0;
    for (DWORD i = {}; i < $ && n < MAX_HELPERS; i++)
        if (CompareStringOrdinal(pProcessInfo[i].pProcessName, -1, L"steamwebhelper.exe", -1, TRUE) == CSTR_EQUAL)
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

    // not every webhelper process is a direct child of steam.exe, some are spawned by other
    // webhelper processes (sandboxed renderers and the like), so we walk the whole chain
    // instead of just checking the immediate parent. n is small (a handful of processes), so
    // the extra passes here cost nothing
    BOOL rooted[MAX_HELPERS] = {};
    for (DWORD i = {}; i < n; i++)
        rooted[i] = parent[i] == GetCurrentProcessId();
    for (DWORD pass = {}; pass < n; pass++)
        for (DWORD i = {}; i < n; i++)
            if (!rooted[i])
                for (DWORD j = {}; j < n; j++)
                    if (rooted[j] && parent[i] == pid[j])
                        rooted[i] = TRUE;

    DWORD cParked = 0;
    for (DWORD i = {}; i < n && cParked < MAX_HELPERS; i++)
        if (rooted[i])
        {
            // PROCESS_TERMINATE is only used by the kill mode build, harmless to request it
            // here too since this file is shared between both builds
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION |
                                               PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                           FALSE, pid[i]);
            if (hProcess)
            {
                Park(hProcess);
                hOut[cParked++] = hProcess;
            }
        }
    return cParked;
}

// this used to be the tail end of WinEventProc itself, blocking forever inside the raw hook
// callback. that's the kind of thing that can blow the stack if steam ever re-enters the
// callback for another matching window while the first call is still parked in here (the
// upstream NoSteamWebHelper fork hit exactly this crash on newer steam builds), so now it's
// its own thread and WinEventProc just kicks it off and returns right away
static DWORD WINAPI MonitorProc(LPVOID lpParameter)
{
#ifdef KILL_MODE
    // killing webhelper alone doesn't stick: steam's own self-healing notices it's gone and
    // relaunches it within a couple seconds, with no registry write we can catch to react to.
    // that self-healing runs on steam's main thread, the same one that owns the window
    // WinEventProc fired for, so suspending it (same as the original project did) stops steam
    // from ever noticing webhelper died at all. this is the one place park mode and kill mode
    // genuinely can't both get what they want: staying dead requires freezing this thread,
    // which is the same thing that made the original project's steam window hang. acceptable
    // trade here specifically because kill mode is for people who don't use the overlay or
    // touch steam's window while gaming anyway
    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, (DWORD)(ULONG_PTR)lpParameter);
#else
    (VOID)lpParameter;
#endif

    HANDLE hParked[MAX_HELPERS] = {};
    DWORD cParked = 0;
    BOOL bParked = FALSE;

#ifndef KILL_MODE
    // park mode only needs to notice park mode isn't a fight against steam the way kill mode
    // is, so there's nothing undoing our work on its own. but with nothing retrimming while
    // parked, a parked process's memory just creeps back up over a long session with no
    // registry write to wake us and catch it. this event lets RegNotifyChangeKeyValue be
    // waited on with a timeout, so there's always somewhere to retrim periodically even when
    // nothing's actually changed
    HANDLE hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
#endif

    // the inner loop below used to just end the whole thread for good the moment
    // RegNotifyChangeKeyValue returned an error, trusting it to never do that. it does: a
    // real test showed watching stop dead after exactly one successful kill, mid game, with
    // no way to recover short of restarting steam. this outer loop means a watch that goes
    // bad just gets reopened and restarted instead of silently giving up for the rest of the
    // session
    for (;;)
    {
        HKEY hKey = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", REG_OPTION_NON_VOLATILE,
                           KEY_NOTIFY | KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        {
            Sleep(1000);
            continue;
        }

#ifdef KILL_MODE
        while (!RegNotifyChangeKeyValue(hKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, NULL, FALSE))
        {
#else
        for (;;)
        {
            if (RegNotifyChangeKeyValue(hKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, hEvent, TRUE) != ERROR_SUCCESS)
                break;

            if (WaitForSingleObject(hEvent, bParked ? 1000 : INFINITE) == WAIT_TIMEOUT)
            {
                for (DWORD i = {}; i < cParked; i++)
                    SetProcessWorkingSetSize(hParked[i], (SIZE_T)-1, (SIZE_T)-1);
                continue;
            }
#endif
            BOOL _ = FALSE;
            RegGetValueW(hKey, NULL, L"RunningAppID", RRF_RT_REG_DWORD, NULL, (PVOID)&_, &((DWORD){sizeof(BOOL)}));

            // re-sweep every time steam says an app is running, not just the first time we
            // notice. games with their own pre-launch menu (settings, server browser,
            // whatever) make steam flip RunningAppID off and back on during the handoff to
            // the real game, and if that happens fast enough we only get woken once, after
            // it's already back on, looking like nothing changed. re-parking unconditionally
            // here means that wakeup still catches and re-kills (or re-trims) whatever steam
            // brought back during the gap, instead of only acting the first time we see the
            // app turn on
            if (_)
            {
#ifdef KILL_MODE
                if (!bParked)
                    SuspendThread(hThread);
#endif
                for (DWORD i = {}; i < cParked; i++)
                    CloseHandle(hParked[i]);
                cParked = ParkDescendants(hParked);
                bParked = TRUE;
            }
            else if (bParked)
            {
#ifdef KILL_MODE
                ResumeThread(hThread);
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
        RegCloseKey(hKey);
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
    CloseHandle(CreateThread(NULL, 0, MonitorProc, (LPVOID)(ULONG_PTR)dwEventThread, 0, NULL));
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
