/*
 * AUDIONT diagnostics / test tool (console)
 *
 *   attest info                      list multimedia devices and AUDIONT status
 *   attest tone F MS RATE BITS CH [DEV] [AMP%]   play a sine tone
 *   attest seq                       standard tone sequence (harness analysis)
 *   attest mix                       two simultaneous streams (440 Hz + 1000 Hz)
 *   attest rec MS RATE BITS CH       record and print level statistics
 *   attest midi [DEV]                play a short MIDI phrase on the synth
 *   attest pos                       position accuracy test
 *   attest reset                     waveOutReset / pause / restart test
 *   attest log                       dump the driver log
 *   attest dump                      dump codec registers / widgets
 *   attest lines                     show mixer lines
 *   attest setline LINE L R MUTE     set a mixer line
 *   attest tonefmt KIND F MS RATE BITS CH   tone in another format: KIND 1 = IEEE float,
 *                                    2 = extensible PCM, 3 = extensible float (what VLC,
 *                                    browsers and DirectSound / WASAPI layers open)
 *   attest mxvol LINE VOL            the volume slider of a mixer line through the mixer
 *                                    API: LINE master / wave / synth, VOL 0..65535
 */
#include "ucrt.h"
#include "audiontio.h"

static int g_fail;

/* sine with 4096-entry table + linear interpolation, amplitude 32767,
   phase in 1/2^32 of a cycle */
static short isin(unsigned long ph)
{
    static long tab[4097];
    static int init;
    unsigned long i, f;
    long a, b;
    if (!init) {
        double s = 0, c = 1, d = 2 * 3.14159265358979323846 / 4096.0, sd, cd, t;
        sd = d - d * d * d / 6 + d * d * d * d * d / 120 - d * d * d * d * d * d * d / 5040;
        cd = 1 - d * d / 2 + d * d * d * d / 24 - d * d * d * d * d * d / 720;
        for (i = 0; i <= 4096; i++) {
            tab[i] = (long)(s * 32767.0 * 65536.0);
            t = s * cd + c * sd; c = c * cd - s * sd; s = t;
        }
        init = 1;
    }
    i = ph >> 20; f = (ph >> 4) & 0xFFFF;
    a = tab[i] >> 8; b = tab[i + 1] >> 8;
    return (short)((a + (long)(((__int64)(b - a) * f) >> 16) + 128) >> 8);
}

static void gen(char *buf, int frames, int rate, int bits, int ch, int freq, int amp,
                unsigned long *phase)
{
    unsigned long step = (unsigned long)(((unsigned __int64)freq << 32) / rate);
    int i, c;
    for (i = 0; i < frames; i++) {
        long s = (long)isin(*phase) * amp / 100;
        *phase += step;
        for (c = 0; c < ch; c++) {
            long v = s;
            if (ch == 2 && c == 1 && freq > 0) v = s;   /* same on both channels */
            if (bits == 8) *buf++ = (char)((v >> 8) + 128);
            else if (bits == 16) { *buf++ = (char)v; *buf++ = (char)(v >> 8); }
            else if (bits == 24) { *buf++ = 0; *buf++ = (char)v; *buf++ = (char)(v >> 8); }
            else { *buf++ = 0; *buf++ = 0; *buf++ = (char)v; *buf++ = (char)(v >> 8); }
        }
    }
}

static const char *mmerr(MMRESULT r)
{
    static char b[32];
    wsprintfA(b, "mmerr %d", (int)r);
    return b;
}

#define NBUF 4
static int play_tone(int freq, int ms, int rate, int bits, int ch, int dev, int amp, int verbose)
{
    HWAVEOUT h; WAVEFORMATEX wf; WAVEHDR hdr[NBUF]; MMRESULT r; MMTIME mt;
    int bufFrames = rate / 10, total = (int)((__int64)rate * ms / 1000), done = 0, i, queued = 0;
    int ba = ch * bits / 8;
    unsigned long phase = 0;
    DWORD t0, t1;
    char *mem;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = (WORD)ch; wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = (WORD)bits; wf.nBlockAlign = (WORD)ba; wf.nAvgBytesPerSec = rate * ba;
    r = waveOutOpen(&h, dev, &wf, 0, 0, 0);
    if (r) { u_printf("  waveOutOpen(%d Hz %d bit %d ch) failed: %s\r\n", rate, bits, ch, mmerr(r)); g_fail++; return 0; }
    mem = (char *)GlobalAlloc(GPTR, NBUF * bufFrames * ba);
    memset(hdr, 0, sizeof(hdr));
    t0 = GetTickCount();
    while (done < total || queued) {
        for (i = 0; i < NBUF; i++) {
            if (hdr[i].dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(h, &hdr[i], sizeof(WAVEHDR));
                hdr[i].dwFlags = 0; queued--;
            }
            if (!(hdr[i].dwFlags & WHDR_PREPARED) && done < total) {
                int n = bufFrames < total - done ? bufFrames : total - done;
                hdr[i].lpData = mem + i * bufFrames * ba;
                hdr[i].dwBufferLength = n * ba;
                gen(hdr[i].lpData, n, rate, bits, ch, freq, amp, &phase);
                waveOutPrepareHeader(h, &hdr[i], sizeof(WAVEHDR));
                r = waveOutWrite(h, &hdr[i], sizeof(WAVEHDR));
                if (r) { u_printf("  waveOutWrite failed %s\r\n", mmerr(r)); g_fail++; done = total; break; }
                done += n; queued++;
            }
        }
        Sleep(10);
        if (GetTickCount() - t0 > (DWORD)ms + 5000) { u_printf("  TIMEOUT waiting for buffers\r\n"); g_fail++; break; }
    }
    t1 = GetTickCount();
    mt.wType = TIME_BYTES;
    waveOutGetPosition(h, &mt, sizeof(mt));
    if (verbose)
        u_printf("  tone %d Hz %dms %d/%d/%d: wall %d ms, pos %d bytes (expect %d)\r\n", freq, ms,
                 rate, bits, ch, (int)(t1 - t0), (int)mt.u.cb, total * ba);
    if ((int)mt.u.cb != total * ba) { u_printf("  POSITION MISMATCH\r\n"); g_fail++; }
    waveOutReset(h);
    waveOutClose(h);
    GlobalFree((HGLOBAL)mem);
    return (int)(t1 - t0);
}

/* the same tone in IEEE float / WAVE_FORMAT_EXTENSIBLE (kind 1 - 3), stereo or more channels */
static int play_tone_fmt(int kind, int freq, int ms, int rate, int bits, int ch)
{
    HWAVEOUT h; WAVEHDR hdr[NBUF]; MMRESULT r;
    struct { WAVEFORMATEX f; WORD valid; DWORD mask; GUID sub; } wf;
    static const GUID sub_pcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    static const GUID sub_flt = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    int bufFrames = rate / 10, total = (int)((__int64)rate * ms / 1000), done = 0, i, c, k, queued = 0;
    int ba = ch * bits / 8, flt = kind == 1 || kind == 3;
    unsigned long phase = 0, step = (unsigned long)(((unsigned __int64)freq << 32) / rate);
    DWORD t0;
    char *mem;
    memset(&wf, 0, sizeof(wf));
    wf.f.wFormatTag = kind == 1 ? 3 : 0xFFFE; wf.f.nChannels = (WORD)ch; wf.f.nSamplesPerSec = rate;
    wf.f.wBitsPerSample = (WORD)bits; wf.f.nBlockAlign = (WORD)ba; wf.f.nAvgBytesPerSec = rate * ba;
    if (kind != 1) {
        wf.f.cbSize = 22; wf.valid = (WORD)bits;
        wf.mask = ch == 1 ? 4 : ch == 2 ? 3 : ch == 6 ? 0x3F : 0;
        wf.sub = flt ? sub_flt : sub_pcm;
    }
    r = waveOutOpen(&h, 0, &wf.f, 0, 0, 0);
    if (r) { u_printf("  waveOutOpen(kind %d, %d Hz %d bit %d ch) failed: %s\r\n", kind, rate, bits, ch, mmerr(r)); g_fail++; return 0; }
    mem = (char *)GlobalAlloc(GPTR, NBUF * bufFrames * ba);
    memset(hdr, 0, sizeof(hdr));
    t0 = GetTickCount();
    while (done < total || queued) {
        for (i = 0; i < NBUF; i++) {
            if (hdr[i].dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(h, &hdr[i], sizeof(WAVEHDR));
                hdr[i].dwFlags = 0; queued--;
            }
            if (!(hdr[i].dwFlags & WHDR_PREPARED) && done < total) {
                int n = bufFrames < total - done ? bufFrames : total - done;
                char *p = mem + i * bufFrames * ba;
                for (k = 0; k < n; k++) {
                    long v = (long)isin(phase) / 2;              /* half scale */
                    phase += step;
                    for (c = 0; c < ch; c++) {
                        /* 5.1: the tone on front left / right only (the down-mix keeps their share) */
                        long sv = (ch > 2 && c >= 2) ? 0 : v;
                        if (flt && bits == 32) { float fv = sv / 32768.0f; memcpy(p, &fv, 4); p += 4; }
                        else if (flt) { double dv = sv / 32768.0; memcpy(p, &dv, 8); p += 8; }
                        else if (bits == 16) { *p++ = (char)sv; *p++ = (char)(sv >> 8); }
                        else if (bits == 24) { *p++ = 0; *p++ = (char)sv; *p++ = (char)(sv >> 8); }
                        else { *p++ = 0; *p++ = 0; *p++ = (char)sv; *p++ = (char)(sv >> 8); }
                    }
                }
                hdr[i].lpData = mem + i * bufFrames * ba;
                hdr[i].dwBufferLength = n * ba;
                waveOutPrepareHeader(h, &hdr[i], sizeof(WAVEHDR));
                r = waveOutWrite(h, &hdr[i], sizeof(WAVEHDR));
                if (r) { u_printf("  waveOutWrite failed %s\r\n", mmerr(r)); g_fail++; done = total; break; }
                done += n; queued++;
            }
        }
        Sleep(10);
        if (GetTickCount() - t0 > (DWORD)ms + 5000) { u_printf("  TIMEOUT waiting for buffers\r\n"); g_fail++; break; }
    }
    u_printf("  tonefmt kind %d: %d Hz %d ms %d/%d/%d played\r\n", kind, freq, ms, rate, bits, ch);
    waveOutReset(h);
    waveOutClose(h);
    GlobalFree((HGLOBAL)mem);
    return 1;
}

/* a mixer line's volume slider through the mixer API (what sndvol32 does) */
typedef UINT (WINAPI *PFN_MXOPEN)(HMIXER *, UINT, DWORD_PTR, DWORD_PTR, DWORD);
typedef UINT (WINAPI *PFN_MXLINE)(HMIXEROBJ, MIXERLINEW *, DWORD);
typedef UINT (WINAPI *PFN_MXCTLS)(HMIXEROBJ, MIXERLINECONTROLSW *, DWORD);
typedef UINT (WINAPI *PFN_MXSET)(HMIXEROBJ, MIXERCONTROLDETAILS *, DWORD);
typedef UINT (WINAPI *PFN_MXCLOSE)(HMIXER);
static void cmd_mxvol(const WCHAR *line, int vol)
{
    HMODULE w = GetModuleHandleW(L"WINMM");
    PFN_MXOPEN mopen = w ? (PFN_MXOPEN)GetProcAddress(w, "mixerOpen") : 0;
    PFN_MXLINE mline = w ? (PFN_MXLINE)GetProcAddress(w, "mixerGetLineInfoW") : 0;
    PFN_MXCTLS mctls = w ? (PFN_MXCTLS)GetProcAddress(w, "mixerGetLineControlsW") : 0;
    PFN_MXSET mset = w ? (PFN_MXSET)GetProcAddress(w, "mixerSetControlDetails") : 0;
    PFN_MXCLOSE mclose = w ? (PFN_MXCLOSE)GetProcAddress(w, "mixerClose") : 0;
    HMIXER hm; MIXERLINEW ml; MIXERLINECONTROLSW mlc; MIXERCONTROLW mc; MIXERCONTROLDETAILS md;
    MIXERCONTROLDETAILS_UNSIGNED v[2]; UINT r;
    DWORD type = !u_wcsicmp(line, L"wave") ? MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT :
                 !u_wcsicmp(line, L"synth") ? MIXERLINE_COMPONENTTYPE_SRC_SYNTHESIZER : MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    if (!mopen || !mline || !mctls || !mset || !mclose) { u_printf("  mixer API not available\r\n"); g_fail++; return; }
    if ((r = mopen(&hm, 0, 0, 0, 0)) != 0) { u_printf("  mixerOpen failed %d\r\n", (int)r); g_fail++; return; }
    memset(&ml, 0, sizeof(ml)); ml.cbStruct = sizeof(ml); ml.dwComponentType = type;
    if ((r = mline((HMIXEROBJ)hm, &ml, MIXER_GETLINEINFOF_COMPONENTTYPE)) != 0) {
        u_printf("  no %ls line (%d)\r\n", line, (int)r); g_fail++; mclose(hm); return;
    }
    memset(&mlc, 0, sizeof(mlc)); memset(&mc, 0, sizeof(mc));
    mlc.cbStruct = sizeof(mlc); mlc.dwLineID = ml.dwLineID; mlc.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
    mlc.cControls = 1; mlc.cbmxctrl = sizeof(mc); mlc.pamxctrl = &mc;
    if ((r = mctls((HMIXEROBJ)hm, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE)) != 0) {
        u_printf("  no volume control on %ls (%d)\r\n", line, (int)r); g_fail++; mclose(hm); return;
    }
    memset(&md, 0, sizeof(md));
    v[0].dwValue = v[1].dwValue = (DWORD)vol;
    md.cbStruct = sizeof(md); md.dwControlID = mc.dwControlID; md.cChannels = 2;
    md.cbDetails = sizeof(v[0]); md.paDetails = v;
    r = mset((HMIXEROBJ)hm, &md, MIXER_SETCONTROLDETAILSF_VALUE);
    u_printf("  mixer line \"%ls\" (%ls) volume %d -> %d\r\n", ml.szName, line, vol, (int)r);
    if (r) g_fail++;
    mclose(hm);
}

static void silence(int ms) { Sleep(ms); }

/* mixer API (NT 3.5 and later; NT 3.1 has none): names and wave device <-> mixer pairing */
typedef UINT (WINAPI *PFN_MXN)(void);
typedef UINT (WINAPI *PFN_MXCAPS)(UINT, void *, UINT);
typedef UINT (WINAPI *PFN_MXID)(HANDLE, UINT *, DWORD);
static void MixerInfo(void)
{
    HMODULE w = GetModuleHandleW(L"WINMM");
    PFN_MXN num = w ? (PFN_MXN)GetProcAddress(w, "mixerGetNumDevs") : 0;
    PFN_MXCAPS caps = w ? (PFN_MXCAPS)GetProcAddress(w, "mixerGetDevCapsW") : 0;
    PFN_MXID id = w ? (PFN_MXID)GetProcAddress(w, "mixerGetID") : 0;
    UINT n, i, m;
    struct { WORD wMid, wPid; UINT vDriverVersion; WCHAR szPname[32]; DWORD fdwSupport, cDestinations; } mc;
    if (!num || !caps || !id) { u_printf("mixer API: not available\r\n"); return; }
    n = num(); u_printf("mixer devices: %d\r\n", n);
    for (i = 0; i < n; i++) if (!caps(i, &mc, sizeof(mc))) u_printf("  %d: %ls dest=%d\r\n", i, mc.szPname, (int)mc.cDestinations);
    n = waveOutGetNumDevs();
    for (i = 0; i < n; i++) {
        m = 0xFFFF;
        if (!id((HANDLE)(ULONG_PTR)i, &m, 0x10000000L /* MIXER_OBJECTF_WAVEOUT */)) u_printf("  waveOut %d -> mixer %d\r\n", i, m);
        else u_printf("  waveOut %d -> no mixer\r\n", i);
    }
    n = waveInGetNumDevs();
    for (i = 0; i < n; i++) {
        m = 0xFFFF;
        if (!id((HANDLE)(ULONG_PTR)i, &m, 0x20000000L /* MIXER_OBJECTF_WAVEIN */)) u_printf("  waveIn %d -> mixer %d\r\n", i, m);
        else u_printf("  waveIn %d -> no mixer\r\n", i);
    }
}

static void cmd_info(void)
{
    UINT n, i;
    WAVEOUTCAPSW wo; WAVEINCAPSW wi; MIDIOUTCAPSW mo; AUXCAPSW ax;
    HANDLE h;
    n = waveOutGetNumDevs(); u_printf("waveOut devices: %d\r\n", n);
    for (i = 0; i < n; i++) if (!waveOutGetDevCapsW(i, &wo, sizeof(wo)))
        u_printf("  %d: %ls fmt=%x ch=%d sup=%x\r\n", i, wo.szPname, (int)wo.dwFormats, wo.wChannels, (int)wo.dwSupport);
    n = waveInGetNumDevs(); u_printf("waveIn devices: %d\r\n", n);
    for (i = 0; i < n; i++) if (!waveInGetDevCapsW(i, &wi, sizeof(wi)))
        u_printf("  %d: %ls fmt=%x ch=%d\r\n", i, wi.szPname, (int)wi.dwFormats, wi.wChannels);
    n = midiOutGetNumDevs(); u_printf("midiOut devices: %d\r\n", n);
    for (i = 0; i < n; i++) if (!midiOutGetDevCapsW(i, &mo, sizeof(mo)))
        u_printf("  %d: %ls tech=%d voices=%d notes=%d\r\n", i, mo.szPname, mo.wTechnology, mo.wVoices, mo.wNotes);
    n = auxGetNumDevs(); u_printf("aux devices: %d\r\n", n);
    for (i = 0; i < n; i++) if (!auxGetDevCapsW(i, &ax, sizeof(ax)))
        u_printf("  %d: %ls tech=%d\r\n", i, ax.szPname, ax.wTechnology);
    h = CreateFileW(L"\\\\.\\AudioNT0", GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        AUDIONT_INFO in; DWORD got;
        if (DeviceIoControl(h, IOCTL_AUDIONT_GET_INFO, 0, 0, &in, sizeof(in), &got, 0))
            u_printf("AUDIONT %d.%02d on NT %x build %d: %s / %s, irq %d (%s), rec=%d\r\n"
                     "card name: \"%s\" (wave out \"%s\", wave in \"%s\", midi \"%s\", aux \"%s\", mixer \"%s\")\r\n",
                     (int)(in.Version >> 8), (int)(in.Version & 0xFF), (int)in.NtVersion, (int)in.NtBuild,
                     in.Controller, in.Codec, (int)in.Irq, in.InterruptMode ? "interrupt" : "polling",
                     (int)in.CanRecord, in.Name, in.DevName[0], in.DevName[1], in.DevName[2], in.DevName[3],
                     in.DevName[4]);
        CloseHandle(h);
    } else u_printf("AudioNT0 control device not found (%d)\r\n", (int)GetLastError());
    MixerInfo();
}

static HANDLE ctl(void)
{
    HANDLE h = CreateFileW(L"\\\\.\\AudioNT0", GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) { u_printf("cannot open AudioNT0 (%d)\r\n", (int)GetLastError()); g_fail++; }
    return h;
}

static void cmd_log(void)
{
    static char buf[32768]; DWORD got = 0; HANDLE h = ctl();
    if (h == INVALID_HANDLE_VALUE) return;
    if (DeviceIoControl(h, IOCTL_AUDIONT_GET_LOG, 0, 0, buf, sizeof(buf) - 1, &got, 0)) u_out(buf, got);
    CloseHandle(h);
}

static void cmd_dump(void)
{
    static char buf[65536]; DWORD got = 0; HANDLE h = ctl();
    if (h == INVALID_HANDLE_VALUE) return;
    if (DeviceIoControl(h, IOCTL_AUDIONT_CODEC_DUMP, 0, 0, buf, sizeof(buf) - 1, &got, 0)) u_out(buf, got);
    else u_printf("dump failed %d\r\n", (int)GetLastError());
    CloseHandle(h);
}

static void cmd_stats(void)
{
    AUDIONT_STATS st; DWORD got; HANDLE h = ctl();
    if (h == INVALID_HANDLE_VALUE) return;
    if (DeviceIoControl(h, IOCTL_AUDIONT_GET_STATS, 0, 0, &st, sizeof(st), &got, 0))
        u_printf("stats: int=%d ticks=%d underruns=%d played=%d recframes=%d play=%d rec=%d\r\n",
                 (int)st.Interrupts, (int)st.Ticks, (int)st.Underruns, (int)st.PlayedFramesLo,
                 (int)st.RecFramesLo, (int)st.Playing, (int)st.Recording);
    CloseHandle(h);
}

static void cmd_lines(void)
{
    static const char *nm[] = { "Master", "Wave", "Synth", "CD", "Line", "Mic", "Aux", "PCSpk", "RecGain", "-" };
    AUDIONT_LINES ls; AUDIONT_LINENAMES ln; DWORD got; HANDLE h = ctl(); unsigned i;
    if (h == INVALID_HANDLE_VALUE) return;
    memset(&ln, 0, sizeof(ln));
    if (!DeviceIoControl(h, IOCTL_AUDIONT_GET_LINENAMES, 0, 0, &ln, sizeof(ln), &got, 0)) memset(&ln, 0, sizeof(ln));
    if (DeviceIoControl(h, IOCTL_AUDIONT_GET_LINES, 0, 0, &ls, sizeof(ls), &got, 0)) {
        for (i = 0; i < ls.Count && i < AT_LINE_COUNT; i++)
            if (ls.Line[i].Present) {
                ln.Long[i][31] = 0;
                u_printf("  %-8s %5u %5u %s%s%s%s%s\r\n", nm[i], ls.Line[i].Left, ls.Line[i].Right,
                         ls.Line[i].Mute ? "muted " : "", ls.Line[i].HwVolume ? "(hw)" : "(sw)",
                         ln.Long[i][0] ? " \"" : "", ln.Long[i], ln.Long[i][0] ? "\"" : "");
            }
        u_printf("  recsrc mask %x current %x\r\n", (int)ls.RecSrcMask, (int)ls.RecSrc);
    }
    CloseHandle(h);
}

static void cmd_setline(int line, int l, int r, int mute)
{
    AUDIONT_SET_LINE sl; DWORD got; HANDLE h = ctl();
    if (h == INVALID_HANDLE_VALUE) return;
    sl.Line = line; sl.Left = (unsigned short)l; sl.Right = (unsigned short)r; sl.Mute = mute;
    if (!DeviceIoControl(h, IOCTL_AUDIONT_SET_LINE, &sl, sizeof(sl), 0, 0, &got, 0)) { u_printf("setline failed\r\n"); g_fail++; }
    CloseHandle(h);
}

static void cmd_recsrc(int line)
{
    DWORD mask = 1UL << line, got; HANDLE h = ctl();
    if (h == INVALID_HANDLE_VALUE) return;
    if (!DeviceIoControl(h, IOCTL_AUDIONT_SET_RECSRC, &mask, 4, 0, 0, &got, 0)) { u_printf("recsrc %d failed\r\n", line); g_fail++; }
    else u_printf("record source: line %d\r\n", line);
    CloseHandle(h);
}

static void cmd_seq(void)
{
    /* the harness looks for these tones in out.wav, in this order */
    static const int t[][5] = {
        /* freq, rate, bits, ch, ms */
        { 440, 11025, 8, 1, 700 },
        { 1000, 22050, 16, 1, 700 },
        { 1500, 44100, 16, 2, 700 },
        { 2000, 48000, 16, 2, 700 },
        { 2500, 8000, 8, 2, 700 },
        { 3000, 96000, 16, 2, 700 },
        { 3500, 32000, 24, 2, 700 },
        { 750, 44100, 8, 2, 700 },
    };
    int i;
    for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
        play_tone(t[i][0], t[i][4], t[i][1], t[i][2], t[i][3], WAVE_MAPPER, 50, 1);
        silence(300);
    }
}

static DWORD WINAPI mix_thread(LPVOID p)
{
    play_tone((int)(DWORD_PTR)p, 2000, 44100, 16, 2, 0, 30, 1);
    return 0;
}

static void cmd_mix(void)
{
    DWORD tid; HANDLE th = CreateThread(0, 0, mix_thread, (LPVOID)440, 0, &tid);
    Sleep(300);
    play_tone(1000, 1500, 22050, 16, 1, 0, 30, 1);
    WaitForSingleObject(th, 10000);
    CloseHandle(th);
}

static DWORD WINAPI stress_thread(LPVOID p)
{
    static const int cfg[4][4] = { { 300, 11025, 8, 1 }, { 500, 22050, 16, 2 }, { 700, 44100, 16, 2 }, { 900, 48000, 16, 1 } };
    int i = (int)(DWORD_PTR)p;
    play_tone(cfg[i][0], 6000, cfg[i][1], cfg[i][2], cfg[i][3], 0, 15, 1);
    return 0;
}

static void cmd_stress(void)
{
    HANDLE th[4]; DWORD tid; int i, k; HMIDIOUT hm;
    for (i = 0; i < 4; i++) { th[i] = CreateThread(0, 0, stress_thread, (LPVOID)(DWORD_PTR)i, 0, &tid); Sleep(150); }
    if (!midiOutOpen(&hm, 0, 0, 0, 0)) {
        for (k = 0; k < 40; k++) {
            midiOutShortMsg(hm, 0x00500090 | ((48 + (k * 7) % 24) << 8));
            midiOutShortMsg(hm, 0x00600099 | ((k & 1 ? 38 : 36) << 8));
            Sleep(120);
            midiOutShortMsg(hm, 0x00000080 | ((48 + (k * 7) % 24) << 8));
        }
        midiOutReset(hm); midiOutClose(hm);
    } else { u_printf("  midi open failed\r\n"); g_fail++; }
    WaitForMultipleObjects(4, th, TRUE, 30000);
    for (i = 0; i < 4; i++) CloseHandle(th[i]);
    /* rapid open/close */
    for (k = 0; k < 40; k++) {
        HWAVEOUT h; WAVEFORMATEX wf;
        memset(&wf, 0, sizeof(wf));
        wf.wFormatTag = 1; wf.nChannels = 2; wf.nSamplesPerSec = 22050; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 88200;
        if (waveOutOpen(&h, 0, &wf, 0, 0, 0)) { u_printf("  open %d failed\r\n", k); g_fail++; break; }
        waveOutClose(h);
    }
    u_printf("  stress done\r\n");
}

static void cmd_pos(void)
{
    /* GetPosition while playing must grow monotonically and roughly with wall time */
    HWAVEOUT h; WAVEFORMATEX wf; WAVEHDR hdr; MMTIME mt; DWORD t0, last = 0; int i, bad = 0;
    char *mem; unsigned long ph = 0;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = 1; wf.nChannels = 2; wf.nSamplesPerSec = 44100; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 176400;
    if (waveOutOpen(&h, 0, &wf, 0, 0, 0)) { u_printf("open failed\r\n"); g_fail++; return; }
    mem = (char *)GlobalAlloc(GPTR, 176400 * 2);
    gen(mem, 88200, 44100, 16, 2, 600, 30, &ph);
    memset(&hdr, 0, sizeof(hdr)); hdr.lpData = mem; hdr.dwBufferLength = 176400 * 2;
    waveOutPrepareHeader(h, &hdr, sizeof(hdr));
    t0 = GetTickCount();
    waveOutWrite(h, &hdr, sizeof(hdr));
    for (i = 0; i < 40; i++) {
        Sleep(50);
        mt.wType = TIME_SAMPLES; waveOutGetPosition(h, &mt, sizeof(mt));
        if (mt.u.sample < last) bad++;
        last = mt.u.sample;
        if (i % 8 == 0) u_printf("  t=%d ms pos=%d samples (%d ms)\r\n", (int)(GetTickCount() - t0), (int)mt.u.sample, (int)(mt.u.sample / 44));
        if (i == 20) { waveOutPause(h); u_printf("  paused\r\n"); }
        if (i == 26) { waveOutRestart(h); u_printf("  restarted\r\n"); }
    }
    while (!(hdr.dwFlags & WHDR_DONE) && GetTickCount() - t0 < 6000) Sleep(20);
    mt.wType = TIME_SAMPLES; waveOutGetPosition(h, &mt, sizeof(mt));
    u_printf("  done after %d ms, final pos %d (expect 88200), nonmonotonic=%d\r\n",
             (int)(GetTickCount() - t0), (int)mt.u.sample, bad);
    if (mt.u.sample != 88200 || bad) g_fail++;
    waveOutUnprepareHeader(h, &hdr, sizeof(hdr));
    waveOutClose(h);
    GlobalFree((HGLOBAL)mem);
}

static void cmd_reset(void)
{
    HWAVEOUT h; WAVEFORMATEX wf; WAVEHDR hdr[3]; MMTIME mt; int i; char *mem; unsigned long ph = 0;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = 1; wf.nChannels = 1; wf.nSamplesPerSec = 22050; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 44100;
    if (waveOutOpen(&h, 0, &wf, 0, 0, 0)) { u_printf("open failed\r\n"); g_fail++; return; }
    mem = (char *)GlobalAlloc(GPTR, 44100 * 3);
    memset(hdr, 0, sizeof(hdr));
    for (i = 0; i < 3; i++) {
        hdr[i].lpData = mem + i * 44100; hdr[i].dwBufferLength = 44100;
        gen(hdr[i].lpData, 22050, 22050, 16, 1, 880, 30, &ph);
        waveOutPrepareHeader(h, &hdr[i], sizeof(WAVEHDR));
        waveOutWrite(h, &hdr[i], sizeof(WAVEHDR));
    }
    Sleep(600);
    waveOutReset(h);
    Sleep(50);
    mt.wType = TIME_BYTES; waveOutGetPosition(h, &mt, sizeof(mt));
    for (i = 0; i < 3; i++)
        if (!(hdr[i].dwFlags & WHDR_DONE)) { u_printf("  buffer %d not returned after reset\r\n", i); g_fail++; }
    u_printf("  reset ok, pos after reset %d\r\n", (int)mt.u.cb);
    for (i = 0; i < 3; i++) waveOutUnprepareHeader(h, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(h);
    GlobalFree((HGLOBAL)mem);
}

static void cmd_rec(int ms, int rate, int bits, int ch)
{
    HWAVEIN h; WAVEFORMATEX wf; WAVEHDR hdr[4]; int i, ba = ch * bits / 8, bytes, total = 0;
    char *mem; DWORD t0; long peak = 0; __int64 sum = 0; int n = 0;
    long cross = 0, last = 0, frames = 0;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = 1; wf.nChannels = (WORD)ch; wf.nSamplesPerSec = rate; wf.wBitsPerSample = (WORD)bits;
    wf.nBlockAlign = (WORD)ba; wf.nAvgBytesPerSec = rate * ba;
    if (waveInOpen(&h, 0, &wf, 0, 0, 0)) { u_printf("waveInOpen failed\r\n"); g_fail++; return; }
    bytes = rate * ba / 4;
    mem = (char *)GlobalAlloc(GPTR, bytes * 4);
    memset(hdr, 0, sizeof(hdr));
    for (i = 0; i < 4; i++) {
        hdr[i].lpData = mem + i * bytes; hdr[i].dwBufferLength = bytes;
        waveInPrepareHeader(h, &hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(h, &hdr[i], sizeof(WAVEHDR));
    }
    t0 = GetTickCount();
    waveInStart(h);
    while (GetTickCount() - t0 < (DWORD)ms) {
        for (i = 0; i < 4; i++) {
            if (hdr[i].dwFlags & WHDR_DONE) {
                int j;
                total += hdr[i].dwBytesRecorded;
                if (bits == 16) for (j = 0; j < (int)hdr[i].dwBytesRecorded / 2; j++) {
                    long v = ((short *)hdr[i].lpData)[j];
                    if (j % ch == 0) {
                        /* first channel: zero crossings give the frequency */
                        if (frames && ((v >= 0) != (last >= 0))) cross++;
                        last = v; frames++;
                    }
                    if (v < 0) v = -v;
                    if (v > peak) peak = v;
                    sum += v; n++;
                }
                waveInUnprepareHeader(h, &hdr[i], sizeof(WAVEHDR));
                hdr[i].dwFlags = 0;
                waveInPrepareHeader(h, &hdr[i], sizeof(WAVEHDR));
                waveInAddBuffer(h, &hdr[i], sizeof(WAVEHDR));
            }
        }
        Sleep(10);
    }
    waveInReset(h);
    for (i = 0; i < 4; i++) waveInUnprepareHeader(h, &hdr[i], sizeof(WAVEHDR));
    waveInClose(h);
    u_printf("  recorded %d bytes in %d ms (expect ~%d), peak %d avg %d freq %d\r\n", total, ms,
             (int)((__int64)rate * ba * ms / 1000), (int)peak, n ? (int)(sum / n) : 0,
             frames ? (int)((__int64)cross * rate / 2 / frames) : 0);
    if (total < (int)((__int64)rate * ba * ms / 1000) / 2) g_fail++;
    GlobalFree((HGLOBAL)mem);
}

static void cmd_midi(int dev)
{
    HMIDIOUT h; MMRESULT r; int i;
    static const int notes[] = { 60, 64, 67, 72, 67, 64, 60 };
    r = midiOutOpen(&h, dev, 0, 0, 0);
    if (r) { u_printf("midiOutOpen(%d) failed %s\r\n", dev, mmerr(r)); g_fail++; return; }
    midiOutShortMsg(h, 0x000000C0);                 /* piano */
    for (i = 0; i < 7; i++) {
        midiOutShortMsg(h, 0x00700090 | (notes[i] << 8));
        Sleep(200);
        midiOutShortMsg(h, 0x00000080 | (notes[i] << 8));
    }
    midiOutShortMsg(h, 0x000030C1);                 /* strings chord on ch 2 */
    midiOutShortMsg(h, 0x00603C91); midiOutShortMsg(h, 0x00604091); midiOutShortMsg(h, 0x00604391);
    midiOutShortMsg(h, 0x00702499);                 /* kick */
    Sleep(300);
    midiOutShortMsg(h, 0x00702699);                 /* snare */
    Sleep(300);
    midiOutShortMsg(h, 0x00702A99);                 /* hihat */
    Sleep(600);
    midiOutShortMsg(h, 0x00003C81); midiOutShortMsg(h, 0x00004081); midiOutShortMsg(h, 0x00004381);
    Sleep(500);
    midiOutReset(h);
    midiOutClose(h);
    u_printf("  midi phrase sent\r\n");
}

static void cmd_reg(int dw, const WCHAR *key, const WCHAR *name, const WCHAR *val)
{
    HKEY k; DWORD disp, v; LONG r;
    r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, &disp);
    if (r) { u_printf("RegCreateKeyEx failed %d\r\n", (int)r); g_fail++; return; }
    if (dw) { v = u_watoi(val); r = RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&v, 4); }
    else r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (u_wcslen(val) + 1) * 2);
    if (r) { u_printf("RegSetValueEx failed %d\r\n", (int)r); g_fail++; }
    RegCloseKey(k);
}

/* Drivers32 entries naming audiont.dll: list them, or remove them (drv32 clear) */
static void cmd_drv32(int clear)
{
    static const WCHAR *types[] = { L"wave", L"midi", L"aux", L"mixer" };
    HKEY k; WCHAR name[16], val[MAX_PATH]; DWORD type, len; int t, i, n = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", 0,
                      KEY_ALL_ACCESS, &k)) { u_printf("  no Drivers32 key\r\n"); g_fail++; return; }
    for (t = 0; t < 4; t++)
        for (i = 0; i < 10; i++) {
            if (i) wsprintfW(name, L"%s%d", types[t], i); else u_wcscpy(name, types[t]);
            len = sizeof(val); val[0] = 0;
            if (RegQueryValueExW(k, name, 0, &type, (BYTE *)val, &len) || u_wcsicmp(val, L"audiont.dll")) continue;
            n++;
            if (clear) { RegDeleteValueW(k, name); u_printf("  removed %ls\r\n", name); }
            else u_printf("  drivers32 %ls = audiont.dll\r\n", name);
        }
    RegCloseKey(k);
    u_printf("  drivers32: %d audiont.dll entries%s\r\n", n, clear ? " removed" : "");
}

int main(void)
{
    int argc; WCHAR **argv = u_argv(&argc);
    const WCHAR *c = argc > 1 ? argv[1] : L"info";
    if (!u_wcsicmp(c, L"info")) cmd_info();
    else if (!u_wcsicmp(c, L"tone") && argc >= 7)
        play_tone(u_watoi(argv[2]), u_watoi(argv[3]), u_watoi(argv[4]), u_watoi(argv[5]), u_watoi(argv[6]),
                  argc > 7 ? u_watoi(argv[7]) : 0, argc > 8 ? u_watoi(argv[8]) : 50, 1);
    else if (!u_wcsicmp(c, L"seq")) cmd_seq();
    else if (!u_wcsicmp(c, L"mix")) cmd_mix();
    else if (!u_wcsicmp(c, L"stress")) cmd_stress();
    else if (!u_wcsicmp(c, L"killtest")) {
        /* start a child that plays/records, kill it mid-way: exercises IRP cancel + cleanup */
        STARTUPINFOW si; PROCESS_INFORMATION pi; int k;
        WCHAR cmdl[3][128];
        u_wcscpy(cmdl[0], L"C:\\AUDIONT\\TEST\\ATTEST.EXE tone 500 8000 44100 16 2");
        u_wcscpy(cmdl[1], L"C:\\AUDIONT\\TEST\\ATTEST.EXE rec 8000 22050 16 1");
        u_wcscpy(cmdl[2], L"C:\\AUDIONT\\TEST\\ATTEST.EXE midi");
        for (k = 0; k < 3; k++) {
            memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
            if (CreateProcessW(0, cmdl[k], 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
                Sleep(1200);
                TerminateProcess(pi.hProcess, 3);
                WaitForSingleObject(pi.hProcess, 5000);
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                u_printf("  killed child %d\r\n", k);
            }
        }
        play_tone(1234, 500, 44100, 16, 2, 0, 50, 1);
    }
    else if (!u_wcsicmp(c, L"pos")) cmd_pos();
    else if (!u_wcsicmp(c, L"reset")) cmd_reset();
    else if (!u_wcsicmp(c, L"rec") && argc >= 6) cmd_rec(u_watoi(argv[2]), u_watoi(argv[3]), u_watoi(argv[4]), u_watoi(argv[5]));
    else if (!u_wcsicmp(c, L"midi")) cmd_midi(argc > 2 ? u_watoi(argv[2]) : 0);
    else if (!u_wcsicmp(c, L"log")) cmd_log();
    else if (!u_wcsicmp(c, L"dump")) cmd_dump();
    else if (!u_wcsicmp(c, L"stats")) cmd_stats();
    else if (!u_wcsicmp(c, L"lines")) cmd_lines();
    else if (!u_wcsicmp(c, L"sleep") && argc >= 3) Sleep(u_watoi(argv[2]));
    else if (!u_wcsicmp(c, L"mci") && argc >= 3) {
        WCHAR ret[256]; MCIERROR e; int i;
        for (i = 2; i < argc; i++) {
            ret[0] = 0;
            e = mciSendStringW(argv[i], ret, 256, 0);
            u_printf("  mci \"%ls\" -> %d %ls\r\n", argv[i], (int)e, ret);
            if (e) { WCHAR et[256]; mciGetErrorStringW(e, et, 256); u_printf("    %ls\r\n", et); g_fail++; }
        }
    }
    else if (!u_wcsicmp(c, L"runwait") && argc >= 3) {
        STARTUPINFOW si; PROCESS_INFORMATION pi; DWORD code = 999;
        memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
        if (CreateProcessW(0, argv[2], 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 600000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
        else u_printf("CreateProcess failed %d\r\n", (int)GetLastError());
        u_printf("runwait exit %d\r\n", (int)code);
    }
    else if (!u_wcsicmp(c, L"regsz") && argc >= 5) cmd_reg(0, argv[2], argv[3], argv[4]);
    else if (!u_wcsicmp(c, L"regdw") && argc >= 5) cmd_reg(1, argv[2], argv[3], argv[4]);
    else if (!u_wcsicmp(c, L"recsrc") && argc >= 3) cmd_recsrc(u_watoi(argv[2]));
    else if (!u_wcsicmp(c, L"tonefmt") && argc >= 8)
        play_tone_fmt(u_watoi(argv[2]), u_watoi(argv[3]), u_watoi(argv[4]), u_watoi(argv[5]), u_watoi(argv[6]),
                      u_watoi(argv[7]));
    else if (!u_wcsicmp(c, L"drv32")) cmd_drv32(argc > 2 && !u_wcsicmp(argv[2], L"clear"));
    else if (!u_wcsicmp(c, L"mxvol") && argc >= 4) cmd_mxvol(argv[2], u_watoi(argv[3]));
    else if (!u_wcsicmp(c, L"setline") && argc >= 6) cmd_setline(u_watoi(argv[2]), u_watoi(argv[3]), u_watoi(argv[4]), u_watoi(argv[5]));
    else { u_printf("usage: attest info|tone|seq|mix|pos|reset|rec|midi|log|dump|stats|lines|setline\r\n"); return 2; }
    u_printf("RESULT %s (%d failures)\r\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}

void __stdcall WinMainCRTStartup(void) { ExitProcess(main()); }
