/*
 * AUDIONT - helpers shared by the Realtek SoundWire peripheral drivers (see sdw_rtcom.h).
 *
 *   index registers   rt7xx_sdca_index_write / _read (regmap-sdw-mbq, 2 bytes)
 *   HDA verbs         rt700_sdw_write / rt700_sdw_read (identical in rt711-sdw.c, rt715-sdw.c)
 *   bus clock         rt700_clock_config: curr_dr_freq / 2 -> code in 0xe0 / 0xf0; the link
 *                     bus clock is not known to the peripheral drivers, so it is taken from the
 *                     same ACPI link subproperties the bus core uses (sdw.c LinkProps)
 *   port mirror       for the multi-function SDCA codecs (one playback stream per peripheral in
 *                     the bus core): a second sink port with the transport parameters of the first
 */
#include "sdw_rtcom.h"

/* ------------------------------------------------------------------ */
/* SDCA index registers                                                 */
/* ------------------------------------------------------------------ */
BOOLEAN SdwRtIdxWr(SDW_DEV *d, ULONG nid, ULONG reg, ULONG v)
{
    return SdwWriteMbq(d, (nid << 20) | reg, 2, v & 0xFFFF);
}

BOOLEAN SdwRtIdxRd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG *v)
{
    return SdwReadMbq(d, (nid << 20) | reg, 2, v);
}

BOOLEAN SdwRtIdxUpd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;
    if (!SdwRtIdxRd(d, nid, reg, &v)) return FALSE;
    return SdwRtIdxWr(d, nid, reg, (v & ~mask) | (val & mask));
}

/* ------------------------------------------------------------------ */
/* HDA verbs over SoundWire                                             */
/* ------------------------------------------------------------------ */
#define RT_READ_HDA_3       0x2012          /* .. RT_READ_HDA_0 0x2015: the verb response, MSB first */
#define RT_PRIV_DATA_W_H    0x7400          /* + nid (rt700 / rt715: 0x7420 | nid, the same for their NID 20h) */
#define RT_PRIV_DATA_R_H    0x9c00          /* + nid (rt700: 0x9c20 | nid, only used with NID 20h) */

static ULONG Lo(ULONG r) { return (r + 0x1000) | 0x80; }  /* the low byte register of a 16-bit payload */

BOOLEAN SdwRtHdaWr(SDW_DEV *d, ULONG reg, ULONG val)
{
    BOOLEAN ok = TRUE;
    if (reg > 0xffff) {                                 /* index register */
        ULONG idx = reg & 0xff, r = reg >> 8, nid = r & 0xff;
        ok &= SdwWrite(d, r, 0);
        ok &= SdwWrite(d, Lo(r), (UCHAR)idx);
        r = RT_PRIV_DATA_W_H | nid;
        ok &= SdwWrite(d, r, (UCHAR)(val >> 8));
        ok &= SdwWrite(d, Lo(r), (UCHAR)val);
    } else if (reg < 0x4fff || reg == RT_FUNC_RESET) {
        ok = SdwWrite(d, reg, (UCHAR)val);
    } else if ((reg & 0xf000) == 0x7000) {
        ok &= SdwWrite(d, reg, (UCHAR)(val >> 8));
        ok &= SdwWrite(d, Lo(reg), (UCHAR)val);
    } else if ((reg & 0xff00) == 0x8300) {              /* R channel form of SET_AMP_GAIN_MUTE */
        ok &= SdwWrite(d, (reg - 0x1000) & ~0x80UL, (UCHAR)(val >> 8));
        ok &= SdwWrite(d, reg, (UCHAR)val);
    } else {
        return FALSE;
    }
    return ok;
}

BOOLEAN SdwRtHdaRd(SDW_DEV *d, ULONG reg, ULONG *val)
{
    ULONG in = *val, mask = reg & 0xf000, i, v = 0;
    BOOLEAN ok = TRUE;
    UCHAR b;
    if (reg > 0xffff) {
        ULONG idx = reg & 0xff, r = reg >> 8, nid = r & 0xff;
        ok &= SdwWrite(d, r, 0);
        ok &= SdwWrite(d, Lo(r), (UCHAR)idx);
        r = RT_PRIV_DATA_R_H | nid;
        ok &= SdwWrite(d, r, (UCHAR)(in >> 8));
        ok &= SdwWrite(d, Lo(r), (UCHAR)in);
    } else if (mask == 0x3000) {                        /* SET verb -> its GET form (verb + 0x800) */
        ok = SdwWrite(d, reg + 0x8000, (UCHAR)in);
    } else if (mask == 0x7000) {
        ULONG r = (reg + 0x2000) | 0x800;
        ok &= SdwWrite(d, r, (UCHAR)(in >> 8));
        ok &= SdwWrite(d, Lo(r), (UCHAR)in);
    } else if ((reg & 0xff00) == 0x8300) {
        ok &= SdwWrite(d, (reg - 0x1000) & ~0x80UL, (UCHAR)(in >> 8));
        ok &= SdwWrite(d, reg, (UCHAR)in);
    } else if (mask == 0x9000) {
        ok &= SdwWrite(d, reg, (UCHAR)(in >> 8));
        ok &= SdwWrite(d, Lo(reg), (UCHAR)in);
    } else if (mask == 0xb000) {
        ok = SdwWrite(d, reg, (UCHAR)in);
    } else {                                            /* plain SoundWire register */
        if (!SdwRead(d, reg, &b)) return FALSE;
        *val = b;
        return TRUE;
    }
    for (i = 0; i < 4 && ok; i++) {
        ok &= SdwRead(d, RT_READ_HDA_3 + i, &b);
        v = (v << 8) | b;
    }
    if (ok) *val = v;
    return ok;
}

BOOLEAN SdwRtHdaSeq(SDW_DEV *d, const ULONG *rv, ULONG n)
{
    ULONG i;
    BOOLEAN ok = TRUE;
    for (i = 0; i < n; i++) ok &= SdwRtHdaWr(d, rv[2 * i], rv[2 * i + 1]);
    return ok;
}

BOOLEAN SdwRtHdaIdxWr(SDW_DEV *d, ULONG nid, ULONG reg, ULONG v)
{
    return SdwRtHdaWr(d, ((0x7500UL | nid) << 8) | reg, v);
}

BOOLEAN SdwRtHdaIdxRd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG *v)
{
    *v = 0;
    return SdwRtHdaRd(d, ((0x7500UL | nid) << 8) | reg, v);
}

BOOLEAN SdwRtHdaIdxUpd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;
    if (!SdwRtHdaIdxRd(d, nid, reg, &v)) return FALSE;
    return SdwRtHdaIdxWr(d, nid, reg, ((v & ~mask) | (val & mask)) & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* bus clock                                                            */
/* ------------------------------------------------------------------ */
typedef struct { const char *Name; AML_DEV Dev; BOOLEAN Found; } RT_CTRL;

static BOOLEAN IsSdwCtrl(const AML_DEV *a)
{
    ULONGLONG adr;
    return AmlAdr(a, &adr) && adr == 0x40000000ULL;
}

static BOOLEAN FindCtrl(const AML_DEV *a, void *ctx)
{
    RT_CTRL *c = (RT_CTRL *)ctx;
    if (!IsSdwCtrl(a) || (c->Name && memcmp(a->Name, c->Name, 4))) return TRUE;
    c->Dev = *a;
    c->Found = TRUE;
    return FALSE;
}

ULONG SdwRtBusClock(SDW_DEV *d)
{
    RT_CTRL c;
    AML_DEV p, sub;
    ULONGLONG v;
    ULONG f[8], n, ip = 38400000, mclk;
    char name[40];
    memset(&c, 0, sizeof(c));
    if (AmlParent(&d->Acpi, &p) && IsSdwCtrl(&p)) {
        c.Dev = p;
        c.Found = TRUE;
    } else {
        /* Scope (\_SB.PC00.HDAS.SNDW) { Device (SWD0) .. } in an SSDT: the controller of that name */
        if (AmlParent(&d->Acpi, &p)) c.Name = p.Name;
        AmlForEachDevice(FindCtrl, &c);
    }
    if (!c.Found) return 0;
    OsSprintf(name, sizeof(name), "mipi-sdw-link-%u-subproperties", d->Link);
    if (!SdwAcpiSub(&c.Dev, name, &sub)) return 0;
    if (SdwAcpiInt(&sub, "mipi-sdw-max-clock-frequency", &v) && v) return (ULONG)v;
    n = SdwAcpiInts(&sub, "mipi-sdw-clock-frequencies-supported", f, 8);
    if (n && f[0]) return f[0];
    if (SdwAcpiInt(&sub, "intel-sdw-ip-clock", &v) && v) ip = (ULONG)v;
    mclk = ip / 2;                                      /* as sdw.c: the BIOS reports twice the IP clock */
    return !(mclk % 4800000) ? 4800000 : !(mclk % 6000000) ? 6000000 : mclk / 4;
}

BOOLEAN SdwRtClockConfig(SDW_DEV *d)
{
    ULONG clk = SdwRtBusClock(d);
    UCHAR v;
    switch (clk) {
    case 12000000: v = 0; break;
    case 6000000:  v = 1; break;
    case 9600000:  v = 2; break;
    case 4800000:  v = 3; break;
    case 2400000:  v = 4; break;
    case 12288000: v = 5; break;
    default:
        LOGE("sdw: %s: bus clock %u Hz not supported by the clock config", d->Name, clk);
        return FALSE;
    }
    return SdwWrite(d, 0xe0, v) && SdwWrite(d, 0xf0, v);
}

/* ------------------------------------------------------------------ */
/* SDCA functions in ACPI                                               */
/* ------------------------------------------------------------------ */
ULONG SdwRtFuncs(SDW_DEV *d)
{
    AML_DEV f, c5;
    ULONGLONG v;
    ULONG a, m = 0;
    BOOLEAN typed = FALSE;
    for (a = 1; a <= 7; a++) {
        if (!SdwAcpiFunction(d, a, &f)) continue;
        if (!(SdwAcpiSub(&f, "mipi-sdca-control-0x5-subproperties", &c5) &&
              SdwAcpiInt(&c5, "mipi-sdca-control-dc-value", &v)) &&
            !SdwAcpiInt(&f, "mipi-sdca-function-type", &v)) continue;
        typed = TRUE;
        switch ((ULONG)v) {
        case SDCA_FN_SMART_AMP: case SDCA_FN_SIMPLE_AMP: m |= RT_FN_AMP; break;
        case SDCA_FN_SMART_MIC: case SDCA_FN_SIMPLE_MIC: case SDCA_FN_SPEAKER_MIC: m |= RT_FN_MIC; break;
        case SDCA_FN_UAJ: case SDCA_FN_RJ: case SDCA_FN_SIMPLE_JACK: m |= RT_FN_JACK; break;
        }
    }
    return typed ? m : RT_FN_ALL;
}

BOOLEAN SdwRtPreferDmic(void)
{
    return OsRegGetDword(L"SdwDmic", 0) != 0;
}

/* ------------------------------------------------------------------ */
/* port mirror                                                          */
/* ------------------------------------------------------------------ */
static BOOLEAN Copy(SDW_DEV *d, ULONG from, ULONG to)
{
    UCHAR v;
    return SdwRead(d, from, &v) && SdwWrite(d, to, v);
}

BOOLEAN SdwRtMirror(SDW_DEV *d, const SDW_DPN *src, ULONG to)
{
    ULONG n = src->Num, i, b, cur;
    UCHAR st = 0, pc;
    BOOLEAN ok;
    if (!n || !to || n == to) return FALSE;
    SdwRead(d, SDW_SCP_CTRL, &st);                      /* SCP_Stat: the current bank */
    cur = (st & SDW_SCP_STAT_CURR_BANK) ? 1 : 0;
    ok = SdwRead(d, SDW_DPN_PORTCTRL(n), &pc) && SdwUpdate(d, SDW_DPN_PORTCTRL(to), 0x0F, pc);
    ok &= Copy(d, SDW_DPN_BLOCKCTRL1(n), SDW_DPN_BLOCKCTRL1(to));
    for (i = 0; i < 2 && ok; i++) {
        b = i ? cur : !cur;                             /* the inactive bank first */
        ok &= Copy(d, SDW_DPN_SAMPLECTRL1(n, b), SDW_DPN_SAMPLECTRL1(to, b));
        ok &= Copy(d, SDW_DPN_SAMPLECTRL2(n, b), SDW_DPN_SAMPLECTRL2(to, b));
        ok &= Copy(d, SDW_DPN_OFFSETCTRL1(n, b), SDW_DPN_OFFSETCTRL1(to, b));
        ok &= Copy(d, SDW_DPN_OFFSETCTRL2(n, b), SDW_DPN_OFFSETCTRL2(to, b));
        ok &= Copy(d, SDW_DPN_HCTRL(n, b), SDW_DPN_HCTRL(to, b));
        ok &= Copy(d, SDW_DPN_BLOCKCTRL3(n, b), SDW_DPN_BLOCKCTRL3(to, b));
        ok &= Copy(d, SDW_DPN_CHANNELEN(n, b), SDW_DPN_CHANNELEN(to, b));
    }
    if (!ok) LOGE("sdw: %s: DP%u as a copy of DP%u failed", d->Name, to, n);
    else LOGV("sdw: %s: DP%u = DP%u (bank %u: channels %x, offset %x, columns %x)", d->Name, to, n, cur,
              SdwGet(d, SDW_DPN_CHANNELEN(to, cur)), SdwGet(d, SDW_DPN_OFFSETCTRL1(to, cur)),
              SdwGet(d, SDW_DPN_HCTRL(to, cur)));
    return ok;
}

void SdwRtMirrorOff(SDW_DEV *d, ULONG port)
{
    if (!port) return;
    SdwWrite(d, SDW_DPN_CHANNELEN(port, 0), 0);
    SdwWrite(d, SDW_DPN_CHANNELEN(port, 1), 0);
}

/* ------------------------------------------------------------------ */
/* jack Group Entity                                                    */
/* ------------------------------------------------------------------ */
void SdwRtJackMode(SDW_DEV *d, ULONG fn, ULONG ge)
{
    UCHAR m = 0;
    SdwRead(d, SDW_SDCA_CTL(fn, ge, SDCA_CTL_GE_DETECTED_MODE, 0), &m);
    SdwWrite(d, SDW_SDCA_CTL(fn, ge, SDCA_CTL_GE_SELECTED_MODE, 0), m ? m : 5);
}
