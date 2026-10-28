/*
 * AUDIONT - AC'97 codec layer (register model per AC'97 2.3), shared by the
 * ICH and VIA controller back-ends.
 */
#include "audiont.h"
#include "ac97.h"

#define RD(r)    c->Ac97->CodecRead(c, (r))
#define WR(r, v) c->Ac97->CodecWrite(c, (r), (USHORT)(v))

static const struct { ULONG Id, Mask; const char *Name; } g_CodecNames[] = {
    { 0x41445303, 0xFFFFFFFF, "Analog Devices AD1819" },
    { 0x41445340, 0xFFFFFFFF, "Analog Devices AD1881" },
    { 0x41445348, 0xFFFFFFFF, "Analog Devices AD1881A" },
    { 0x41445360, 0xFFFFFFFF, "Analog Devices AD1885" },
    { 0x41445361, 0xFFFFFFFF, "Analog Devices AD1886" },
    { 0x41445362, 0xFFFFFFFF, "Analog Devices AD1887" },
    { 0x41445368, 0xFFFFFFFF, "Analog Devices AD1888" },
    { 0x41445370, 0xFFFFFFFF, "Analog Devices AD1980" },
    { 0x41445372, 0xFFFFFFFF, "Analog Devices AD1981A" },
    { 0x41445374, 0xFFFFFFFF, "Analog Devices AD1981B" },
    { 0x41445375, 0xFFFFFFFF, "Analog Devices AD1985" },
    { 0x41445378, 0xFFFFFFFF, "Analog Devices AD1986" },
    { 0x414C4300, 0xFFFFFFF0, "Realtek ALC100" },
    { 0x414C4710, 0xFFFFFFF0, "Realtek ALC200/201" },
    { 0x414C4720, 0xFFFFFFF0, "Realtek ALC650" },
    { 0x414C4730, 0xFFFFFFF0, "Realtek ALC101" },
    { 0x414C4740, 0xFFFFFFF0, "Realtek ALC202" },
    { 0x414C4750, 0xFFFFFFF0, "Realtek ALC250" },
    { 0x414C4760, 0xFFFFFFF0, "Realtek ALC655" },
    { 0x414C4770, 0xFFFFFFF0, "Realtek ALC203" },
    { 0x414C4780, 0xFFFFFFF0, "Realtek ALC658" },
    { 0x414C4790, 0xFFFFFFF0, "Realtek ALC850" },
    { 0x434D4941, 0xFFFFFFFF, "C-Media CMI9738" },
    { 0x434D4961, 0xFFFFFFFF, "C-Media CMI9739" },
    { 0x434D4978, 0xFFFFFFFF, "C-Media CMI9761" },
    { 0x43525900, 0xFFFFFFF8, "Cirrus Logic CS4297" },
    { 0x43525910, 0xFFFFFFF8, "Cirrus Logic CS4297A" },
    { 0x43525920, 0xFFFFFFF8, "Cirrus Logic CS4294/4298" },
    { 0x43525930, 0xFFFFFFF8, "Cirrus Logic CS4299" },
    { 0x43525960, 0xFFFFFFF8, "Cirrus Logic CS4205" },
    { 0x43585430, 0xFFFFFFFF, "Conexant Cx20468" },
    { 0x45838308, 0xFFFFFFFF, "ESS ES1921" },
    { 0x49434511, 0xFFFFFFFF, "ICEnsemble ICE1232" },
    { 0x4E534331, 0xFFFFFFFF, "National LM4549" },
    { 0x53494C22, 0xFFFFFFFF, "Silicon Labs Si3036" },
    { 0x54524103, 0xFFFFFFFF, "TriTech TR28023" },
    { 0x54524106, 0xFFFFFFFF, "TriTech TR28026" },
    { 0x54524108, 0xFFFFFFFF, "TriTech TR28028" },
    { 0x56494161, 0xFFFFFFFF, "VIA VT1612A" },
    { 0x56494170, 0xFFFFFFFF, "VIA VT1617A" },
    { 0x56494182, 0xFFFFFFFF, "VIA VT1618" },
    { 0x574D4C00, 0xFFFFFFF0, "Wolfson WM9701A" },
    { 0x574D4C03, 0xFFFFFFFF, "Wolfson WM9703/9707" },
    { 0x574D4C04, 0xFFFFFFFF, "Wolfson WM9704" },
    { 0x83847600, 0xFFFFFFFF, "SigmaTel STAC9700/9783/9784" },
    { 0x83847604, 0xFFFFFFFF, "SigmaTel STAC9701/9703/9704/9705" },
    { 0x83847605, 0xFFFFFFFF, "SigmaTel STAC9704" },
    { 0x83847608, 0xFFFFFFFF, "SigmaTel STAC9708/9711" },
    { 0x83847609, 0xFFFFFFFF, "SigmaTel STAC9721/9723" },
    { 0x83847644, 0xFFFFFFFF, "SigmaTel STAC9744" },
    { 0x83847650, 0xFFFFFFFF, "SigmaTel STAC9750/9751" },
    { 0x83847652, 0xFFFFFFFF, "SigmaTel STAC9752/9753" },
    { 0x83847656, 0xFFFFFFFF, "SigmaTel STAC9756/9757" },
    { 0x83847666, 0xFFFFFFFF, "SigmaTel STAC9766/9767" },
    { 0x594D4800, 0xFFFFFFFF, "Yamaha YMF743" },
    { 0x594D4803, 0xFFFFFFFF, "Yamaha YMF753" },
    { 0, 0, NULL }
};

/* boards whose external amplifier is powered by EAPD = 1 (Linux AC97_TUNE_INV_EAPD) */
static const struct { USHORT Ven, Dev; } g_InvEapd[] = {
    { 0x1014, 0x0534 },     /* ThinkPad X31 */
    { 0x104D, 0x8144 },     /* Sony */
    { 0x104D, 0x8197 },     /* Sony S1XP */
    { 0x104D, 0x81C0 },     /* Sony VAIO VGN-T350P */
    { 0x104D, 0x81C5 },     /* Sony VAIO VGN-B1VP */
    { 0x161F, 0x202F },     /* Gateway M520 */
    { 0x161F, 0x203A },     /* Gateway 4525GZ */
    { 0, 0 }
};

#define UPD(r, mask, val) WR((r), (RD(r) & ~(mask)) | (val))

/* Cirrus Logic codecs with the AC mode control register 0x5E (Linux patch_cirrus_spdif) */
static BOOLEAN IsCirrusAcMode(ULONG id)
{
    ULONG m = id & 0xFFFFFFF8;
    return m == 0x43525910 || m == 0x43525930 || m == 0x43525948 || m == 0x43525958;
}

/* codec specific set-up that decides whether the analog outputs carry sound at all
   (mirrors the init part of the Linux ac97_patch.c handlers) */
static void CodecQuirks(CARD *c)
{
    AC97_STATE *a = c->Ac97;
    ULONG id = a->VendorId;
    if ((id & 0xFFFFFF00) == 0x41445300) {                  /* Analog Devices */
        UPD(0x74, 0x7000, 0x7000);                           /* serial cfg: all codec slots */
        if (id != 0x41445303) WR(0x70, 0x0000);              /* single codec: no chain ID */
        switch (id & 0xFF) {
        case 0x60: WR(0x76, 0x0404); break;                  /* AD1885: line-out is the HP amp */
        case 0x61: WR(0x72, 0x0010); break;                  /* AD1886: jack sense / SPDIF */
        case 0x68: case 0x70: case 0x75:                     /* AD1888 / AD1980 / AD1985 */
            /* most boards wire the line-out jack to HP_OUT: HP amp <- mixer, LINE_OUT <-
               surround DAC; SPREAD copies the front PCM to surround so both jacks play */
            UPD(0x76, 0x0020 | 0x0400 | 0x0080, 0x0020 | 0x0400 | 0x0080);
            break;
        }
    } else if ((id & 0xFFFFFFF0) == 0x414C4720) {           /* ALC650 */
        UPD(0x78, 0x8000, 0x8000);                           /* GPIO/clock registers writable */
        WR(0x6A, 0x0000);                                    /* multi-channel: no jack sharing */
        UPD(0x76, 0x0001, 0x0001);                           /* GPIO0 output: mic bias */
        UPD(0x78, 0x0110, 0x0100);
        WR(0x64, 0x0808); WR(0x66, 0x0808);                  /* surround / LFE DAC 0 dB */
    } else if ((id & 0xFFFFFFF0) == 0x414C4760 || (id & 0xFFFFFFF0) == 0x414C4780) {  /* ALC655/658 */
        UPD(0x24, 0x000F, 0);                                /* page 0 */
        UPD(0x7A, 0x1000, 0);                                /* mic VREF on */
        WR(0x6A, 0x8000);
        WR(0x64, 0x0808); WR(0x66, 0x0808);
        if ((id & 0xFFFFFFF0) == 0x414C4780 && (RD(0x6E) & 0x3F) == 2) UPD(0x74, 0x0800, 0x0800);
    } else if ((id & 0xFFFFFFF0) == 0x414C4790) {           /* ALC850 */
        UPD(0x24, 0x000F, 0);
        WR(0x6A, 0x8000);
        WR(0x7A, 0x20D2);                                    /* front amp on, VREF, mix */
        WR(0x76, 0x8A90);                                    /* jack functions: front/line/mic */
        WR(0x64, 0x0808); WR(0x66, 0x0808);
    } else if ((id & 0xFFFFFFF0) == 0x414C4770) {           /* ALC203: mono-out on pin 37 */
        UPD(0x7A, 0x0400, 0x0400);
    } else if (id == 0x434D4961) {                           /* CMI9739 */
        USHORT v = (USHORT)((RD(0x64) & 0x0010) | 0x0008 | 0x2000);
        if (!(a->ExtId & 0x0004)) v |= 0x4000;               /* pin 47 = EAPD */
        WR(0x64, v);
    } else if (id == 0x434D4978 || id == 0x434D4982 || id == 0x434D4983) {   /* CMI9761 */
        UPD(0x64, 0x0010, 0x0010);                           /* front output on */
    } else if (id == 0x83847608) {                           /* STAC9708/11 */
        USHORT c72 = (USHORT)(RD(0x72) & 0x8000), c6c = RD(0x6C);
        if (!c72 && !c6c) { WR(0x76, 0xABBA); WR(0x78, 0x1000); WR(0x70, 0xABBA); WR(0x72, 0x0007); }
        else if (c72 && !c6c) { WR(0x76, 0xABBA); WR(0x78, 0x1001); WR(0x6E, 0x0008); }
        WR(0x74, 0x0000);
    } else if (id == 0x83847609) {                           /* STAC9721/23 */
        if (RD(0x6C) == 0) { WR(0x76, 0xABBA); WR(0x78, 0x4000); WR(0x70, 0xABBA); WR(0x72, 0x0002); }
        WR(0x74, 0x0000);
    } else if (id == 0x83847644 || id == 0x83847656) {       /* STAC9744 / 9756 */
        WR(0x76, 0xABBA); WR(0x78, 0x0000); WR(0x70, 0xABBA); WR(0x72, 0x0002);
        WR(0x74, 0x0000);
    } else if (id == 0x83847658) {                           /* STAC9758/59 */
        BOOLEAN m675 = c->SubVendor == 0x107B && c->SubDevice == 0x0601;
        UPD(0x24, 0x000F, 0);
        WR(0x64, m675 ? 0xFC70 : 0xD794);                    /* output select: line-out <- mixer */
        WR(0x68, m675 ? 0x2102 : 0x2001);
        WR(0x66, m675 ? 0x0203 : 0x0201);
        WR(0x72, m675 ? 0x0041 : 0x0040);
    } else if (id == 0x54524108) {                           /* TR28028 */
        WR(0x26, 0x0300); WR(0x26, 0x0000);
        WR(0x38, 0x0000); WR(0x3A, 0x0000);
    } else if (id == 0x49544561) {                           /* IT2646: DAC 0 dB */
        WR(0x5E, 0x0808); WR(0x7A, 0x0808);
    } else if (IsCirrusAcMode(id)) {                         /* CS4297A / CS4299 / CS4201 / CS4205 */
        /* AC mode control: slots by codec ID, DACs through the mixer (DDM off).  A register
           reset leaves this vendor register alone, so whatever the BIOS (ThinkPad SMI
           sound) put there stays without it */
        WR(0x5E, 0x0080);
    }
}

/* attenuation register value (1.5 dB steps) for a slider value */
static ULONG AttSteps(USHORT v, ULONG maxSteps)
{
    ULONG db = VolToDb100(v);
    ULONG s = (db + 75) / 150;
    return s > maxSteps ? maxSteps : s;
}

static BOOLEAN RegExists(CARD *c, ULONG reg, USHORT probe)
{
    USHORT old = RD(reg), v;
    WR(reg, probe);
    v = RD(reg);
    WR(reg, old);
    return v == probe;
}

/* everything the driver sets in the codec (after a reset: at start-up, or when the
   codec turns out to have lost its registers) */
static void CodecProgram(CARD *c)
{
    AC97_STATE *a = c->Ac97;
    WR(AC97_POWERDOWN, a->InvEapd ? 0x8000 : 0x0000);
    CodecQuirks(c);

    /* variable rate: lock everything at 48 kHz */
    if (a->ExtId & 1) {
        WR(AC97_EXT_STATUS, (RD(AC97_EXT_STATUS) & ~0x0002) | 0x0001);
        WR(AC97_PCM_FRONT_RATE, 48000);
        WR(AC97_PCM_SURR_RATE, 48000);
        WR(AC97_PCM_LFE_RATE, 48000);
        WR(AC97_PCM_ADC_RATE, 48000);
    }

    /* master width: 6-bit codecs keep bit 5 */
    WR(AC97_MASTER, 0x8020);
    a->MasterBits = (RD(AC97_MASTER) & 0x3F) == 0x20 ? 6 : 5;
    a->HasHp   = RegExists(c, AC97_HEADPHONE, 0x8808);
    a->HasMono = RegExists(c, AC97_MASTER_MONO, 0x8008);
    a->HasAux  = RegExists(c, AC97_AUX, 0x8808);
    a->HasPcBeep = RegExists(c, AC97_PC_BEEP, 0x8010);

    /* digital paths at 0 dB, every analog output unmuted at 0 dB */
    WR(AC97_PCM, 0x0808);
    a->PcmMark = RD(AC97_PCM);
    /* headphone / alternate line out and mono out unmuted even where the probe did not see
       them: a register the codec does not have ignores the write */
    WR(AC97_HEADPHONE, 0x0000);
    WR(AC97_MASTER_MONO, 0x0000);
    WR(AC97_SURROUND, 0x0000);
    WR(AC97_CENTER_LFE, 0x0000);
    WR(AC97_PHONE, 0x8008);
    WR(AC97_VIDEO, 0x8808);
    WR(AC97_GENERAL, 0x0000);                    /* no 3D, no loopback, mix = pre-3D */
}

NTSTATUS Ac97CodecInit(CARD *c)
{
    AC97_STATE *a = c->Ac97;
    ULONG i, j;
    USHORT v;

    WR(AC97_RESET, 0);
    OsStallUs(100);
    /* wait for codec register access */
    for (i = 0; i < 500; i++) {
        v = RD(AC97_RESET);
        if (v != 0xFFFF) break;
        OsStallUs(1000);
    }
    if (v == 0xFFFF) { LOGE("ac97: codec does not respond"); return STATUS_DEVICE_DOES_NOT_EXIST; }

    WR(AC97_POWERDOWN, 0);                     /* power everything up, EAPD off (amp on) */
    for (i = 0; i < 500; i++) {
        if ((RD(AC97_POWERDOWN) & 0x0F) == 0x0F) break;
        OsStallUs(1000);
    }
    /* the mixer may take writes only a while after the reset and the power-up (a CS4299 in
       the ThinkPad T23 kept PCM, headphone and mono at their muted reset values): wait until
       a write sticks */
    for (i = 0; i < 100; i++) {
        WR(AC97_PCM, 0x0808);
        if (RD(AC97_PCM) == 0x0808) break;
        OsSleepMs(10);
    }
    if (i) LOGI("ac97: the mixer took writes after %u ms%s", i * 10, i == 100 ? " (or never)" : "");
    a->VendorId = ((ULONG)RD(AC97_VENDOR_ID1) << 16) | RD(AC97_VENDOR_ID2);
    a->ExtId = RD(AC97_EXT_ID);

    OsSprintf(c->Codec, sizeof(c->Codec), "AC'97 codec %c%c%c rev %u (%08x)",
              (char)(a->VendorId >> 24), (char)(a->VendorId >> 16), (char)(a->VendorId >> 8),
              (ULONG)(a->VendorId & 0xFF), a->VendorId);
    for (j = 0; g_CodecNames[j].Name; j++)
        if ((a->VendorId & g_CodecNames[j].Mask) == g_CodecNames[j].Id) {
            OsSprintf(c->Codec, sizeof(c->Codec), "%s", g_CodecNames[j].Name);
            break;
        }
    /* EAPD = 0 powers the external amplifier, except on a few boards wired the other way */
    for (j = 0; g_InvEapd[j].Ven; j++)
        if (g_InvEapd[j].Ven == c->SubVendor && g_InvEapd[j].Dev == c->SubDevice) a->InvEapd = TRUE;
    j = OsRegGetDword(L"Ac97InvEapd", 2);
    if (j < 2) a->InvEapd = j != 0;
    CodecProgram(c);

    /* lines */
    c->Lines[LINE_MASTER].Present = TRUE;  c->Lines[LINE_MASTER].HwVolume = TRUE;
    c->Lines[LINE_CD].Present = TRUE;      c->Lines[LINE_CD].HwVolume = TRUE;
    c->Lines[LINE_LINEIN].Present = TRUE;  c->Lines[LINE_LINEIN].HwVolume = TRUE;
    c->Lines[LINE_MIC].Present = TRUE;     c->Lines[LINE_MIC].HwVolume = TRUE;
    c->Lines[LINE_AUX].Present = a->HasAux; c->Lines[LINE_AUX].HwVolume = TRUE;
    c->Lines[LINE_PCSPK].Present = a->HasPcBeep; c->Lines[LINE_PCSPK].HwVolume = TRUE;
    c->Lines[LINE_RECORD].Present = TRUE;  c->Lines[LINE_RECORD].HwVolume = TRUE;
    c->RecSrcMask = RECSRC_MIC | RECSRC_LINE | RECSRC_CD | RECSRC_MIX | (a->HasAux ? RECSRC_AUX : 0);
    c->RecSrc = RECSRC_LINE;

    LOGI("ac97: %s, ext %04x (%s), master %u-bit%s%s%s", c->Codec, a->ExtId,
         (a->ExtId & 1) ? "VRA" : "fixed 48k", a->MasterBits,
         a->HasHp ? ", hp" : "", a->HasAux ? ", aux" : "", a->HasPcBeep ? ", beep" : "");
    return STATUS_SUCCESS;
}

static USHORT StereoGain(LINE_STATE *l)
{
    /* 5-bit gain registers: 0x08 = 0 dB, +1 = -1.5 dB, max 0x1F */
    USHORT left = (USHORT)(8 + AttSteps(l->Left, 23));
    USHORT right = (USHORT)(8 + AttSteps(l->Right, 23));
    USHORT v = (USHORT)((left << 8) | right);
    if (l->Mute || (!l->Left && !l->Right)) v |= 0x8000;
    return v;
}

void Ac97ApplyLine(CARD *c, ULONG line)
{
    AC97_STATE *a = c->Ac97;
    LINE_STATE *l = &c->Lines[line];
    ULONG max = (a->MasterBits == 6) ? 63 : 31;
    USHORT v;
    switch (line) {
    case LINE_MASTER:
        v = (USHORT)((AttSteps(l->Left, max) << 8) | AttSteps(l->Right, max));
        if (l->Mute || (!l->Left && !l->Right)) v |= 0x8000;
        WR(AC97_MASTER, v);
        /* 5-bit registers: clamp the attenuation (a 6-bit value masked to 5 bits wraps
           to a louder setting) */
        v = (USHORT)((v & 0x8000) | (MIN(AttSteps(l->Left, max), 31) << 8) | MIN(AttSteps(l->Right, max), 31));
        WR(AC97_HEADPHONE, v);
        WR(AC97_MASTER_MONO, (USHORT)((v & 0x8000) | (v & 0x1F)));
        WR(AC97_SURROUND, v);
        break;
    case LINE_CD:     WR(AC97_CD, StereoGain(l)); break;
    case LINE_LINEIN: WR(AC97_LINE, StereoGain(l)); break;
    case LINE_AUX:    if (a->HasAux) WR(AC97_AUX, StereoGain(l)); break;
    case LINE_MIC:
        v = (USHORT)(8 + AttSteps((USHORT)((l->Left / 2) + (l->Right / 2)), 23));
        if (l->Mute || !l->Left) v |= 0x8000;
        WR(AC97_MIC, v);
        break;
    case LINE_PCSPK:
        v = (USHORT)(AttSteps(l->Left, 15) << 1);
        if (l->Mute || !l->Left) v |= 0x8000;
        if (a->HasPcBeep) WR(AC97_PC_BEEP, v);
        break;
    case LINE_RECORD: {
        /* 4-bit gain, 0 = 0 dB .. 15 = +22.5 dB; map slider so 0xC000 ~ 0 dB */
        USHORT gl = (USHORT)(((ULONG)l->Left * 15) / 65535), gr = (USHORT)(((ULONG)l->Right * 15) / 65535);
        v = (USHORT)((gl << 8) | gr);
        if (l->Mute) v |= 0x8000;
        WR(AC97_REC_GAIN, v);
        break;
    }
    default: break;
    }
}

/*
 * Called when playback starts.  The BIOS (SMI handlers for notebook volume buttons, APM
 * power management) or a suspend may have reset the codec or powered parts of it down:
 * on a ThinkPad sound comes back only once the mixer is written again.  The PCM
 * register is the marker: the driver keeps it at 0 dB unmuted, the reset value is muted.
 */
void Ac97Refresh(CARD *c)
{
    AC97_STATE *a = c->Ac97;
    USHORT pcm = RD(AC97_PCM), pd, want;
    ULONG k;
    if (pcm == 0xFFFF) return;
    if (pcm != a->PcmMark || (pcm & 0x8000)) {
        /* reset by the BIOS / a suspend, or (muted) a write at start-up did not take */
        if (!a->RefreshLogged) LOGI("ac97: codec registers lost (pcm %04x), setting them again", pcm);
        a->RefreshLogged = TRUE;
        CodecProgram(c);
        for (k = 0; k < LINE_COUNT; k++)
            if (c->Lines[k].Present && c->Lines[k].HwVolume) Ac97ApplyLine(c, k);
        if (c->RecSrc) Ac97ApplyRecSrc(c);
        return;
    }
    pd = RD(AC97_POWERDOWN);
    want = a->InvEapd ? 0x8000 : 0x0000;
    if ((pd & 0xFF00) != want) {
        LOGV("ac97: codec powered down (%04x), powering up", pd);
        WR(AC97_POWERDOWN, want);
    }
    if (IsCirrusAcMode(a->VendorId) && RD(0x5E) != 0x0080) WR(0x5E, 0x0080);
    Ac97ApplyLine(c, LINE_MASTER);
}

void Ac97ApplyRecSrc(CARD *c)
{
    USHORT s = 4;
    switch (c->RecSrc) {
    case RECSRC_MIC:  s = 0; break;
    case RECSRC_CD:   s = 1; break;
    case RECSRC_AUX:  s = 3; break;
    case RECSRC_LINE: s = 4; break;
    case RECSRC_MIX:  s = 5; break;
    }
    WR(AC97_REC_SEL, (s << 8) | s);
}

ULONG Ac97Dump(CARD *c, char *buf, ULONG size)
{
    ULONG n = 0, r;
    EngineLock(c);
    n += OsSprintf(buf + n, size - n, "%s\r\n%s\r\n", c->Desc, c->Codec);
    for (r = 0; r < 0x80 && n + 40 < size; r += 2) {
        n += OsSprintf(buf + n, size - n, "%02x:%04x%s", r, RD(r), (r % 16) == 14 ? "\r\n" : " ");
    }
    EngineUnlock(c);
    return n;
}
