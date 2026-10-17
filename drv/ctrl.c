/*
 * AUDIONT - mixer controls, aux device and the private control device
 */
#include "audiont.h"

/* 40*log10(v/65535) attenuation in 1/100 dB (squared-law volume curve) */
ULONG VolToDb100(USHORT v)
{
    /* log2(v) in Q16 */
    ULONG x = v, l2 = 0, i;
    if (v == 0) return 100000;      /* -1000 dB = silence */
    if (v >= 65535) return 0;
    while (x >= 2) { x >>= 1; l2 += 65536; }
    /* fractional part: m = v / 2^k in [1,2) as Q16 */
    {
        ULONG k = l2 >> 16, m = ((ULONG)v << 16) >> k, bit = 32768;
        for (i = 0; i < 16; i++) {
            m = (ULONG)(((ULONGLONG)m * m) >> 16);
            if (m >= 131072) { m >>= 1; l2 += bit; }
            bit >>= 1;
        }
    }
    /* log2(65535) ~= 16.0 (Q16: 1048554) */
    {
        ULONG d = 1048554 - l2;             /* log2 ratio, Q16 */
        /* dB100 = 4000 * log10(2) * d / 65536 = 1204.12 * d / 65536 */
        return (ULONG)(((ULONGLONG)d * 120412) / (65536 * 100));
    }
}

LONG CtrlGain(CARD *c, ULONG line, int right)
{
    LINE_STATE *l = &c->Lines[line];
    ULONG v;
    if (l->Mute) return 0;
    if (l->HwVolume) return 32768;
    v = right ? l->Right : l->Left;
    if (v >= 65535) return 32768;
    return (LONG)(((ULONG)v * v) >> 17);
}

void CtrlInit(CARD *c)
{
    static const USHORT defv[LINE_COUNT] = { 0xFFFF, 0xFFFF, 0xD000, 0xC000, 0xC000, 0x8000,
                                             0xC000, 0x8000, 0xC000, 0xFFFF };
    ULONG i;
    for (i = 0; i < LINE_COUNT; i++) {
        c->Lines[i].Left = c->Lines[i].Right = defv[i];
        c->Lines[i].HasMute = TRUE;
    }
    c->Lines[LINE_MASTER].Present = TRUE;
    c->Lines[LINE_WAVE].Present = TRUE;
    c->Lines[LINE_SYNTH].Present = TRUE;
    c->Lines[LINE_MIC].Mute = TRUE;
}

void CtrlLoad(CARD *c)
{
    AUDIONT_LINE saved[LINE_COUNT];
    WCHAR name[16];
    ULONG i, rs;
    OsWcsCopyA(name, "Mixer0", 16); name[5] = (WCHAR)('0' + c->Index);
    if (OsRegGetBinary(name, saved, sizeof(saved)) == sizeof(saved)) {
        for (i = 0; i < LINE_COUNT; i++) {
            c->Lines[i].Left = saved[i].Left;
            c->Lines[i].Right = saved[i].Right;
            c->Lines[i].Mute = saved[i].Mute;
        }
    }
    OsWcsCopyA(name, "RecSrc0", 16); name[6] = (WCHAR)('0' + c->Index);
    rs = OsRegGetDword(name, 0);
    if (rs & c->RecSrcMask) c->RecSrc = rs & c->RecSrcMask;
}

void CtrlSave(CARD *c)
{
    AUDIONT_LINE saved[LINE_COUNT];
    WCHAR name[16];
    ULONG i;
    for (i = 0; i < LINE_COUNT; i++) {
        saved[i].Present = c->Lines[i].Present; saved[i].HwVolume = c->Lines[i].HwVolume;
        saved[i].HasMute = c->Lines[i].HasMute; saved[i].Mute = c->Lines[i].Mute;
        saved[i].Left = c->Lines[i].Left; saved[i].Right = c->Lines[i].Right;
    }
    OsWcsCopyA(name, "Mixer0", 16); name[5] = (WCHAR)('0' + c->Index);
    OsRegSetBinary(name, saved, sizeof(saved));
    OsWcsCopyA(name, "RecSrc0", 16); name[6] = (WCHAR)('0' + c->Index);
    OsRegSetDword(name, c->RecSrc);
}

static void SetLine(CARD *c, ULONG line, USHORT l, USHORT r, BOOLEAN mute)
{
    c->Lines[line].Left = l;
    c->Lines[line].Right = r;
    c->Lines[line].Mute = mute;
    if (c->Lines[line].HwVolume && c->Ops->ApplyLine) c->Ops->ApplyLine(c, line);
    c->ChangeCount++;
}

NTSTATUS AuxIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp)
{
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    switch (IrpCode(sp)) {
    case IOCTL_AUX_GET_CAPABILITIES: {
        AUXCAPSW caps; char name[40];
        memset(&caps, 0, sizeof(caps));
        caps.wMid = MM_MICROSOFT; caps.wPid = 4;
        caps.vDriverVersion = AUDIONT_VERSION;
        CardNameFor(c, DEV_AUX, name, sizeof(name));
        OsWcsCopyA(caps.szPname, name, MAXPNAMELEN);
        caps.wTechnology = AUXCAPS_AUXIN;
        caps.dwSupport = AUXCAPS_VOLUME | AUXCAPS_LRVOLUME;
        irp->IoStatus.Information = MIN(IrpOutLen(sp), sizeof(caps));
        memcpy(buf, &caps, irp->IoStatus.Information);
        return STATUS_SUCCESS;
    }
    case IOCTL_AUX_SET_VOLUME:
        if (IrpInLen(sp) < sizeof(AUX_DD_VOLUME)) return STATUS_INVALID_PARAMETER;
        EngineLock(c);
        SetLine(c, LINE_MASTER, (USHORT)(((AUX_DD_VOLUME *)buf)->Left >> 16),
                (USHORT)(((AUX_DD_VOLUME *)buf)->Right >> 16), c->Lines[LINE_MASTER].Mute);
        EngineUnlock(c);
        return STATUS_SUCCESS;
    case IOCTL_AUX_GET_VOLUME: {
        AUX_DD_VOLUME v;
        if (IrpOutLen(sp) < sizeof(v)) return STATUS_BUFFER_TOO_SMALL;
        v.Left  = (ULONG)c->Lines[LINE_MASTER].Left << 16 | c->Lines[LINE_MASTER].Left;
        v.Right = (ULONG)c->Lines[LINE_MASTER].Right << 16 | c->Lines[LINE_MASTER].Right;
        memcpy(buf, &v, sizeof(v));
        irp->IoStatus.Information = sizeof(v);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS CtrlIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp)
{
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    ULONG outl = IrpOutLen(sp), inl = IrpInLen(sp), i;
    switch (IrpCode(sp)) {
    case IOCTL_AUDIONT_GET_INFO: {
        AUDIONT_INFO in;
        memset(&in, 0, sizeof(in));
        in.Version = AUDIONT_VERSION; in.CardIndex = c->Index;
        in.NtVersion = g_NtVersion; in.NtBuild = g_NtBuild; in.HwType = c->Type;
        in.VendorId = c->VendorId; in.DeviceId = c->DeviceId;
        in.SubVendor = c->SubVendor; in.SubDevice = c->SubDevice;
        in.Bus = c->Bus; in.Dev = c->Dev; in.Fn = c->Fn; in.Irq = c->Irq;
        in.InterruptMode = c->UseInterrupt; in.CanRecord = c->CanRecord;
        in.WaveOutDev = c->DevIndex[DEV_WAVEOUT]; in.WaveInDev = c->DevIndex[DEV_WAVEIN];
        in.MidiOutDev = c->DevIndex[DEV_MIDIOUT]; in.AuxDev = c->DevIndex[DEV_AUX];
        memcpy(in.Controller, c->Desc, sizeof(in.Controller));
        memcpy(in.Codec, c->Codec, sizeof(in.Codec));
        in.Connected = c->Type != HW_USB || UsbConnected(c);
        for (i = 0; i < DEV_KINDS; i++) CardNameFor(c, i, in.DevName[i], sizeof(in.DevName[i]));
        CardNameFor(c, DEV_KINDS, in.Name, sizeof(in.Name));
        irp->IoStatus.Information = MIN(outl, sizeof(in));
        memcpy(buf, &in, irp->IoStatus.Information);
        return STATUS_SUCCESS;
    }
    case IOCTL_AUDIONT_GET_LINES: {
        AUDIONT_LINES ls;
        memset(&ls, 0, sizeof(ls));
        ls.Count = LINE_COUNT; ls.RecSrcMask = c->RecSrcMask; ls.RecSrc = c->RecSrc;
        ls.ChangeCount = c->ChangeCount;
        for (i = 0; i < LINE_COUNT; i++) {
            ls.Line[i].Present = c->Lines[i].Present; ls.Line[i].HwVolume = c->Lines[i].HwVolume;
            ls.Line[i].HasMute = c->Lines[i].HasMute; ls.Line[i].Mute = c->Lines[i].Mute;
            ls.Line[i].Left = c->Lines[i].Left; ls.Line[i].Right = c->Lines[i].Right;
        }
        irp->IoStatus.Information = MIN(outl, sizeof(ls));
        memcpy(buf, &ls, irp->IoStatus.Information);
        return STATUS_SUCCESS;
    }
    case IOCTL_AUDIONT_GET_LINENAMES: {
        AUDIONT_LINENAMES ln;
        memset(&ln, 0, sizeof(ln));
        for (i = 0; i < LINE_COUNT && i < AT_LINE_COUNT; i++) {
            memcpy(ln.Short[i], c->LineShort[i], sizeof(ln.Short[i]) - 1);
            memcpy(ln.Long[i], c->LineLong[i], sizeof(ln.Long[i]) - 1);
        }
        irp->IoStatus.Information = MIN(outl, sizeof(ln));
        memcpy(buf, &ln, irp->IoStatus.Information);
        return STATUS_SUCCESS;
    }
    case IOCTL_AUDIONT_SET_LINE: {
        AUDIONT_SET_LINE *sl = buf;
        if (inl < sizeof(*sl) || sl->Line >= LINE_COUNT) return STATUS_INVALID_PARAMETER;
        EngineLock(c);
        SetLine(c, sl->Line, sl->Left, sl->Right, sl->Mute ? TRUE : FALSE);
        EngineUnlock(c);
        return STATUS_SUCCESS;
    }
    case IOCTL_AUDIONT_SET_RECSRC:
        if (inl < 4) return STATUS_INVALID_PARAMETER;
        if (!(*(PULONG)buf & c->RecSrcMask)) return STATUS_INVALID_PARAMETER;
        EngineLock(c);
        c->RecSrc = *(PULONG)buf & c->RecSrcMask;
        c->RecSrc &= -(LONG)c->RecSrc;      /* keep lowest bit: single source */
        if (c->Ops->ApplyRecSrc) c->Ops->ApplyRecSrc(c);
        c->ChangeCount++;
        EngineUnlock(c);
        return STATUS_SUCCESS;
    case IOCTL_AUDIONT_GET_LOG:
        irp->IoStatus.Information = OsLogSnapshot(buf, outl);
        return STATUS_SUCCESS;
    case IOCTL_AUDIONT_GET_STATS: {
        AUDIONT_STATS st;
        memset(&st, 0, sizeof(st));
        EngineLock(c);
        st.Interrupts = c->IntCount; st.Ticks = c->TickCount; st.Underruns = c->Underruns;
        st.PlayedFramesLo = (ULONG)c->PlayedFrames; st.PlayedFramesHi = (ULONG)(c->PlayedFrames >> 32);
        st.RecFramesLo = (ULONG)c->RecFrames; st.RecFramesHi = (ULONG)(c->RecFrames >> 32);
        for (i = 0; i < MAX_PLAY_STREAMS; i++) st.ActivePlayStreams += c->Play[i].InUse;
        for (i = 0; i < MAX_REC_STREAMS; i++) st.ActiveRecStreams += c->Rec[i].InUse;
        st.Playing = c->Playing; st.Recording = c->Recording;
        EngineUnlock(c);
        irp->IoStatus.Information = MIN(outl, sizeof(st));
        memcpy(buf, &st, irp->IoStatus.Information);
        return STATUS_SUCCESS;
    }
    case IOCTL_AUDIONT_CODEC_DUMP:
        if (!c->Ops->Dump) return STATUS_NOT_SUPPORTED;
        irp->IoStatus.Information = c->Ops->Dump(c, buf, outl);
        return STATUS_SUCCESS;
    case IOCTL_AUDIONT_SAVE:
        CtrlSave(c);
        return STATUS_SUCCESS;
    case IOCTL_AUDIONT_USERLOG: {
        char tmp[200];
        ULONG n = MIN(inl, sizeof(tmp) - 1);
        memcpy(tmp, buf, n); tmp[n] = 0;
        LOGI("user: %s", tmp);
        return STATUS_SUCCESS;
    }
    case IOCTL_AUDIONT_SET_DEBUG:
        if (inl < 4) return STATUS_INVALID_PARAMETER;
        g_DebugLevel = *(PULONG)buf;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}
