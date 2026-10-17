/*
 * AUDIONT Setup on Windows 2000 / XP (x86 and x64): Plug and Play
 * installation of audiont.sys from 2kxp\audiont.inf.  The sound controllers
 * the INF names get audiont.sys, also over a driver they have now; removal
 * gives them back to Windows (it looks for drivers again).
 *
 * SetupAPI and newdev are loaded at run time (setup.exe also runs on
 * NT 3.1).  A 32 bit program cannot install devices on 64 bit Windows: there
 * the 32 bit setup.exe starts 2kxp\amd64\setup.exe for this part.
 */
#include "ucrt.h"
#include <setupapi.h>
#include "pnp5.h"

#ifndef INSTALLFLAG_FORCE
#define INSTALLFLAG_FORCE 0x00000001
#endif

typedef BOOL (WINAPI *P_UpdateDriver)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);
typedef BOOL (WINAPI *P_CopyOEMInf)(PCSTR, PCSTR, DWORD, DWORD, PSTR, DWORD, PDWORD, PSTR *);
typedef HDEVINFO (WINAPI *P_GetClassDevs)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *P_EnumDeviceInfo)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *P_GetDevRegProp)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef BOOL (WINAPI *P_CallClassInstaller)(DI_FUNCTION, HDEVINFO, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *P_DestroyList)(HDEVINFO);
typedef BOOL (WINAPI *P_UninstallOEMInf)(PCSTR, DWORD, PVOID);
typedef DWORD (WINAPI *P_LocateDevNode)(PDWORD, PCSTR, ULONG);
typedef DWORD (WINAPI *P_Reenumerate)(DWORD, ULONG);

static P_UpdateDriver UpdateDriver;
static P_CopyOEMInf CopyOEMInf;
static P_GetClassDevs GetClassDevs;
static P_EnumDeviceInfo EnumDeviceInfo;
static P_GetDevRegProp GetDevRegProp;
static P_CallClassInstaller CallClassInstaller;
static P_DestroyList DestroyList;
static P_UninstallOEMInf UninstallOEMInf;
static P_LocateDevNode LocateDevNode;
static P_Reenumerate Reenumerate;

BOOL P5Wow64(void)
{
    typedef BOOL (WINAPI *P_IsWow64)(HANDLE, PBOOL);
    P_IsWow64 f = (P_IsWow64)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process");
    BOOL w = FALSE;
    return f != NULL && f(GetCurrentProcess(), &w) && w;
}

static BOOL Load(void)
{
    HMODULE s = LoadLibraryA("setupapi.dll"), n = LoadLibraryA("newdev.dll");
    if (!s || !n) { P5Log("SetupAPI is not available (error %u)", (unsigned)GetLastError()); return FALSE; }
    UpdateDriver = (P_UpdateDriver)GetProcAddress(n, "UpdateDriverForPlugAndPlayDevicesA");
    CopyOEMInf = (P_CopyOEMInf)GetProcAddress(s, "SetupCopyOEMInfA");
    GetClassDevs = (P_GetClassDevs)GetProcAddress(s, "SetupDiGetClassDevsA");
    EnumDeviceInfo = (P_EnumDeviceInfo)GetProcAddress(s, "SetupDiEnumDeviceInfo");
    GetDevRegProp = (P_GetDevRegProp)GetProcAddress(s, "SetupDiGetDeviceRegistryPropertyA");
    CallClassInstaller = (P_CallClassInstaller)GetProcAddress(s, "SetupDiCallClassInstaller");
    DestroyList = (P_DestroyList)GetProcAddress(s, "SetupDiDestroyDeviceInfoList");
    UninstallOEMInf = (P_UninstallOEMInf)GetProcAddress(s, "SetupUninstallOEMInfA");
    LocateDevNode = (P_LocateDevNode)GetProcAddress(s, "CM_Locate_DevNodeA");
    Reenumerate = (P_Reenumerate)GetProcAddress(s, "CM_Reenumerate_DevNode");
    if (!UpdateDriver || !CopyOEMInf || !GetClassDevs || !EnumDeviceInfo || !GetDevRegProp || !CallClassInstaller ||
        !DestroyList) {
        P5Log("SetupAPI lacks a function setup needs");
        return FALSE;
    }
    return TRUE;
}

static BOOL Same(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return FALSE;
    }
    return *a == 0 && *b == 0;
}

static BOOL Contains(const char *t, const char *p)
{
    int n = u_strlen(p), i;
    for (; *t; t++) {
        for (i = 0; i < n && t[i] == p[i]; i++) ;
        if (i == n) return TRUE;
    }
    return FALSE;
}

/* the hardware ids of the [Section] lines "%x% = Install, ID" */
#define MAX_IDS 512
static char g_IdBuf[65536];
static char *g_Ids[MAX_IDS];
static int g_NIds;

static BOOL ReadIds(const char *inf, const char *section)
{
    HANDLE f = CreateFileA(inf, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    DWORD n = 0;
    char *p, *line, want[64];
    BOOL in = FALSE;
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile(f, g_IdBuf, sizeof(g_IdBuf) - 1, &n, 0);
    CloseHandle(f);
    g_IdBuf[n] = 0;
    wsprintfA(want, "[%s]", section);
    g_NIds = 0;
    for (p = g_IdBuf; *p && g_NIds < MAX_IDS;) {
        line = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        while (*p == '\r' || *p == '\n') *p++ = 0;
        if (line[0] == '[') { in = Same(line, want); continue; }
        if (in && line[0] == '%') {
            char *c = line;
            while (*c && *c != ',') c++;
            if (*c == ',') { c++; while (*c == ' ') c++; g_Ids[g_NIds++] = c; }
        }
    }
    return g_NIds > 0;
}

/* an id of the INF in a REG_MULTI_SZ list; a PCI\CC_xxxx id of the INF matches longer class ids */
static BOOL Named(const char *multi)
{
    const char *s;
    int i;
    for (s = multi; *s; s += u_strlen(s) + 1)
        for (i = 0; i < g_NIds; i++)
            if (Same(s, g_Ids[i])) return TRUE;
    return FALSE;
}

BOOL P5CopyInf(const char *inf)
{
    char oem[MAX_PATH];
    int t;
    DWORD e = 0;
    if (!Load()) return FALSE;
    for (t = 0; t < 30; t++) {
        if (CopyOEMInf(inf, 0, SPOST_PATH, 0, oem, sizeof(oem), 0, 0)) {
            P5Log("%s -> %s", inf, oem);
            return TRUE;
        }
        e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) break;
        Sleep(1000);                    /* Windows is looking at the INF folder just now */
    }
    P5Log("cannot copy %s (error %u)", inf, (unsigned)e);
    return FALSE;
}

int P5Update(const char *inf, const char *section, const char *service, BOOL *reboot)
{
    static char hw[1024], cp[1024], desc[256], *seen[32];
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i, type;
    int nseen = 0, done = 0, j;
    if (!Load() || !ReadIds(inf, section)) { P5Log("%s names no devices", inf); return -1; }
    h = GetClassDevs(0, 0, 0, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return -1;
    for (i = 0;; i++) {
        BOOL dup = FALSE, r = FALSE;
        d.cbSize = sizeof(d);
        if (!EnumDeviceInfo(h, i, &d)) break;
        memset(hw, 0, sizeof(hw)); memset(cp, 0, sizeof(cp)); memset(desc, 0, sizeof(desc));
        GetDevRegProp(h, &d, SPDRP_HARDWAREID, &type, (PBYTE)hw, sizeof(hw) - 2, 0);
        GetDevRegProp(h, &d, SPDRP_COMPATIBLEIDS, &type, (PBYTE)cp, sizeof(cp) - 2, 0);
        if (!hw[0] || (!Named(hw) && !Named(cp))) continue;
        for (j = 0; j < nseen; j++) if (Same(seen[j], hw)) dup = TRUE;
        if (dup || nseen >= 32) continue;
        seen[nseen] = (char *)LocalAlloc(LMEM_FIXED, u_strlen(hw) + 1);
        lstrcpyA(seen[nseen], hw);
        GetDevRegProp(h, &d, SPDRP_DEVICEDESC, &type, (PBYTE)desc, sizeof(desc) - 2, 0);
        if (UpdateDriver(0, seen[nseen], inf, INSTALLFLAG_FORCE, &r)) {
            P5Log("%s (%s): %s", seen[nseen], desc, service);
            if (r) *reboot = TRUE;
            done++;
        } else {
            P5Log("%s (%s): error %u", seen[nseen], desc, (unsigned)GetLastError());
        }
        nseen++;
    }
    DestroyList(h);
    return done;
}

int P5Remove(const char *service, const char *provider)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i, type, root;
    char svc[64], desc[256], win[MAX_PATH], pat[MAX_PATH], path[MAX_PATH], head[1024], mark[64];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    int n = 0;
    if (!Load()) return -1;
    h = GetClassDevs(0, 0, 0, DIGCF_ALLCLASSES);
    if (h != INVALID_HANDLE_VALUE) {
        for (i = 0;; i++) {
            d.cbSize = sizeof(d);
            if (!EnumDeviceInfo(h, i, &d)) break;
            memset(svc, 0, sizeof(svc));
            if (!GetDevRegProp(h, &d, SPDRP_SERVICE, &type, (PBYTE)svc, sizeof(svc) - 1, 0) || !Same(svc, service))
                continue;
            memset(desc, 0, sizeof(desc));
            GetDevRegProp(h, &d, SPDRP_DEVICEDESC, &type, (PBYTE)desc, sizeof(desc) - 1, 0);
            if (CallClassInstaller(DIF_REMOVE, h, &d)) { P5Log("%s removed", desc); n++; }
            else P5Log("%s: error %u", desc, (unsigned)GetLastError());
        }
        DestroyList(h);
    }
    /* the INF copies: oemN.inf of this provider */
    wsprintfA(mark, "Provider=%%%s%%", provider);
    GetWindowsDirectoryA(win, sizeof(win));
    wsprintfA(pat, "%s\\inf\\oem*.inf", win);
    f = FindFirstFileA(pat, &fd);
    while (f != INVALID_HANDLE_VALUE) {
        HANDLE fh;
        DWORD got = 0;
        wsprintfA(path, "%s\\inf\\%s", win, fd.cFileName);
        fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (fh != INVALID_HANDLE_VALUE) {
            ReadFile(fh, head, sizeof(head) - 1, &got, 0);
            CloseHandle(fh);
            head[got] = 0;
            if (got && Contains(head, mark)) {
                if (UninstallOEMInf) UninstallOEMInf(fd.cFileName, 1, 0);
                else {
                    int l = u_strlen(path);
                    DeleteFileA(path);
                    path[l - 3] = 'p';
                    DeleteFileA(path);
                }
                P5Log("%s removed", fd.cFileName);
            }
        }
        if (!FindNextFileA(f, &fd)) { FindClose(f); break; }
    }
    if (LocateDevNode && Reenumerate && LocateDevNode(&root, 0, 0) == 0) Reenumerate(root, 1);
    return n;
}

/* 64 bit Windows: the 64 bit setup.exe (2kxp\amd64) does the Plug and Play part */
int P5RunNative(const WCHAR *dir, const WCHAR *args)
{
    WCHAR exe[MAX_PATH], line[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD rc = 1;
    wsprintfW(exe, L"%s2kxp\\amd64\\setup.exe", dir);
    if (GetFileAttributesW(exe) == 0xFFFFFFFF) {
        P5Log("64 bit Windows: 2kxp\\amd64\\setup.exe is missing");
        return 1;
    }
    wsprintfW(line, L"\"%s\" %s /pnp", exe, args);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, line, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
        P5Log("cannot start the 64 bit setup (error %u)", (unsigned)GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)rc;
}
