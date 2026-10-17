/*
 * AUDIONT Setup - automatic installer / uninstaller
 *
 *   setup            install (or upgrade) and offer a restart
 *   setup /u         uninstall
 *   options: /s silent (no messages), /r restart without asking,
 *            /mmdrv  use the stock mmdrv.dll instead of audiont.dll
 *            /nomapper  leave the MIDI mapper alone
 *
 * Works on Windows NT 3.1, 3.5, 3.51 and 4.0, and on Windows 2000 / XP
 * (x86 and x64), where audiont.sys is a Plug and Play driver installed from
 * 2kxp\audiont.inf (pnp5.c) and audiont.dll the WinMM driver of 32 and 64
 * bit programs.
 */
#include "ucrt.h"
#include <ddeml.h>
#include "pnp5.h"

#define SVC_NAME   L"audiont"
#define SVC_KEY    L"SYSTEM\\CurrentControlSet\\Services\\audiont"
#define PARAM_KEY  L"SYSTEM\\CurrentControlSet\\Services\\audiont\\Parameters"
#define DRV32_KEY  L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32"
#define TITLE      L"AUDIONT Setup"

static BOOL g_Silent, g_Reboot, g_Mmdrv, g_NoMapper, g_Pnp, g_Mapper;
static WCHAR g_SrcDir[MAX_PATH], g_SysDir[MAX_PATH], g_DrvDir[MAX_PATH];
static WCHAR g_Log[4096];
static BOOL g_NeedReboot;

static void Log(const WCHAR *s)
{
    if (u_wcslen(g_Log) + u_wcslen(s) + 3 < sizeof(g_Log) / 2) { u_wcscat(g_Log, s); u_wcscat(g_Log, L"\r\n"); }
}

static void SaveLog(void)
{
    WCHAR path[MAX_PATH];
    HANDLE f;
    DWORD w;
    GetWindowsDirectoryW(path, MAX_PATH);
    u_wcscat(path, L"\\AUDSETUP.LOG");
    f = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, "\xff\xfe", 2, &w, 0);
    WriteFile(f, g_Log, u_wcslen(g_Log) * 2, &w, 0);
    CloseHandle(f);
}

static int Msg(const WCHAR *text, UINT flags)
{
    SaveLog();
    if (g_Silent) return (flags & MB_YESNO) ? IDNO : IDOK;
    return MessageBoxW(0, text, TITLE, flags);
}

/* ------------------------------------------------------------------ */
static BOOL RegGetSz(HKEY root, const WCHAR *key, const WCHAR *name, WCHAR *out, DWORD cch)
{
    HKEY k; DWORD type, size = cch * 2; LONG r;
    out[0] = 0;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k)) return FALSE;
    r = RegQueryValueExW(k, name, 0, &type, (BYTE *)out, &size);
    RegCloseKey(k);
    if (r || type != REG_SZ) { out[0] = 0; return FALSE; }
    return TRUE;
}

static BOOL RegSetSz(HKEY root, const WCHAR *key, const WCHAR *name, const WCHAR *val)
{
    HKEY k; DWORD disp; LONG r;
    if (RegCreateKeyExW(root, key, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, &disp)) return FALSE;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (u_wcslen(val) + 1) * 2);
    RegCloseKey(k);
    return r == 0;
}

static BOOL RegSetDw(HKEY root, const WCHAR *key, const WCHAR *name, DWORD v)
{
    HKEY k; DWORD disp; LONG r;
    if (RegCreateKeyExW(root, key, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, &disp)) return FALSE;
    r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, 4);
    RegCloseKey(k);
    return r == 0;
}

static void RegDel(HKEY root, const WCHAR *key, const WCHAR *name)
{
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_ALL_ACCESS, &k)) return;
    RegDeleteValueW(k, name);
    RegCloseKey(k);
}

/* ------------------------------------------------------------------ */
static BOOL CopyOne(const WCHAR *name, const WCHAR *dstDir, const WCHAR *dstName)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH], tmp[MAX_PATH], line[MAX_PATH + 64];
    u_wcscpy(src, g_SrcDir); u_wcscat(src, name);
    u_wcscpy(dst, dstDir); u_wcscat(dst, L"\\"); u_wcscat(dst, dstName ? dstName : name);
    if (GetFileAttributesW(src) == 0xFFFFFFFF) {
        wsprintfW(line, L"missing source file %s", src); Log(line);
        return FALSE;
    }
    if (CopyFileW(src, dst, FALSE)) { wsprintfW(line, L"copied %s", dst); Log(line); return TRUE; }
    /* in use: replace at next boot (8.3-safe temporary name, FAT has no long names on NT 3.x) */
    {
        static int seq;
        wsprintfW(tmp, L"%s\\AUDNEW%02d.TMP", dstDir, ++seq);
    }
    if (CopyFileW(src, tmp, FALSE) && MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
        wsprintfW(line, L"%s is in use, it will be replaced at restart", dst); Log(line);
        g_NeedReboot = TRUE;
        return TRUE;
    }
    wsprintfW(line, L"cannot copy %s (error %d)", dst, (int)GetLastError()); Log(line);
    return FALSE;
}

/* the audio DSP firmware: FW\*.* of the package -> %SystemRoot%\System32\drivers\audiont\fw */
static int CopyFw(void)
{
    static const WCHAR *where[] = { L"FW\\", L"..\\FW\\", L"..\\..\\FW\\" };
    WCHAR src[MAX_PATH], dir[MAX_PATH], pat[MAX_PATH], from[MAX_PATH], to[MAX_PATH], line[MAX_PATH + 64];
    WIN32_FIND_DATAW fd;
    HANDLE h = INVALID_HANDLE_VALUE;
    int i, n = 0;
    for (i = 0; i < 3 && h == INVALID_HANDLE_VALUE; i++) {
        u_wcscpy(src, g_SrcDir); u_wcscat(src, where[i]);
        u_wcscpy(pat, src); u_wcscat(pat, L"*.*");
        h = FindFirstFileW(pat, &fd);
    }
    if (h == INVALID_HANDLE_VALUE) { Log(L"no FW folder: audio DSP firmware not installed"); return 0; }
    u_wcscpy(dir, g_DrvDir); u_wcscat(dir, L"\\audiont");
    CreateDirectoryW(dir, 0);
    u_wcscat(dir, L"\\fw");
    CreateDirectoryW(dir, 0);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        u_wcscpy(from, src); u_wcscat(from, fd.cFileName);
        u_wcscpy(to, dir); u_wcscat(to, L"\\"); u_wcscat(to, fd.cFileName);
        if (CopyFileW(from, to, FALSE)) n++;
        else { wsprintfW(line, L"cannot copy %s (error %d)", to, (int)GetLastError()); Log(line); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    wsprintfW(line, L"%d audio DSP firmware files in %s", n, dir);
    Log(line);
    return n;
}

static void DeleteFw(void)
{
    WCHAR dir[MAX_PATH], pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    u_wcscpy(dir, g_DrvDir); u_wcscat(dir, L"\\audiont\\fw");
    u_wcscpy(pat, dir); u_wcscat(pat, L"\\*.*");
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            u_wcscpy(p, dir); u_wcscat(p, L"\\"); u_wcscat(p, fd.cFileName);
            DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
    u_wcscpy(dir, g_DrvDir); u_wcscat(dir, L"\\audiont");
    RemoveDirectoryW(dir);
}

static void DeleteOne(const WCHAR *dir, const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    u_wcscpy(p, dir); u_wcscat(p, L"\\"); u_wcscat(p, name);
    if (GetFileAttributesW(p) == 0xFFFFFFFF) return;
    if (!DeleteFileW(p)) { MoveFileExW(p, 0, MOVEFILE_DELAY_UNTIL_REBOOT); g_NeedReboot = TRUE; }
}

/* ------------------------------------------------------------------ */
static BOOL StopService(SC_HANDLE svc)
{
    SERVICE_STATUS ss;
    int i;
    if (!QueryServiceStatus(svc, &ss) || ss.dwCurrentState == SERVICE_STOPPED) return TRUE;
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    for (i = 0; i < 30; i++) {
        if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState == SERVICE_STOPPED) return TRUE;
        Sleep(100);
    }
    return FALSE;
}

static BOOL InstallService(void)
{
    SC_HANDLE scm, svc;
    BOOL ok = FALSE;
    scm = OpenSCManagerW(0, 0, SC_MANAGER_ALL_ACCESS);
    if (!scm) { Log(L"cannot open the service control manager (administrator rights needed)"); return FALSE; }
    svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (svc) {
        if (!StopService(svc)) g_NeedReboot = TRUE;
        /* display name NULL = unchanged: changing it crashes the NT 3.1 service controller
           (screg.exe access violation) every now and then */
        ok = ChangeServiceConfigW(svc, SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START, SERVICE_ERROR_IGNORE,
                                  L"\\SystemRoot\\System32\\drivers\\audiont.sys", L"Base", 0, 0, 0, 0, 0);
        Log(ok ? L"service updated" : L"ChangeServiceConfig failed");
    } else {
        svc = CreateServiceW(scm, SVC_NAME, L"AUDIONT AC'97 / HD Audio", SERVICE_ALL_ACCESS,
                             SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START, SERVICE_ERROR_IGNORE,
                             L"\\SystemRoot\\System32\\drivers\\audiont.sys", L"Base", 0, 0, 0, 0);
        ok = svc != 0;
        Log(ok ? L"service created" : L"CreateService failed");
    }
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    g_NeedReboot = TRUE;          /* kernel drivers of this start type load at boot */
    return ok;
}

static void RemoveService(void)
{
    SC_HANDLE scm, svc;
    scm = OpenSCManagerW(0, 0, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (svc) {
        StopService(svc);
        Log(DeleteService(svc) ? L"service removed" : L"DeleteService failed");
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

/* ------------------------------------------------------------------ */
/* Drivers32 registration                                              */
/* ------------------------------------------------------------------ */
static void RegisterType(const WCHAR *type, WCHAR *used)
{
    WCHAR name[16], val[MAX_PATH], line[128];
    int i;
    /* already registered? */
    for (i = 0; i < 10; i++) {
        if (i) wsprintfW(name, L"%s%d", type, i); else u_wcscpy(name, type);
        if (RegGetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, name, val, MAX_PATH) && !u_wcsicmp(val, L"audiont.dll")) {
            u_wcscat(used, name); u_wcscat(used, L",");
            return;
        }
    }
    for (i = 0; i < 10; i++) {
        if (i) wsprintfW(name, L"%s%d", type, i); else u_wcscpy(name, type);
        if (!RegGetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, name, val, MAX_PATH) || !val[0]) {
            RegSetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, name, L"audiont.dll");
            u_wcscat(used, name); u_wcscat(used, L",");
            wsprintfW(line, L"registered %s=audiont.dll", name); Log(line);
            return;
        }
    }
}

static void UnregisterDrivers(void)
{
    WCHAR name[16], val[MAX_PATH];
    static const WCHAR *types[] = { L"wave", L"midi", L"aux", L"mixer" };
    int t, i;
    for (t = 0; t < 4; t++)
        for (i = 0; i < 10; i++) {
            if (i) wsprintfW(name, L"%s%d", types[t], i); else u_wcscpy(name, types[t]);
            if (RegGetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, name, val, MAX_PATH) && !u_wcsicmp(val, L"audiont.dll")) {
                RegDel(HKEY_LOCAL_MACHINE, DRV32_KEY, name);
            }
        }
    /* MIDI mapper */
    if (RegGetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"midimapper", val, MAX_PATH) && !u_wcsicmp(val, L"audiont.dll")) {
        WCHAR old[MAX_PATH];
        if (RegGetSz(HKEY_LOCAL_MACHINE, PARAM_KEY, L"OldMidiMapper", old, MAX_PATH) && old[0])
            RegSetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"midimapper", old);
        else
            RegDel(HKEY_LOCAL_MACHINE, DRV32_KEY, L"midimapper");
    }
    Log(L"multimedia driver entries removed");
}

/* ------------------------------------------------------------------ */
/* Program Manager group (DDE)                                         */
/* ------------------------------------------------------------------ */
static HDDEDATA CALLBACK DdeCb(UINT t, UINT f, HCONV c, HSZ a, HSZ b, HDDEDATA d, DWORD x, DWORD y)
{
    (void)t; (void)f; (void)c; (void)a; (void)b; (void)d; (void)x; (void)y;
    return 0;
}

static void Progman(const WCHAR *cmds)
{
    DWORD inst = 0;
    HSZ hs;
    HCONV conv;
    if (DdeInitializeW(&inst, (PFNCALLBACK)DdeCb, APPCMD_CLIENTONLY, 0)) return;
    hs = DdeCreateStringHandleW(inst, L"PROGMAN", CP_WINUNICODE);
    conv = DdeConnect(inst, hs, hs, 0);
    if (conv) {
        DdeClientTransaction((LPBYTE)cmds, (u_wcslen(cmds) + 1) * 2, conv, 0, 0, XTYP_EXECUTE, 5000, 0);
        DdeDisconnect(conv);
    }
    DdeFreeStringHandle(inst, hs);
    DdeUninitialize(inst);
}

/* the program group is rebuilt on every install: Program Manager adds items to an existing group
   (a second install used to show every icon twice) */
static void MakeGroup(void)
{
    WCHAR cmd[1024];
    Progman(L"[DeleteGroup(AUDIONT)]");
    wsprintfW(cmd, L"[CreateGroup(AUDIONT)][AddItem(%s\\audmix.exe,AUDIONT Mixer,%s\\audmix.exe,0)]"
                   L"[AddItem(%s\\audtest.exe,AUDIONT Diagnostics,%s\\audtest.exe,0)]"
                   L"[AddItem(%s\\audsetup.exe /u,Uninstall AUDIONT,%s\\audsetup.exe,1)]",
              g_SysDir, g_SysDir, g_SysDir, g_SysDir, g_SysDir, g_SysDir);
    Progman(cmd);
}

/* ------------------------------------------------------------------ */
static void Restart(void)
{
    HANDLE tok; TOKEN_PRIVILEGES tp;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LookupPrivilegeValueW(0, L"SeShutdownPrivilege", &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, 0, 0);
    }
    ExitWindowsEx(EWX_REBOOT, 0);
}

static int Install(void)
{
    WCHAR used[128] = L"", old[MAX_PATH], msg[5000];
    DWORD ver = GetVersion();
    BOOL ok = TRUE;
    wsprintfW(msg, L"Windows NT %d.%d build %d", (int)(ver & 0xFF), (int)((ver >> 8) & 0xFF), (int)(ver >> 16) & 0x7FFF);
    Log(msg);
    ok &= CopyOne(L"audiont.sys", g_DrvDir, 0);
    ok &= CopyOne(L"audiont.dll", g_SysDir, 0);
    CopyOne(L"audmix.exe", g_SysDir, 0);
    CopyOne(L"audtest.exe", g_SysDir, 0);
    CopyOne(L"setup.exe", g_SysDir, L"audsetup.exe");
    CopyFw();
    if (!ok) goto fail;
    if (!InstallService()) goto fail;
    RegSetDw(HKEY_LOCAL_MACHINE, PARAM_KEY, L"StandardNames", g_Mmdrv ? 1 : 0);
    if (!g_Mmdrv) {
        RegisterType(L"wave", used);
        RegisterType(L"midi", used);
        RegisterType(L"aux", used);
        RegisterType(L"mixer", used);
        if (!g_NoMapper) {
            if (RegGetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"midimapper", old, MAX_PATH) && u_wcsicmp(old, L"audiont.dll"))
                RegSetSz(HKEY_LOCAL_MACHINE, PARAM_KEY, L"OldMidiMapper", old);
            RegSetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"midimapper", L"audiont.dll");
            Log(L"MIDI mapper routed to the AUDIONT synthesizer");
        }
    } else {
        UnregisterDrivers();
        if (!RegGetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"wave", old, MAX_PATH)) RegSetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"wave", L"mmdrv.dll");
        if (!RegGetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"midi", old, MAX_PATH)) RegSetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"midi", L"mmdrv.dll");
        Log(L"compatibility mode: devices served by mmdrv.dll");
    }
    MakeGroup();
    wsprintfW(msg, L"AUDIONT has been installed.\r\n\r\n%s\r\nRestart Windows NT now to start the driver?", g_Log);
    if (g_Reboot || Msg(msg, MB_YESNO | MB_ICONINFORMATION) == IDYES) Restart();
    return 0;
fail:
    wsprintfW(msg, L"AUDIONT installation failed.\r\n\r\n%s", g_Log);
    Msg(msg, MB_OK | MB_ICONSTOP);
    return 1;
}

static int Uninstall(void)
{
    WCHAR msg[5000];
    if (!g_Silent && Msg(L"Remove the AUDIONT sound driver?", MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
    UnregisterDrivers();
    RemoveService();
    DeleteOne(g_DrvDir, L"audiont.sys");
    DeleteFw();
    DeleteOne(g_SysDir, L"audiont.dll");
    DeleteOne(g_SysDir, L"audmix.exe");
    DeleteOne(g_SysDir, L"audtest.exe");
    DeleteOne(g_SysDir, L"audsetup.exe");
    Progman(L"[DeleteGroup(AUDIONT)]");
    wsprintfW(msg, L"AUDIONT has been removed.\r\n\r\n%s\r\nRestart Windows NT now?", g_Log);
    if (g_Reboot || Msg(msg, MB_YESNO | MB_ICONINFORMATION) == IDYES) Restart();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Windows 2000 / XP                                                   */
/* ------------------------------------------------------------------ */
void P5Log(const char *fmt, ...)
{
    char buf[512];
    WCHAR w[512];
    va_list ap;
    int i;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    for (i = 0; buf[i] && i < 511; i++) w[i] = (UCHAR)buf[i];
    w[i] = 0;
    Log(w);
}

static void InfPath(char *out)
{
    static const WCHAR *where[] = { L"2kxp\\audiont.inf", L"audiont.inf", L"..\\audiont.inf" };
    WCHAR full[MAX_PATH], res[MAX_PATH], *part;
    int i, k;
    for (i = 0; i < 3; i++) {
        u_wcscpy(full, g_SrcDir); u_wcscat(full, where[i]);
        if (!GetFullPathNameW(full, MAX_PATH, res, &part)) u_wcscpy(res, full);
        if (GetFileAttributesW(res) != 0xFFFFFFFF) break;
    }
    for (k = 0; res[k] && k < MAX_PATH - 1; k++) out[k] = (char)res[k];
    out[k] = 0;
}

static void Args(WCHAR *a)
{
    a[0] = 0;
    if (g_Silent) u_wcscat(a, L" /s");
    if (g_Mapper) u_wcscat(a, L" /mapper");
}

static int Install5(void)
{
    WCHAR used[128] = L"", msg[5000], a[64], old[MAX_PATH];
    char inf[MAX_PATH];
    BOOL reboot = FALSE;
    int n = 0, rc = 0;
    DWORD ver = GetVersion();
    wsprintfW(msg, L"Windows %d.%d build %d (%s)", (int)(ver & 0xFF), (int)((ver >> 8) & 0xFF),
              (int)(ver >> 16) & 0x7FFF, sizeof(void *) == 8 ? L"x64" : (P5Wow64() ? L"32 bit part on x64" : L"x86"));
    Log(msg);
    /* the WinMM driver and the tools of this architecture (System32 or, for 32 bit, SysWOW64) */
    if (!CopyOne(L"audiont.dll", g_SysDir, 0)) goto fail;
    CopyOne(L"audmix.exe", g_SysDir, 0);
    CopyOne(L"audtest.exe", g_SysDir, 0);
    CopyOne(L"setup.exe", g_SysDir, L"audsetup.exe");
    RegisterType(L"wave", used);
    RegisterType(L"midi", used);
    RegisterType(L"aux", used);
    RegisterType(L"mixer", used);
    if (g_Mapper) {
        if (RegGetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"midimapper", old, MAX_PATH) && u_wcsicmp(old, L"audiont.dll"))
            RegSetSz(HKEY_LOCAL_MACHINE, PARAM_KEY, L"OldMidiMapper", old);
        RegSetSz(HKEY_LOCAL_MACHINE, DRV32_KEY, L"midimapper", L"audiont.dll");
    }
    RegSetDw(HKEY_LOCAL_MACHINE, PARAM_KEY, L"StandardNames", 0);
    if (P5Wow64()) {
        Args(a);
        rc = P5RunNative(g_SrcDir, a);
        if (rc == 3010) reboot = TRUE;
        else if (rc) goto fail;
    } else {
        CopyFw();                                /* the native setup: System32\drivers is not redirected */
        InfPath(inf);
        if (!P5CopyInf(inf)) goto fail;
        n = P5Update(inf, "AUDIONT", "audiont.sys", &reboot);
        if (n < 0) goto fail;
        if (n == 0) Log(L"no sound controller for AUDIONT found now (it is used when one appears)");
    }
    if (g_Pnp) return reboot ? 3010 : 0;
    MakeGroup();
    if (reboot) {
        wsprintfW(msg, L"AUDIONT has been installed.\r\n\r\n%s\r\nRestart Windows now?", g_Log);
        if (g_Reboot || Msg(msg, MB_YESNO | MB_ICONINFORMATION) == IDYES) Restart();
    } else {
        wsprintfW(msg, L"AUDIONT has been installed and is running.\r\n\r\n%s", g_Log);
        Msg(msg, MB_OK | MB_ICONINFORMATION);
    }
    return 0;
fail:
    if (g_Pnp) return 1;
    wsprintfW(msg, L"AUDIONT installation failed.\r\n\r\n%s", g_Log);
    Msg(msg, MB_OK | MB_ICONSTOP);
    return 1;
}

static int Uninstall5(void)
{
    WCHAR msg[5000], a[64];
    if (!g_Pnp && !g_Silent && Msg(L"Remove the AUDIONT sound driver?", MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
    UnregisterDrivers();
    if (P5Wow64()) {
        Args(a);
        u_wcscat(a, L" /u");
        P5RunNative(g_SrcDir, a);
    } else {
        P5Remove("audiont", "AUDIONT");
        DeleteFw();
    }
    DeleteOne(g_SysDir, L"audiont.dll");
    DeleteOne(g_SysDir, L"audmix.exe");
    DeleteOne(g_SysDir, L"audtest.exe");
    DeleteOne(g_SysDir, L"audsetup.exe");
    if (g_Pnp) return 0;
    Progman(L"[DeleteGroup(AUDIONT)]");
    wsprintfW(msg, L"AUDIONT has been removed; Windows uses its own drivers for the sound controllers again.\r\n\r\n%s", g_Log);
    Msg(msg, MB_OK | MB_ICONINFORMATION);
    return 0;
}

int main(void)
{
    int argc, i;
    BOOL un = FALSE;
    WCHAR **argv = u_argv(&argc), *p;
    for (i = 1; i < argc; i++) {
        if (!u_wcsicmp(argv[i], L"/u") || !u_wcsicmp(argv[i], L"-u")) un = TRUE;
        else if (!u_wcsicmp(argv[i], L"/s")) g_Silent = TRUE;
        else if (!u_wcsicmp(argv[i], L"/r")) g_Reboot = TRUE;
        else if (!u_wcsicmp(argv[i], L"/mmdrv")) g_Mmdrv = TRUE;
        else if (!u_wcsicmp(argv[i], L"/nomapper")) g_NoMapper = TRUE;
        else if (!u_wcsicmp(argv[i], L"/mapper")) g_Mapper = TRUE;
        else if (!u_wcsicmp(argv[i], L"/pnp")) { g_Pnp = TRUE; g_Silent = TRUE; }
    }
    GetModuleFileNameW(0, g_SrcDir, MAX_PATH);
    p = g_SrcDir + u_wcslen(g_SrcDir);
    while (p > g_SrcDir && p[-1] != '\\' && p[-1] != ':') p--;
    *p = 0;
    GetSystemDirectoryW(g_SysDir, MAX_PATH);
    u_wcscpy(g_DrvDir, g_SysDir); u_wcscat(g_DrvDir, L"\\drivers");
    if ((GetVersion() & 0x80000000) == 0 && (GetVersion() & 0xFF) >= 5) {
        int r = un ? Uninstall5() : Install5();
        SaveLog();
        return r;
    }
    return un ? Uninstall() : Install();
}

void __stdcall WinMainCRTStartup(void) { ExitProcess(main()); }
