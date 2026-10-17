/*
 * AUDIONT test agent (development harness only, not shipped).
 *
 *   nttest /prep   - enable auto logon, register the agent to run at logon,
 *                    register the audiont service, shut down.
 *   nttest         - run C:\AUDIONT\TEST\AUTORUN.BAT, mirror its output to
 *                    COM1, then shut down (unless C:\AUDIONT\TEST\NOSHUT exists).
 */
#include "ucrt.h"

static HANDLE g_com = INVALID_HANDLE_VALUE;
static DWORD g_start;
static DWORD g_comerr;

static void com(const char *s)
{
    DWORD w; int i;
    for (i = 0; i < 20 && g_com == INVALID_HANDLE_VALUE; i++) {
        g_com = CreateFileW(L"COM1", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        if (g_com == INVALID_HANDLE_VALUE) { g_comerr = GetLastError(); Sleep(1000); }
    }
    if (g_com != INVALID_HANDLE_VALUE) WriteFile(g_com, s, u_strlen(s), &w, 0);
}

static void comf(const char *fmt, ...)
{
    char buf[512]; va_list ap;
    va_start(ap, fmt); wvsprintfA(buf, fmt, ap); va_end(ap);
    com(buf);
}

static BOOL setsz(HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *val)
{
    HKEY k; LONG r; DWORD disp;
    r = RegCreateKeyExW(root, path, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, &disp);
    if (r) return FALSE;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (u_wcslen(val) + 1) * 2);
    RegCloseKey(k);
    return r == 0;
}

static BOOL setdw(HKEY root, const WCHAR *path, const WCHAR *name, DWORD v)
{
    HKEY k; LONG r; DWORD disp;
    r = RegCreateKeyExW(root, path, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, &disp);
    if (r) return FALSE;
    r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, 4);
    RegCloseKey(k);
    return r == 0;
}

static void do_shutdown(void)
{
    HANDLE tok; TOKEN_PRIVILEGES tp;
    if (GetFileAttributesW(L"C:\\AUDIONT\\TEST\\NOSHUT") != 0xFFFFFFFF) { com("##AGENT-NOSHUT##\r\n"); return; }
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LookupPrivilegeValueW(0, L"SeShutdownPrivilege", &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, 0, 0);
    }
    /* winlogon clears AutoAdminLogon after use when the password is empty: re-arm it,
       late enough that winlogon's own write has certainly happened */
    while (GetTickCount() - g_start < 25000) Sleep(1000);
    setsz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"AutoAdminLogon", L"1");
    com("##AGENT-SHUTDOWN##\r\n");
    if (g_com != INVALID_HANDLE_VALUE) { FlushFileBuffers(g_com); CloseHandle(g_com); g_com = INVALID_HANDLE_VALUE; }
    ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE, 0);
}

static void prep(void)
{
    SC_HANDLE scm, svc;
    const WCHAR *wl = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    com("##PREP-START##\r\n");
    comf("autologon=%d\r\n",
        setsz(HKEY_LOCAL_MACHINE, wl, L"AutoAdminLogon", L"1") &&
        setsz(HKEY_LOCAL_MACHINE, wl, L"DefaultUserName", L"Administrator") &&
        setsz(HKEY_LOCAL_MACHINE, wl, L"DefaultPassword", L""));
    comf("userinit=%d\r\n", setsz(HKEY_LOCAL_MACHINE, wl, L"Userinit",
        L"userinit,C:\\AUDIONT\\TEST\\NTTEST.EXE"));

    scm = OpenSCManagerW(0, 0, SC_MANAGER_ALL_ACCESS);
    if (scm) {
        svc = CreateServiceW(scm, L"audiont", L"AUDIONT", SERVICE_ALL_ACCESS,
                             SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START, SERVICE_ERROR_NORMAL,
                             L"\\SystemRoot\\System32\\drivers\\audiont.sys", L"Base", 0, 0, 0, 0);
        comf("createservice=%d err=%d\r\n", svc != 0, svc ? 0 : (int)GetLastError());
        if (svc) CloseServiceHandle(svc);
        CloseServiceHandle(scm);
    } else comf("openscm failed %d\r\n", (int)GetLastError());
    setdw(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\audiont\\Parameters",
          L"DebugPorts", 1);
    setdw(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\audiont\\Parameters",
          L"DebugLevel", 3);
    com("##PREP-DONE##\r\n");
}

static void agent(void)
{
    STARTUPINFOW si; PROCESS_INFORMATION pi; HANDLE h; char buf[512]; DWORD n;
    WCHAR cmd[] = L"cmd.exe /c C:\\AUDIONT\\TEST\\AUTORUN.BAT > C:\\AUDIONT\\TEST\\OUT.TXT";
    HANDLE mtx = CreateMutexW(0, TRUE, L"AudiontTestAgent");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return;
    (void)mtx;
    com("##AGENT-START##\r\n");
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_SHOWMINNOACTIVE;
    if (CreateProcessW(0, cmd, 0, 0, FALSE, 0, 0, L"C:\\AUDIONT\\TEST", &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 600000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    } else comf("createprocess failed %d\r\n", (int)GetLastError());
    h = CreateFileW(L"C:\\AUDIONT\\TEST\\OUT.TXT", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        while (ReadFile(h, buf, sizeof(buf) - 1, &n, 0) && n) { buf[n] = 0; com(buf); }
        CloseHandle(h);
    }
    comf("\r\ncomerr=%d\r\n##AGENT-DONE##\r\n", (int)g_comerr);
    do_shutdown();
}

int main(void)
{
    int argc; WCHAR **argv;
    g_start = GetTickCount();
    argv = u_argv(&argc);
    if (argc > 1 && !u_wcsicmp(argv[1], L"/prep")) { prep(); do_shutdown(); }
    else agent();
    return 0;
}

void __stdcall WinMainCRTStartup(void) { ExitProcess(main()); }
