/*
 * AUDIONT - SDCA (MIPI SoundWire Device Class for Audio) helpers and the
 * generic SDCA peripheral driver.
 *
 * The generic driver takes any SoundWire peripheral with class id 1 (SDCA)
 * that no vendor driver claimed and drives it by the standard controls the
 * ACPI DisCo description lists (Linux sound/soc/sdca: sdca_functions.c,
 * sdca_asoc.c):
 *
 *   Device (SWDx) {                      the peripheral, 64-bit _ADR
 *     _DSD: mipi-sdw-sink-port-list, mipi-sdw-source-port-list,
 *           mipi-sdw-dp-N-sink|source-subproperties, mipi-sdw-paging-supported
 *     Device (AFxx) { _ADR (1..7) }      SDCA functions
 *       _DSD: mipi-sdca-control-0x5-subproperties -> mipi-sdca-control-dc-value
 *             (Entity 0 Function_Topology: the function type)
 *             mipi-sdca-entity-id-list, mipi-sdca-entity-id-0xNN-subproperties
 *             -> mipi-sdca-entity-type, mipi-sdca-control-0x10-subproperties
 *             (Clock Source sample rate index: mipi-sdca-control-range)
 *   }
 *
 * Function types: SmartAmp / SimpleAmp -> speaker amplifier, SmartMic /
 * SimpleMic -> microphones, UAJ / SimpleJack / RJ -> headset (jack) codec.
 * Init: every Clock Source to the 48 kHz index of its range table (9 when the
 * table cannot be read - the value Realtek SDCA parts use), every Feature
 * Unit muted with 0 dB volume, every Power Domain Entity in PS3.  Stream on:
 * the function's PDEs to PS0 (waiting for ACTUAL_PS), the Group Entity of a
 * jack to its detected mode, Feature Units unmuted; off: mute, PS3.
 */
#include "sdw.h"

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */
BOOLEAN SdcaPower(SDW_DEV *d, ULONG fn, ULONG pde, ULONG ps, ULONG waitMs)
{
    ULONG t;
    UCHAR v = 0xFF;
    if (!SdwWrite(d, SDW_SDCA_CTL(fn, pde, SDCA_CTL_PDE_REQUESTED_PS, 0), (UCHAR)ps)) return FALSE;
    for (t = 0; t < waitMs; t += 5) {
        if (SdwRead(d, SDW_SDCA_CTL(fn, pde, SDCA_CTL_PDE_ACTUAL_PS, 0), &v) && v == ps) return TRUE;
        SdwDelay(5);
    }
    if (waitMs) LOGI("sdw: %s: PDE %02x of function %u: actual PS %u (requested %u)", d->Name, pde, fn, v, ps);
    return !waitMs;
}

BOOLEAN SdcaMute(SDW_DEV *d, ULONG fn, ULONG fu, ULONG chMask, BOOLEAN mute)
{
    ULONG ch;
    BOOLEAN ok = TRUE;
    for (ch = 1; ch <= 8; ch++)
        if (chMask & (1UL << (ch - 1))) ok &= SdwWrite(d, SDW_SDCA_CTL(fn, fu, SDCA_CTL_FU_MUTE, ch), mute ? 1 : 0);
    return ok;
}

BOOLEAN SdcaVolume(SDW_DEV *d, ULONG fn, ULONG fu, ULONG ctl, ULONG chMask, LONG q78)
{
    ULONG ch;
    BOOLEAN ok = TRUE;
    for (ch = 1; ch <= 8; ch++)
        if (chMask & (1UL << (ch - 1))) ok &= SdwWriteMbq(d, SDW_SDCA_CTL(fn, fu, ctl, ch), 2, (ULONG)q78 & 0xFFFF);
    return ok;
}

BOOLEAN SdcaRate(SDW_DEV *d, ULONG fn, ULONG cs, ULONG index)
{
    return SdwWrite(d, SDW_SDCA_CTL(fn, cs, SDCA_CTL_CS_SAMPLERATEINDEX, 0), (UCHAR)index);
}

/* ------------------------------------------------------------------ */
/* the generic driver                                                   */
/* ------------------------------------------------------------------ */
#define SDCA_FNS  4
typedef struct _SDCA_FN {
    UCHAR Adr, Type, Ge, NPde, NFu, NCs;
    UCHAR Pde[4], Fu[6], Cs[3], Rate[3];
} SDCA_FN;
typedef struct _SDCA_PRIV {
    UCHAR   NFn;
    SDCA_FN Fn[SDCA_FNS];
} SDCA_PRIV;
typedef char SdcaPrivFits[sizeof(SDCA_PRIV) <= sizeof(((SDW_DEV *)0)->Priv) ? 1 : -1];

static SDCA_PRIV *P(SDW_DEV *d) { return (SDCA_PRIV *)d->Priv; }

static BOOLEAN IsAmp(ULONG t) { return t == SDCA_FN_SMART_AMP || t == SDCA_FN_SIMPLE_AMP; }
static BOOLEAN IsMic(ULONG t) { return t == SDCA_FN_SMART_MIC || t == SDCA_FN_SIMPLE_MIC; }
static BOOLEAN IsJack(ULONG t) { return t == SDCA_FN_UAJ || t == SDCA_FN_SIMPLE_JACK || t == SDCA_FN_RJ; }

/* the 48 kHz row of a Clock Source's SampleRateIndex range (DisCo mipi-sdca-control-range: 16-bit columns,
   16-bit rows, then rows x columns 32-bit values: index, rate) */
static UCHAR RateIndex(const AML_DEV *ent)
{
    AML_DEV c;
    UCHAR b[4 + 16 * 8];
    ULONG n, cols, rows, r;
    if (!SdwAcpiSub(ent, "mipi-sdca-control-0x10-subproperties", &c)) return 9;
    n = SdwAcpiBuf(&c, "mipi-sdca-control-range", b, sizeof(b));
    if (n < 4) return 9;
    cols = b[0] | (b[1] << 8);
    rows = b[2] | (b[3] << 8);
    if (cols < 2) return 9;
    for (r = 0; r < rows && 4 + (r + 1) * cols * 4 <= n; r++) {
        const UCHAR *v = b + 4 + r * cols * 4;
        ULONG idx = v[0] | (v[1] << 8) | ((ULONG)v[2] << 16) | ((ULONG)v[3] << 24);
        ULONG rate = v[4] | (v[5] << 8) | ((ULONG)v[6] << 16) | ((ULONG)v[7] << 24);
        if (rate == 48000) return (UCHAR)idx;
    }
    return 9;
}

static void ParseFunction(SDW_DEV *d, ULONG adr, const AML_DEV *f, SDCA_FN *fn)
{
    AML_DEV c5, ent;
    ULONGLONG v;
    ULONG ids[32], n, i;
    char name[48];
    memset(fn, 0, sizeof(*fn));
    fn->Adr = (UCHAR)adr;
    if (SdwAcpiSub(f, "mipi-sdca-control-0x5-subproperties", &c5) && SdwAcpiInt(&c5, "mipi-sdca-control-dc-value", &v))
        fn->Type = (UCHAR)v;
    else if (SdwAcpiInt(f, "mipi-sdca-function-type", &v))
        fn->Type = (UCHAR)v;
    n = SdwAcpiInts(f, "mipi-sdca-entity-id-list", ids, 32);
    for (i = 0; i < n; i++) {
        OsSprintf(name, sizeof(name), "mipi-sdca-entity-id-0x%x-subproperties", ids[i]);
        if (!SdwAcpiSub(f, name, &ent) || !SdwAcpiInt(&ent, "mipi-sdca-entity-type", &v)) continue;
        switch ((ULONG)v) {
        case SDCA_ENT_PDE: if (fn->NPde < 4) fn->Pde[fn->NPde++] = (UCHAR)ids[i]; break;
        case SDCA_ENT_FU:  if (fn->NFu < 6) fn->Fu[fn->NFu++] = (UCHAR)ids[i]; break;
        case SDCA_ENT_CS:  if (fn->NCs < 3) { fn->Rate[fn->NCs] = RateIndex(&ent); fn->Cs[fn->NCs++] = (UCHAR)ids[i]; } break;
        case SDCA_ENT_GE:  fn->Ge = (UCHAR)ids[i]; break;
        }
    }
    LOGV("sdw: %s function %u type %u: %u PDE, %u FU, %u CS%s", d->Name, adr, fn->Type, fn->NPde, fn->NFu, fn->NCs,
         fn->Ge ? ", GE" : "");
}

/* the lowest data port of a port list and its DisCo subproperties */
static void Port(SDW_DEV *d, const char *list, const char *dir, SDW_DPN *p)
{
    ULONGLONG v;
    ULONG n;
    AML_DEV sub;
    char name[48];
    memset(p, 0, sizeof(*p));
    if (!SdwAcpiInt(&d->Acpi, list, &v) || !(v & 0x7FFE)) return;
    for (n = 1; n < 15 && !(v & (1UL << n)); n++) ;
    p->Num = (UCHAR)n;
    p->Ch = 2;
    p->Type = SDW_DP_FULL;
    OsSprintf(name, sizeof(name), "mipi-sdw-dp-%u-%s-subproperties", n, dir);
    if (SdwAcpiSub(&d->Acpi, name, &sub)) {
        if (SdwAcpiInt(&sub, "mipi-sdw-data-port-type", &v) && v <= 2) p->Type = (UCHAR)v;
        if ((SdwAcpiInt(&sub, "mipi-sdw-simplified-channelprepare-sm", &v) ||
             SdwAcpiInt(&sub, "mipi-sdw-simplified-channel-prepare-sm", &v)) && !v) p->CpSm = TRUE;
    }
}

static BOOLEAN Probe(SDW_DEV *d)
{
    SDCA_PRIV *x = P(d);
    AML_DEV f;
    ULONG a;
    BOOLEAN amp = FALSE, mic = FALSE, jack = FALSE;
    memset(x, 0, sizeof(*x));
    for (a = 1; a <= 7 && x->NFn < SDCA_FNS; a++) {
        if (!SdwAcpiFunction(d, a, &f)) continue;
        ParseFunction(d, a, &f, &x->Fn[x->NFn]);
        amp |= IsAmp(x->Fn[x->NFn].Type);
        mic |= IsMic(x->Fn[x->NFn].Type);
        jack |= IsJack(x->Fn[x->NFn].Type);
        x->NFn++;
    }
    if (d->Class != 1 && !x->NFn) return FALSE;                     /* neither SDCA nor described */
    Port(d, "mipi-sdw-sink-port-list", "sink", &d->Play);
    Port(d, "mipi-sdw-source-port-list", "source", &d->Rec);
    d->Kind = jack ? SDW_KIND_HEADSET : amp ? SDW_KIND_AMP : mic ? SDW_KIND_MIC :
              d->Play.Num && d->Rec.Num ? SDW_KIND_HEADSET : d->Play.Num ? SDW_KIND_AMP : SDW_KIND_MIC;
    if (d->Kind == SDW_KIND_AMP) memset(&d->Rec, 0, sizeof(d->Rec));    /* feedback (I/V sense) not used */
    if (d->Kind == SDW_KIND_MIC) memset(&d->Play, 0, sizeof(d->Play));
    d->Paging = TRUE;                                                   /* SDCA controls are above 0x40000000 */
    return d->Play.Num || d->Rec.Num;
}

static BOOLEAN Init(SDW_DEV *d)
{
    SDCA_PRIV *x = P(d);
    ULONG i, k;
    for (i = 0; i < x->NFn; i++) {
        SDCA_FN *f = &x->Fn[i];
        if (f->Type == SDCA_FN_HID) continue;
        for (k = 0; k < f->NCs; k++) SdcaRate(d, f->Adr, f->Cs[k], f->Rate[k]);
        for (k = 0; k < f->NFu; k++) {
            SdcaMute(d, f->Adr, f->Fu[k], 3, TRUE);
            SdcaVolume(d, f->Adr, f->Fu[k], SDCA_CTL_FU_VOLUME, 3, 0);  /* 0 dB */
        }
        for (k = 0; k < f->NPde; k++) SdcaPower(d, f->Adr, f->Pde[k], SDCA_PS3, 0);
    }
    LOGI("sdw: %s: generic SDCA, %u functions", d->Name, x->NFn);
    return d->Errors < 64;
}

static void Function(SDW_DEV *d, SDCA_FN *f, BOOLEAN on)
{
    ULONG k;
    if (on) {
        for (k = 0; k < f->NPde; k++) SdcaPower(d, f->Adr, f->Pde[k], SDCA_PS0, 200);
        if (f->Ge) {
            UCHAR m;
            if (SdwRead(d, SDW_SDCA_CTL(f->Adr, f->Ge, SDCA_CTL_GE_DETECTED_MODE, 0), &m) && m)
                SdwWrite(d, SDW_SDCA_CTL(f->Adr, f->Ge, SDCA_CTL_GE_SELECTED_MODE, 0), m);
        }
        for (k = 0; k < f->NFu; k++) SdcaMute(d, f->Adr, f->Fu[k], 3, FALSE);
    } else {
        for (k = 0; k < f->NFu; k++) SdcaMute(d, f->Adr, f->Fu[k], 3, TRUE);
        for (k = 0; k < f->NPde; k++) SdcaPower(d, f->Adr, f->Pde[k], SDCA_PS3, 0);
    }
}

static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    SDCA_PRIV *x = P(d);
    BOOLEAN play = rec ? d->PlayOn : on, cap = rec ? on : d->RecOn, was = d->PlayOn || d->RecOn;
    ULONG i;
    for (i = 0; i < x->NFn; i++) {
        SDCA_FN *f = &x->Fn[i];
        if (IsAmp(f->Type)) { if (!rec) Function(d, f, play); }
        else if (IsMic(f->Type)) { if (rec) Function(d, f, cap); }
        else if (IsJack(f->Type) && was != (play || cap)) Function(d, f, play || cap);   /* both directions */
    }
}

static void Shutdown(SDW_DEV *d)
{
    SDCA_PRIV *x = P(d);
    ULONG i;
    for (i = 0; i < x->NFn; i++) if (x->Fn[i].Type != SDCA_FN_HID) Function(d, &x->Fn[i], FALSE);
}

const SDW_DRV g_SdwSdca = { "SDCA", 0, NULL, SDW_KIND_HEADSET, Probe, Init, Stream, Shutdown };
