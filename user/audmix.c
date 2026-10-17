/*
 * AUDIONT Mixer - volume control for every Windows NT version
 * (NT 3.1 has no mixer API, so this talks to the driver directly
 * through \\.\AudioNT<n>).
 */
#include "ucrt.h"
#include "audiontio.h"

#define COLW      78
#define SLIDER_H  150
#define TOP       34
#define ID_SB     100
#define ID_MUTE   200
#define ID_REC    300
#define ID_CARD   301
#define ID_INFO   302
#define ID_DIAG   303
#define ID_SAVE   304
#define TIMER_ID  1

static const WCHAR *g_Names[AT_LINE_COUNT] = { L"Master", L"Wave", L"MIDI Synth", L"CD Audio", L"Line In",
                                               L"Microphone", L"Aux", L"PC Speaker", L"Rec. Level", L"" };
static HWND g_Wnd, g_Sb[AT_LINE_COUNT], g_Mute[AT_LINE_COUNT], g_Lbl[AT_LINE_COUNT], g_Rec, g_Card;
static HANDLE g_Dev = INVALID_HANDLE_VALUE;
static AUDIONT_LINES g_Lines;
static UINT g_CardNo;
static HFONT g_Font;
static HBRUSH g_Face;
static UINT g_RecItems[8], g_NRec;

static BOOL Ioctl(DWORD code, void *in, DWORD inl, void *out, DWORD outl, DWORD *got)
{
    DWORD dummy;
    if (g_Dev == INVALID_HANDLE_VALUE) return FALSE;
    return DeviceIoControl(g_Dev, code, in, inl, out, outl, got ? got : &dummy, 0);
}

static BOOL OpenCard(UINT n)
{
    WCHAR name[32];
    if (g_Dev != INVALID_HANDLE_VALUE) CloseHandle(g_Dev);
    wsprintfW(name, L"\\\\.\\AudioNT%u", n);
    g_Dev = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    g_CardNo = n;
    return g_Dev != INVALID_HANDLE_VALUE;
}

static void SetFont(HWND h) { SendMessageW(h, WM_SETFONT, (WPARAM)g_Font, 0); }

/* card list: "1: Intel ICH6 HD Audio", "2: USB Audio Device (unplugged)" */
#define HW_TYPE_USB 7   /* HW_USB */
static WCHAR g_CardList[8][128];
static UINT g_NCards;

static UINT ScanCards(WCHAR list[8][128])
{
    UINT n;
    for (n = 0; n < 8; n++) {
        WCHAR name[32];
        AUDIONT_INFO in;
        DWORD got = 0;
        HANDLE t;
        wsprintfW(name, L"\\\\.\\AudioNT%u", n);
        t = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        if (t == INVALID_HANDLE_VALUE) break;
        memset(&in, 0, sizeof(in));
        if (DeviceIoControl(t, IOCTL_AUDIONT_GET_INFO, 0, 0, &in, sizeof(in), &got, 0) && got) {
            WCHAR nm[40], ctl[64];
            UINT k;
            BOOL usb = in.HwType == HW_TYPE_USB;
            for (k = 0; k < 63 && in.Controller[k]; k++) ctl[k] = (WCHAR)(UCHAR)in.Controller[k];
            ctl[k] = 0;
            for (k = 0; got >= sizeof(in) && k < 31 && in.Name[k]; k++) nm[k] = (WCHAR)(UCHAR)in.Name[k];
            nm[k] = 0;
            /* "1: Realtek ALC1220 - Intel 200 Series HD Audio", "2: Logitech USB Headset (USB)" */
            if (!nm[0]) wsprintfW(list[n], L"%u: %s", n + 1, ctl);
            else if (usb) wsprintfW(list[n], L"%u: %s %s", n + 1, nm, !in.Connected ? L"(unplugged)" : L"(USB)");
            else if (lstrcmpW(nm, ctl)) wsprintfW(list[n], L"%u: %s - %s", n + 1, nm, ctl);
            else wsprintfW(list[n], L"%u: %s", n + 1, nm);
        } else {
            wsprintfW(list[n], L"Sound card %u", n + 1);
        }
        CloseHandle(t);
    }
    return n;
}

/* re-reads the card list; a card that came, went or changed rebuilds the window */
static void RescanCards(BOOL force);

static void Refresh(BOOL force)
{
    AUDIONT_LINES ls;
    UINT i;
    if (!Ioctl(IOCTL_AUDIONT_GET_LINES, 0, 0, &ls, sizeof(ls), 0)) return;
    if (!force && ls.ChangeCount == g_Lines.ChangeCount) return;
    g_Lines = ls;
    for (i = 0; i < AT_LINE_COUNT; i++) {
        if (!g_Sb[i]) continue;
        SetScrollPos(g_Sb[i], SB_CTL, 100 - (int)((((ULONG)ls.Line[i].Left + ls.Line[i].Right) / 2) * 100 / 65535), TRUE);
        CheckDlgButton(g_Wnd, ID_MUTE + i, ls.Line[i].Mute ? 1 : 0);
    }
    if (g_Rec) {
        for (i = 0; i < g_NRec; i++)
            if (ls.RecSrc & (1UL << g_RecItems[i])) SendMessageW(g_Rec, CB_SETCURSEL, i, 0);
    }
}

/* line labels: the card's own names (AUDIONT_LINENAMES) where it has them */
static WCHAR g_Short[AT_LINE_COUNT][16], g_Long[AT_LINE_COUNT][32];

static void CopyName(WCHAR *d, UINT max, const char *a, const WCHAR *w)
{
    UINT k;
    for (k = 0; k + 1 < max && (a ? a[k] : w[k]); k++) d[k] = a ? (WCHAR)(UCHAR)a[k] : w[k];
    d[k] = 0;
}

static void LoadNames(void)
{
    AUDIONT_LINENAMES ln;
    UINT i;
    memset(&ln, 0, sizeof(ln));
    if (!Ioctl(IOCTL_AUDIONT_GET_LINENAMES, 0, 0, &ln, sizeof(ln), 0)) memset(&ln, 0, sizeof(ln));
    for (i = 0; i < AT_LINE_COUNT; i++) {
        ln.Short[i][15] = 0; ln.Long[i][31] = 0;
        CopyName(g_Short[i], 16, ln.Short[i][0] ? ln.Short[i] : NULL, g_Names[i]);
        CopyName(g_Long[i], 32, ln.Long[i][0] ? ln.Long[i] : ln.Short[i][0] ? ln.Short[i] : NULL, g_Names[i]);
    }
}

static void Build(void)
{
    UINT i, x = 8, col = 0;
    HWND h;
    RECT rc;
    for (i = 0; i < AT_LINE_COUNT; i++) {
        if (g_Sb[i]) { DestroyWindow(g_Sb[i]); DestroyWindow(g_Mute[i]); DestroyWindow(g_Lbl[i]); }
        g_Sb[i] = g_Mute[i] = g_Lbl[i] = 0;
    }
    if (g_Rec) { DestroyWindow(g_Rec); g_Rec = 0; }
    if (!Ioctl(IOCTL_AUDIONT_GET_LINES, 0, 0, &g_Lines, sizeof(g_Lines), 0)) return;
    LoadNames();
    for (i = 0; i < AT_LINE_COUNT; i++) {
        if (!g_Lines.Line[i].Present || i == AT_LINE_REC_WAVEIN) continue;
        x = 8 + col * COLW;
        g_Lbl[i] = CreateWindowExW(0, L"STATIC", g_Short[i], WS_CHILD | WS_VISIBLE | SS_CENTER,
                                   x, TOP, COLW - 8, 16, g_Wnd, 0, 0, 0);
        g_Sb[i] = CreateWindowExW(0, L"SCROLLBAR", 0, WS_CHILD | WS_VISIBLE | SBS_VERT | WS_TABSTOP,
                                  x + (COLW - 8) / 2 - 9, TOP + 20, 18, SLIDER_H, g_Wnd, (HMENU)(UINT_PTR)(ID_SB + i), 0, 0);
        SetScrollRange(g_Sb[i], SB_CTL, 0, 100, FALSE);
        g_Mute[i] = CreateWindowExW(0, L"BUTTON", L"Mute", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                                    x + 8, TOP + 26 + SLIDER_H, COLW - 16, 18, g_Wnd, (HMENU)(UINT_PTR)(ID_MUTE + i), 0, 0);
        SetFont(g_Lbl[i]); SetFont(g_Mute[i]);
        col++;
    }
    g_NRec = 0;
    if (g_Lines.RecSrcMask) {
        h = CreateWindowExW(0, L"STATIC", L"Record source:", WS_CHILD | WS_VISIBLE, 8, TOP + 56 + SLIDER_H, 100, 16,
                            g_Wnd, 0, 0, 0);
        SetFont(h);
        g_Rec = CreateWindowExW(0, L"COMBOBOX", 0, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                110, TOP + 52 + SLIDER_H, 140, 140, g_Wnd, (HMENU)ID_REC, 0, 0);
        SetFont(g_Rec);
        for (i = 0; i < AT_LINE_COUNT; i++)
            if (g_Lines.RecSrcMask & (1UL << i)) {
                SendMessageW(g_Rec, CB_ADDSTRING, 0, (LPARAM)(i == AT_LINE_MASTER ? L"Stereo Mix" : g_Long[i]));
                g_RecItems[g_NRec++] = i;
            }
    }
    rc.left = 0; rc.top = 0;
    rc.right = 16 + (col < 4 ? 4 : col) * COLW;
    rc.bottom = TOP + 90 + SLIDER_H;
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    SetWindowPos(g_Wnd, 0, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    if (g_CardNo < g_NCards) {        /* full card name in the caption, the combo box is narrow */
        WCHAR t[160];
        const WCHAR *p = g_CardList[g_CardNo];
        while (*p && *p != ':') p++;
        wsprintfW(t, L"AUDIONT Mixer - %s", *p ? p + 2 : g_CardList[g_CardNo]);
        SetWindowTextW(g_Wnd, t);
    }
    Refresh(TRUE);
}

static void SetLine(UINT line, int pos, int mute)
{
    AUDIONT_SET_LINE sl;
    ULONG v;
    sl.Line = line;
    if (pos >= 0) {
        v = (ULONG)(100 - pos) * 65535 / 100;
        sl.Left = sl.Right = (USHORT)v;
    } else { sl.Left = g_Lines.Line[line].Left; sl.Right = g_Lines.Line[line].Right; }
    sl.Mute = mute >= 0 ? mute : g_Lines.Line[line].Mute;
    Ioctl(IOCTL_AUDIONT_SET_LINE, &sl, sizeof(sl), 0, 0, 0);
    Refresh(TRUE);
}

static void ShowInfo(void)
{
    AUDIONT_INFO in; AUDIONT_STATS st;
    WCHAR buf[1024];
    WCHAR ctl[64], cod[64];
    if (!Ioctl(IOCTL_AUDIONT_GET_INFO, 0, 0, &in, sizeof(in), 0)) return;
    memset(&st, 0, sizeof(st));
    Ioctl(IOCTL_AUDIONT_GET_STATS, 0, 0, &st, sizeof(st), 0);
    MultiByteToWideChar(CP_ACP, 0, in.Controller, -1, ctl, 64);
    MultiByteToWideChar(CP_ACP, 0, in.Codec, -1, cod, 64);
    wsprintfW(buf, L"AUDIONT %d.%02d\r\n\r\nController: %s\r\nPCI %04x:%04x (subsystem %04x:%04x) at %d:%d.%d, IRQ %d\r\n"
                   L"Codec: %s\r\nMode: %s\r\nRecording: %s\r\n\r\nInterrupts %d, timer ticks %d, underruns %d",
              (int)(in.Version >> 8), (int)(in.Version & 0xFF), ctl, in.VendorId, in.DeviceId, in.SubVendor,
              in.SubDevice, (int)in.Bus, (int)in.Dev, (int)in.Fn, (int)in.Irq, cod,
              in.InterruptMode ? L"interrupt" : L"timer polling", in.CanRecord ? L"yes" : L"no",
              (int)st.Interrupts, (int)st.Ticks, (int)st.Underruns);
    MessageBoxW(g_Wnd, buf, L"AUDIONT Information", MB_OK | MB_ICONINFORMATION);
}

static void Diagnostics(void)
{
    static char buf[65536];
    WCHAR path[MAX_PATH], msg[MAX_PATH + 64];
    HANDLE f; DWORD got, w;
    GetWindowsDirectoryW(path, MAX_PATH);
    u_wcscat(path, L"\\AUDIONT.TXT");
    f = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    if (Ioctl(IOCTL_AUDIONT_CODEC_DUMP, 0, 0, buf, sizeof(buf), &got)) WriteFile(f, buf, got, &w, 0);
    WriteFile(f, "\r\n---- driver log ----\r\n", 24, &w, 0);
    if (Ioctl(IOCTL_AUDIONT_GET_LOG, 0, 0, buf, sizeof(buf), &got)) WriteFile(f, buf, got, &w, 0);
    CloseHandle(f);
    wsprintfW(msg, L"Diagnostic report written to\r\n%s\r\n\r\nPlease attach it when reporting a problem.", path);
    MessageBoxW(g_Wnd, msg, L"AUDIONT Diagnostics", MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    UINT i;
    switch (m) {
    case WM_CREATE: {
        HWND b;
        WCHAR name[32];
        UINT n;
        g_Wnd = h;
        g_Card = CreateWindowExW(0, L"COMBOBOX", 0, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                 8, 6, 120, 160, h, (HMENU)ID_CARD, 0, 0);
        SendMessageW(g_Card, CB_SETDROPPEDWIDTH, 280, 0);
        SetFont(g_Card);
        (void)name; (void)n;
        g_NCards = ScanCards(g_CardList);
        for (n = 0; n < g_NCards; n++) SendMessageW(g_Card, CB_ADDSTRING, 0, (LPARAM)g_CardList[n]);
        SendMessageW(g_Card, CB_SETCURSEL, 0, 0);
        b = CreateWindowExW(0, L"BUTTON", L"Info", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 136, 5, 56, 22, h, (HMENU)ID_INFO, 0, 0);
        SetFont(b);
        b = CreateWindowExW(0, L"BUTTON", L"Report", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 196, 5, 56, 22, h, (HMENU)ID_DIAG, 0, 0);
        SetFont(b);
        b = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 256, 5, 56, 22, h, (HMENU)ID_SAVE, 0, 0);
        SetFont(b);
        if (!OpenCard(0)) {
            MessageBoxW(h, L"The AUDIONT driver is not running.\r\nNo supported AC'97 or HD Audio device was found, "
                           L"or Windows NT has not been restarted after installation.", L"AUDIONT Mixer", MB_OK | MB_ICONSTOP);
            return -1;
        }
        Build();
        SetTimer(h, TIMER_ID, 1000, 0);
        return 0;
    }
    case WM_VSCROLL: {
        HWND sb = (HWND)lp;
        int pos;
        for (i = 0; i < AT_LINE_COUNT; i++) if (g_Sb[i] == sb) break;
        if (i == AT_LINE_COUNT) break;
        pos = GetScrollPos(sb, SB_CTL);
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= 2; break;
        case SB_LINEDOWN: pos += 2; break;
        case SB_PAGEUP: pos -= 10; break;
        case SB_PAGEDOWN: pos += 10; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = (short)HIWORD(wp); break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = 100; break;
        default: return 0;
        }
        if (pos < 0) pos = 0;
        if (pos > 100) pos = 100;
        SetScrollPos(sb, SB_CTL, pos, TRUE);
        SetLine(i, pos, -1);
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_MUTE && id < ID_MUTE + AT_LINE_COUNT) {
            SetLine(id - ID_MUTE, -1, IsDlgButtonChecked(h, id) ? 1 : 0);
        } else if (id == ID_REC && code == CBN_SELCHANGE) {
            UINT sel = (UINT)SendMessageW(g_Rec, CB_GETCURSEL, 0, 0);
            if (sel < g_NRec) { DWORD mask = 1UL << g_RecItems[sel]; Ioctl(IOCTL_AUDIONT_SET_RECSRC, &mask, 4, 0, 0, 0); }
        } else if (id == ID_CARD && code == CBN_SELCHANGE) {
            OpenCard((UINT)SendMessageW(g_Card, CB_GETCURSEL, 0, 0));
            Build();
        } else if (id == ID_INFO) ShowInfo();
        else if (id == ID_DIAG) Diagnostics();
        else if (id == ID_SAVE) Ioctl(IOCTL_AUDIONT_SAVE, 0, 0, 0, 0, 0);
        return 0;
    }
    case WM_TIMER: {
        static UINT tick;
        Refresh(FALSE);
        if (++tick % 2 == 0) RescanCards(FALSE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        SetTextColor((HDC)wp, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)g_Face;
    case WM_DESTROY:
        Ioctl(IOCTL_AUDIONT_SAVE, 0, 0, 0, 0, 0);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void RescanCards(BOOL force)
{
    WCHAR list[8][128];
    UINT n = ScanCards(list), i, same = n == g_NCards, sel;
    for (i = 0; i < n && same; i++) same = !lstrcmpW(list[i], g_CardList[i]);
    if (same && !force) return;
    sel = g_CardNo < n ? g_CardNo : 0;
    SendMessageW(g_Card, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) {
        lstrcpyW(g_CardList[i], list[i]);
        SendMessageW(g_Card, CB_ADDSTRING, 0, (LPARAM)list[i]);
    }
    g_NCards = n;
    SendMessageW(g_Card, CB_SETCURSEL, sel, 0);
    if (n && OpenCard(sel)) Build();
}

int main(void)
{
    WNDCLASSW wc;
    MSG msg;
    HWND h;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(0);
    wc.hIcon = LoadIconW(0, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AudiontMixer";
    RegisterClassW(&wc);
    g_Font = (HFONT)GetStockObject(ANSI_VAR_FONT);
    g_Face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    h = CreateWindowExW(0, L"AudiontMixer", L"AUDIONT Mixer", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, 0, 0, wc.hInstance, 0);
    if (!h) return 1;
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    while (GetMessageW(&msg, 0, 0, 0)) {
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}

void __stdcall WinMainCRTStartup(void) { ExitProcess(main()); }
