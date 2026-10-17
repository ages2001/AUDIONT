/*
 * AUDIONT - SoundWire: Intel SoundWire controller (Cadence masters next to
 * the Smart Sound DSP) and its peripherals.
 *
 * Follows the Linux drivers/soundwire (intel.c, intel_ace2x.c,
 * intel_bus_common.c, cadence_master.c, bus.c, stream.c,
 * generic_bandwidth_allocation.c) and sound/soc/sof/intel (hda.c, mtl.c,
 * hda-mlink.c, hda-dai.c):
 *
 *   cAVS 1.8 .. 2.5 (CNL .. RPL), ACE 1.x (MTL, ARL)
 *       DSP BAR: shim (LCAP / LCTL / SYNC / PCMSyCHM / IOCTL) at 0x2C000
 *       (ACE 1.x 0x38000), ALH stream configuration at 0x2C800 (0x24000),
 *       Cadence masters at 0x30000 + 0x10000 * link.  The links are brought
 *       up after the DSP firmware runs (Linux hda_dsp_post_fw_run /
 *       mtl_dsp_post_fw_run); MTL needs HFPWRCTL.WPIOXPG(1), which sst.c
 *       sets when it powers the DSP.
 *   ACE 2.x+ (LNL, PTL, WCL, NVL)
 *       the HD Audio multi-link SoundWire extended link (LEPTR id 0x00):
 *       per-sublink power (LCTL.SPA / CPA), SYNCPRD in LSYNC; per link at
 *       LEPTR.PTR + 0x8000 * link in the HD Audio BAR: generic shim (+0,
 *       PCMSyCM maps a PDI to an HD Audio link DMA stream tag), Cadence IP
 *       (+0x100, MCP_IP registers at +0x4000), vendor shim (+0x6000).
 *       Brought up at probe before the DSP boots (Linux hda_dsp_probe).
 *
 * Bus configuration (fixed, see sdw.h): the IP clock / bus clock / frame
 * rows of the ACPI link subproperties, single-master mode (no gsync / SYNCGO:
 * Linux md_flags SDW_INTEL_MASTER_DISABLE_MULTI_LINK), a stream per endpoint
 * direction on PDI 2, 3, .. of each link (the Linux "SDW<link> Pin<n>" DAIs
 * of the SOF topologies), ALH stream id link * 16 + pdi + 5.  Commands are
 * polled (no SoundWire interrupts).
 */
#include "sdw.h"

#define G32U(p) ((ULONG)(p)[0] | ((ULONG)(p)[1] << 8) | ((ULONG)(p)[2] << 16) | ((ULONG)(p)[3] << 24))

/* ------------------------------------------------------------------ */
/* Cadence master (cadence_master.c)                                    */
/* ------------------------------------------------------------------ */
#define MCP_CONFIG              0x00
#define  MCP_CONFIG_BUS_REL     (1UL << 6)
#define IP_MCP_CONFIG           0x00            /* + ip offset */
#define  IP_CONFIG_MMASTER      (1UL << 7)
#define  IP_CONFIG_SNIFFER      (1UL << 5)
#define  IP_CONFIG_CMD          (1UL << 3)
#define  IP_CONFIG_OP_MASK      7UL
#define MCP_CONTROL             0x04
#define  MCP_CONTROL_CMD_RST    (1UL << 7)
#define  MCP_CONTROL_SOFT_RST   (1UL << 6)
#define  MCP_CONTROL_HW_RST     (1UL << 4)
#define IP_MCP_CONTROL          0x04            /* + ip offset */
#define  IP_CONTROL_CMD_ACCEPT  (1UL << 1)
#define  IP_CONTROL_BLOCK_WAKEUP 1UL
#define MCP_FRAME_SHAPE_INIT    0x14
#define MCP_CONFIG_UPDATE       0x18
#define MCP_SSP_CTRL0           0x20
#define MCP_SSP_CTRL1           0x28
#define MCP_CLK_CTRL0           0x30
#define MCP_CLK_CTRL1           0x38
#define MCP_STAT                0x40
#define  MCP_STAT_ACTIVE_BANK   (1UL << 20)
#define MCP_INTSTAT             0x44
#define MCP_INTMASK             0x48
#define  MCP_INT_RX_WL          (1UL << 2)
#define MCP_SLAVE_STAT          0x50            /* 2 bits per device number */
#define MCP_SLAVE_INTSTAT0      0x54
#define MCP_SLAVE_INTSTAT1      0x58
#define MCP_SLAVE_INTMASK0      0x5C
#define MCP_SLAVE_INTMASK1      0x60
#define MCP_FIFOLEVEL           0x78
#define MCP_FIFOSTAT            0x7C
#define IP_MCP_CMD_BASE         0x80            /* + ip offset: command / response FIFO */
#define  CMD_SSP_TAG            (1UL << 31)
#define  CMD_READ               2UL
#define  CMD_WRITE              3UL
#define  RESP_ACK               1UL
#define  RESP_NACK              2UL
#define DPN_CONFIG(n, b)        (0x100 + 0x80 * (n) + 0x18 * (b))
#define DPN_CH_EN(n, b)         (0x104 + 0x80 * (n) + 0x18 * (b))
#define DPN_SAMPLE_CTRL(n, b)   (0x108 + 0x80 * (n) + 0x18 * (b))
#define DPN_OFFSET_CTRL(n, b)   (0x10C + 0x80 * (n) + 0x18 * (b))
#define DPN_HCTRL(n, b)         (0x110 + 0x80 * (n) + 0x18 * (b))
#define DPN_PORTCTRL(n)         (0x130 + 0x80 * (n))
#define  PORTCTRL_DIRN          (1UL << 7)
#define  PORTCTRL_TEST_FAILED   (1UL << 1)
#define  PORTCTRL_BULK_ENABLE   (1UL << 16)
#define PDI_CONFIG(n)           (0x1100 + 16 * (n))
#define  PDI_CONFIG_SOFT_RESET  (1UL << 24)
#define CDNS_IP_OFFSET_ACE2     0x4000
#define GSYNC_HZ                4000            /* SDW_CADENCE_GSYNC_HZ */

/* cAVS / ACE 1.x shim (sdw_intel.h) */
#define SHIM_LCAP               0x000
#define  LCAP_MLCS              (1UL << 8)
#define SHIM_LCTL               0x004
#define  LCTL_SPA(m)            ((ULONG)(m))
#define  LCTL_CPA(m)            ((ULONG)(m) << 8)
#define  LCTL_MLCS_SHIFT        27
#define SHIM_SYNC               0x00C
#define  SYNC_SYNCPRD_MASK      0x7FFFUL
#define  SYNC_SYNCCPU           (1UL << 15)
#define SHIM_PCMSCAP(x)         (0x020 + 0x60 * (x))
#define SHIM_PCMSYCHM(x, y)     (0x022 + 0x60 * (x) + 2 * (y))
#define SHIM_PCMSYCHC(x, y)     (0x042 + 0x60 * (x) + 2 * (y))
#define SHIM_IOCTL(x)           (0x06C + 0x60 * (x))
#define  IOCTL_MIF              0x0001
#define  IOCTL_CO               0x0002
#define  IOCTL_COE              0x0004
#define  IOCTL_DO               0x0008
#define  IOCTL_DOE              0x0010
#define  IOCTL_BKE              0x0020
#define  IOCTL_WPDD             0x0040
#define SHIM_CTMCTL(x)          (0x06E + 0x60 * (x))
#define  CTMCTL_DACTQE          0x0001
#define  CTMCTL_DODS            0x0002
#define  CTMCTL_DOAIS_SHIFT     3
#define ALH_STRMZCFG(x)         (4 * (x))
#define MLCS_XTAL               0
#define MLCS_CARDINAL           1
#define MLCS_AUDIO_PLL          2
/* ACE 2.x shim2 (per link) */
#define SHIM2_PCMSCAP           0x10
#define SHIM2_PCMSYCHC(y)       (0x14 + 4 * (y))
#define SHIM2_PCMSYCM(y)        (0x16 + 4 * (y))
#define SHIM2_IP                0x100
#define SHIM2_VS                0x6000
#define VS_LVSCTL               0x04
#define VS_ACTMCTL              0x0E
/* HD Audio multi-link (hda_register.h) */
#define ELID_SDW                0x00
#define ML_LCAP                 0x00
#define ML_LCTL                 0x04
#define ML_LOSIDV               0x08
#define ML_LSYNC                0x1C
#define  LSYNC_SYNCPRD_MASK     0xFFFFFUL
#define  LSYNC_SYNCPU           (1UL << 20)
#define ML_LEPTR                0x20

/* ------------------------------------------------------------------ */
/* platforms (Linux sof/intel cnl.c, icl.c, tgl.c, mtl.c, lnl.c, ptl.c, nvl.c) */
/* ------------------------------------------------------------------ */
typedef struct _SDW_CHIP {
    UCHAR Gen;
    UCHAR Ext;                  /* ACE 2.x+: HD Audio multi-link + HD Audio BAR */
    ULONG Shim, Alh;            /* DSP BAR offsets (sdw_shim_base, sdw_alh_base) */
    ULONG Link, LinkSize;       /* SDW_LINK_BASE / SIZE (Ext: LEPTR.PTR, SDW_INSTANCE_OFFSET) */
    ULONG IpClock;              /* default intel-sdw-ip-clock when ACPI has none */
} SDW_CHIP;

static const SDW_CHIP g_Chips[] = {
    { SST_CNL, 0, 0x2C000, 0x2C800, 0x30000, 0x10000, 24000000 },      /* cAVS 1.8 */
    { SST_ICL, 0, 0x2C000, 0x2C800, 0x30000, 0x10000, 38400000 },      /* cAVS 2.0 */
    { SST_TGL, 0, 0x2C000, 0x2C800, 0x30000, 0x10000, 38400000 },      /* cAVS 2.5 */
    { SST_MTL, 0, 0x38000, 0x24000, 0x30000, 0x10000, 38400000 },      /* ACE 1.x: SDW_SHIM_BASE_ACE */
    { SST_LNL, 1, 0, 0, 0x30000, 0x8000, 38400000 },                   /* ACE 2.0 */
    { SST_PTL, 1, 0, 0, 0x30000, 0x8000, 38400000 },                   /* ACE 3.0 */
    { SST_NVL, 1, 0, 0, 0x30000, 0x8000, 38400000 },                   /* ACE 4.0 */
};

/* SoundWire 1.2 frame shapes (stream.c sdw_rows / sdw_cols) */
static const USHORT g_Rows[24] = { 48, 50, 60, 64, 75, 80, 125, 147, 96, 100, 120, 128, 150, 160, 250, 0,
                                   192, 200, 240, 256, 72, 144, 90, 180 };
static const UCHAR g_Cols[8] = { 2, 4, 6, 8, 10, 12, 14, 16 };

/* ------------------------------------------------------------------ */
/* state                                                                */
/* ------------------------------------------------------------------ */
#define SDW_LINKS     5             /* SDW_INTEL_MAX_LINKS */
#define SDW_DEVS      12
#define SDW_STREAMS   12
#define SDW_MAX_DEVNUM 11           /* SDW_MAX_DEVICES */
#define SDW_BROADCAST 15
#define SDW_RATE      48000
#define SDW_BITS      24
#define QLEN          16

typedef struct _SDW_STRM {
    UCHAR   Link, Pdi, Dir, Ch;
    UCHAR   Ep;                 /* index in s->Ep */
    UCHAR   NDev;
    UCHAR   Dev[4];             /* peripherals (indices in D) */
    SDW_XPORT X;
    BOOLEAN Up;
} SDW_STRM;

typedef struct _SDW_LINK {
    BOOLEAN Acpi;               /* described and enabled in ACPI */
    BOOLEAN Powered, Up;        /* link power / bus running */
    PUCHAR  Regs;               /* Cadence */
    ULONG   Ip;                 /* MCP_IP offset */
    PUCHAR  Shim2, Vs;          /* ACE 2.x per-link shims */
    ULONG   IpClock, Mclk, BusClock, DrFreq, Row, Col, FrameRate;
    ULONG   Pdis;               /* PCM PDIs (bidirectional) */
    UCHAR   Bank;               /* active bank */
    UCHAR   NextPdi;
    ULONG   MsgCount;
    ULONG   Assigned;           /* device numbers in use */
    ULONG   Unknown;            /* enumerated devices without an ACPI description */
    ULONG   Cmds, Errors;
    USHORT  Clde, Doaise2, Dodse2, Clds, Clss, Doaise, Doais, Dodse, Dods;   /* ACE 2.x intel-sdw-* */
} SDW_LINK;

typedef struct _SDW_GW {        /* ALH gateway configuration of an endpoint (IPC4 copier blob) */
    UCHAR   Ep;
    ULONG   Blob[2 + 2 * 4];
} SDW_GW;

typedef struct _SDW {
    SST     *Sst;
    const SDW_CHIP *Chip;
    AML_DEV Ctrl;
    ULONG   Count, Mask;        /* links in ACPI / enabled */
    ULONG   MlOff;              /* ACE 2.x: multi-link SoundWire link */
    PUCHAR  Win;                /* ACE 2.x: HD Audio BAR window of the links */
    ULONG   WinPa, WinSize;
    BOOLEAN Started;
    SDW_LINK L[SDW_LINKS];
    SDW_DEV D[SDW_DEVS];
    ULONG   ND;
    SDW_STRM S[SDW_STREAMS];
    ULONG   NS;
    SDW_GW  G[SST_MAX_EP];
    ULONG   NG;
    /* worker */
    OSLOCK  Lock;
    OSEVENT Ev, Done;
    struct { UCHAR Dev, Rec, On; } Q[QLEN];
    ULONG   Head, Tail;
    BOOLEAN Thread, Quit;
} SDW;

/* peripheral drivers: vendor drivers first, the generic SDCA driver takes the rest */
extern const SDW_DRV g_SdwRt711, g_SdwRt1316;
extern const SDW_DRV g_SdwCs42l43;
extern const SDW_DRV g_SdwRt1318, g_SdwRt1320, g_SdwRt1308, g_SdwMax98373, g_SdwMax98363, g_SdwCs35l56;
extern const SDW_DRV g_SdwRt712, g_SdwRt722, g_SdwRt715, g_SdwRt700, g_SdwRt5682;
const SDW_DRV * const g_SdwDrvs[] = { &g_SdwRt711, &g_SdwRt1316,
                                      &g_SdwRt712, &g_SdwRt722, &g_SdwRt715, &g_SdwRt700, &g_SdwRt5682,
                                      &g_SdwRt1318, &g_SdwRt1320, &g_SdwRt1308, &g_SdwMax98373, &g_SdwMax98363,
                                      &g_SdwCs35l56, &g_SdwCs42l43, &g_SdwSdca, NULL };

static SDW *Of(SST *s) { return s ? (SDW *)s->Sdw : NULL; }
static SDW_LINK *LinkOf(SDW_DEV *d) { SDW *w = Of(d->Sst); return w ? &w->L[d->Link] : NULL; }

void SdwDelay(ULONG ms) { OsSleepMs(ms); }

/* ------------------------------------------------------------------ */
/* register access                                                      */
/* ------------------------------------------------------------------ */
static ULONG CR(SDW_LINK *k, ULONG o) { return MMIO32(k->Regs, o); }
static void  CW(SDW_LINK *k, ULONG o, ULONG v) { MMIO32(k->Regs, o) = v; }
static void  CU(SDW_LINK *k, ULONG o, ULONG mask, ULONG v) { CW(k, o, (CR(k, o) & ~mask) | (v & mask)); }

static BOOLEAN CWait(SDW_LINK *k, ULONG o, ULONG mask, ULONG val, ULONG us)
{
    ULONG t;
    for (t = 0; t < us; t += 20) {
        if ((CR(k, o) & mask) == val) return TRUE;
        OsStallUs(20);
    }
    return (CR(k, o) & mask) == val;
}

/* DSP BAR (cAVS / ACE 1.x shim and ALH) */
static ULONG  SR32(SDW *w, ULONG o) { return DSP32(w->Sst, w->Chip->Shim + o); }
static void   SW32(SDW *w, ULONG o, ULONG v) { DSPW32(w->Sst, w->Chip->Shim + o, v); }
static USHORT SR16(SDW *w, ULONG o) { return MMIO16(w->Sst->Dsp, w->Chip->Shim + o); }
static void   SW16(SDW *w, ULONG o, USHORT v) { MMIO16(w->Sst->Dsp, w->Chip->Shim + o) = v; }

static BOOLEAN SWait(SDW *w, ULONG o, ULONG mask, ULONG val, ULONG us)
{
    ULONG t;
    for (t = 0; t < us; t += 20) {
        if ((SR32(w, o) & mask) == val) return TRUE;
        OsStallUs(20);
    }
    return (SR32(w, o) & mask) == val;
}

/* MCP_CONFIG_UPDATE: commits MCP_CONFIG / CONTROL / CMDCTRL / PHYCTRL changes */
static BOOLEAN ConfigUpdate(SDW_LINK *k)
{
    CW(k, MCP_CONFIG_UPDATE, 1);
    if (CWait(k, MCP_CONFIG_UPDATE, 1, 0, 2000)) return TRUE;
    LOGE("sdw: config update timed out");
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* commands (cadence _cdns_xfer_msg, polled)                            */
/* ------------------------------------------------------------------ */
#define XFER_OK       0
#define XFER_IGNORED  1             /* no ACK: no device with that number */
#define XFER_FAIL     2             /* NACK */
#define XFER_TIMEOUT  3

static int Xfer(SDW_LINK *k, ULONG dev, ULONG cmd, ULONG addr, UCHAR *buf, ULONG n, BOOLEAN ssp)
{
    ULONG i, avail = 0, t, resp[8];
    BOOLEAN nack = FALSE, noack = FALSE;
    if (!k->Up || !n || n > 8) return XFER_FAIL;
    if (k->MsgCount != n) { CW(k, MCP_FIFOLEVEL, n); k->MsgCount = n; }
    CW(k, MCP_INTSTAT, MCP_INT_RX_WL);
    for (i = 0; i < n; i++) {
        ULONG c = (dev << 24) | (cmd << 28) | (((addr + i) & 0xFFFF) << 8) | (ssp ? CMD_SSP_TAG : 0);
        if (cmd == CMD_WRITE) c |= buf[i];
        CW(k, k->Ip + IP_MCP_CMD_BASE + 4 * i, c);
    }
    k->Cmds += n;
    /* a command takes one frame (~21 us at 48 kHz); 20 ms covers a busy bus */
    for (t = 0; t < 20000; t += 10) {
        avail = CR(k, MCP_FIFOSTAT) & 0x3F;
        if (avail >= n) break;
        OsStallUs(10);
    }
    if (avail > 8) avail = 8;
    for (i = 0; i < avail; i++) resp[i] = CR(k, k->Ip + IP_MCP_CMD_BASE + 4 * i);
    CW(k, MCP_INTSTAT, MCP_INT_RX_WL);
    if (avail < n) {
        k->Errors++;
        LOGE("sdw: command timeout: dev %u cmd %u addr %x len %u (%u responses)", dev, cmd, addr, n, avail);
        return XFER_TIMEOUT;
    }
    for (i = 0; i < n; i++) {
        if (!(resp[i] & RESP_ACK)) noack = TRUE;
        if (resp[i] & RESP_NACK) nack = TRUE;
        if (cmd == CMD_READ) buf[i] = (UCHAR)(resp[i] >> 8);
    }
    if (nack) { k->Errors++; return XFER_FAIL; }
    return noack ? XFER_IGNORED : XFER_OK;
}

/* SCP_AddrPage1 / 2 for an address with paging (sdw_fill_msg); cached per device */
static BOOLEAN Page(SDW_LINK *k, SDW_DEV *d, ULONG addr, ULONG *a16)
{
    UCHAR pg[2];
    if (addr < SDW_REG_NO_PAGE || !d->Paging) { *a16 = addr & 0xFFFF; return addr <= 0xFFFF; }
    if (addr >= SDW_REG_MAX) return FALSE;
    pg[0] = (UCHAR)(addr >> SDW_SCP_ADDRPAGE1_SHIFT);
    pg[1] = (UCHAR)(addr >> SDW_SCP_ADDRPAGE2_SHIFT);
    if (!d->PageValid || d->Page1 != pg[0] || d->Page2 != pg[1]) {
        d->PageValid = FALSE;
        if (Xfer(k, d->DevNum, CMD_WRITE, SDW_SCP_ADDRPAGE1, pg, 2, FALSE) != XFER_OK) return FALSE;
        d->Page1 = pg[0]; d->Page2 = pg[1]; d->PageValid = TRUE;
    }
    *a16 = (addr & 0x7FFF) | 0x8000;
    return TRUE;
}

static BOOLEAN Access(SDW_DEV *d, ULONG addr, UCHAR *v, ULONG n, BOOLEAN wr)
{
    SDW_LINK *k = LinkOf(d);
    ULONG a;
    int r;
    if (!k || !d->Attached || !d->DevNum) return FALSE;
    if (!Page(k, d, addr, &a)) { d->Errors++; return FALSE; }
    r = Xfer(k, d->DevNum, wr ? CMD_WRITE : CMD_READ, a, v, n, FALSE);
    if (r == XFER_OK) return TRUE;
    if (d->Errors++ < 8)
        LOGV("sdw: %s %s %08x failed (%s)", d->Name, wr ? "write" : "read", addr, r == XFER_IGNORED ? "no ack" : "nack");
    d->PageValid = FALSE;
    return FALSE;
}

BOOLEAN SdwRead(SDW_DEV *d, ULONG addr, UCHAR *val) { return Access(d, addr, val, 1, FALSE); }
BOOLEAN SdwWrite(SDW_DEV *d, ULONG addr, UCHAR val) { return Access(d, addr, &val, 1, TRUE); }

ULONG SdwGet(SDW_DEV *d, ULONG addr)
{
    UCHAR v;
    return SdwRead(d, addr, &v) ? v : 0xFFFFFFFFUL;
}

BOOLEAN SdwUpdate(SDW_DEV *d, ULONG addr, UCHAR mask, UCHAR val)
{
    UCHAR v;
    if (!SdwRead(d, addr, &v)) return FALSE;
    if (((v & ~mask) | (val & mask)) == v) return TRUE;
    return SdwWrite(d, addr, (UCHAR)((v & ~mask) | (val & mask)));
}

/* regmap-sdw-mbq: the high bytes go through the MBQ alias first, the low byte last commits */
BOOLEAN SdwWriteMbq(SDW_DEV *d, ULONG addr, ULONG bytes, ULONG val)
{
    ULONG sh = 8 * bytes;
    if (!bytes || bytes > 4) return FALSE;
    while (--bytes > 0) {
        sh -= 8;
        if (!SdwWrite(d, SDW_SDCA_MBQ_CTL(addr), (UCHAR)(val >> sh))) return FALSE;
    }
    return SdwWrite(d, addr, (UCHAR)val);
}

BOOLEAN SdwReadMbq(SDW_DEV *d, ULONG addr, ULONG bytes, ULONG *val)
{
    UCHAR b;
    ULONG v, sh = 0;
    if (!bytes || bytes > 4 || !SdwRead(d, addr, &b)) return FALSE;
    v = b;
    while (--bytes > 0) {
        if (!SdwRead(d, SDW_SDCA_MBQ_CTL(addr), &b)) return FALSE;
        sh += 8;
        v |= (ULONG)b << sh;
    }
    *val = v;
    return TRUE;
}

BOOLEAN SdwSeq(SDW_DEV *d, const ULONG *rv, ULONG n)
{
    ULONG i;
    BOOLEAN ok = TRUE;
    for (i = 0; i < n; i++) ok &= SdwWrite(d, rv[2 * i], (UCHAR)rv[2 * i + 1]);
    return ok;
}

/* ------------------------------------------------------------------ */
/* data ports (stream.c sdw_program_slave_port_params etc.)             */
/* ------------------------------------------------------------------ */
BOOLEAN SdwDpnProgram(SDW_DEV *d, const SDW_DPN *p, const SDW_XPORT *x, ULONG b)
{
    ULONG n = p->Num, si = x->SampleInterval - 1;
    BOOLEAN ok;
    if (!n) return FALSE;
    ok = SdwUpdate(d, SDW_DPN_PORTCTRL(n), 0x0F, 0);                  /* normal data mode, isochronous */
    ok &= SdwWrite(d, SDW_DPN_BLOCKCTRL1(n), (UCHAR)(x->Bits - 1));
    ok &= SdwWrite(d, SDW_DPN_SAMPLECTRL1(n, b), (UCHAR)si);
    ok &= SdwWrite(d, SDW_DPN_OFFSETCTRL1(n, b), (UCHAR)x->Offset);
    if (p->Type != SDW_DP_SIMPLE) {
        ok &= SdwWrite(d, SDW_DPN_OFFSETCTRL2(n, b), (UCHAR)(x->Offset >> 8));
        ok &= SdwWrite(d, SDW_DPN_BLOCKCTRL3(n, b), 0);               /* block packing per port */
        if (p->Type == SDW_DP_FULL) {
            ok &= SdwWrite(d, SDW_DPN_SAMPLECTRL2(n, b), (UCHAR)(si >> 8));
            ok &= SdwWrite(d, SDW_DPN_HCTRL(n, b), (UCHAR)((x->HStart << 4) | (x->HStop & 0x0F)));
        }
    }
    return ok;
}

BOOLEAN SdwDpnEnable(SDW_DEV *d, const SDW_DPN *p, ULONG b, BOOLEAN on)
{
    return p->Num && SdwWrite(d, SDW_DPN_CHANNELEN(p->Num, b), on ? (UCHAR)((1U << p->Ch) - 1) : 0);
}

BOOLEAN SdwDpnPrepare(SDW_DEV *d, const SDW_DPN *p, BOOLEAN on)
{
    UCHAR mask = (UCHAR)((1U << p->Ch) - 1), v = 0xFF;
    ULONG t;
    if (!p->Num) return FALSE;
    if (!p->CpSm) return TRUE;                                          /* simplified channel prepare */
    if (!SdwWrite(d, SDW_DPN_PREPARECTRL(p->Num), on ? mask : 0)) return FALSE;
    for (t = 0; t < 20; t++) {                                          /* ch_prep_timeout 10 ms + slack */
        if (SdwRead(d, SDW_DPN_PREPARESTATUS(p->Num), &v) && !(v & mask)) return TRUE;
        OsSleepMs(1);
    }
    LOGE("sdw: %s DP%u channel prepare timed out (status %02x)", d->Name, p->Num, v);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* ACPI DisCo helpers                                                   */
/* ------------------------------------------------------------------ */
static ULONG PkgLen(const UCHAR *p, ULONG avail, ULONG *hdr)
{
    ULONG n = p[0] >> 6, len, i;
    if (!avail || avail < n + 1) return 0;
    if (!n) { *hdr = 1; return p[0] & 0x3F; }
    len = p[0] & 0x0F;
    for (i = 0; i < n; i++) len |= (ULONG)p[1 + i] << (4 + 8 * i);
    *hdr = n + 1;
    return len;
}

/* "prop" string object at or after 'from' inside [from, end) */
static BOOLEAN FindProp(const UCHAR *T, ULONG from, ULONG end, const char *prop, ULONG *after)
{
    ULONG n = strlen_s(prop), i;
    for (i = from; i + n + 2 <= end; i++)
        if (T[i] == 0x0D && T[i + 1] == (UCHAR)prop[0] && !memcmp(T + i + 1, prop, n) && T[i + 1 + n] == 0) {
            *after = i + n + 2;
            return TRUE;
        }
    return FALSE;
}

BOOLEAN SdwAcpiInt(const AML_DEV *scope, const char *prop, ULONGLONG *v)
{
    return AmlDsdInt(scope, prop, v);
}

ULONG SdwAcpiInts(const AML_DEV *scope, const char *prop, ULONG *v, ULONG max)
{
    AML_ELEM e[16];
    ULONG n = AmlDsdPkg(scope, prop, e, MIN(max, 16)), i, k = 0;
    for (i = 0; i < n; i++) if (e[i].Kind == AML_EL_INT) v[k++] = (ULONG)e[i].Int;
    return k;
}

/* Name (XXXX, Package (..)) inside [from, end): the package as a scope */
static BOOLEAN NamedPkg(const AML_DEV *d, ULONG from, ULONG end, const char *name, AML_DEV *sub)
{
    const UCHAR *T = d->T;
    ULONG i, hdr, plen;
    for (i = from; i + 7 < end; i++) {
        if (T[i] != 0x08 || memcmp(T + i + 1, name, 4) || T[i + 5] != 0x12) continue;
        plen = PkgLen(T + i + 6, end - i - 6, &hdr);
        if (!plen || i + 6 + plen > end) continue;
        *sub = *d;
        sub->Start = i;
        sub->Body = i + 6 + hdr;
        sub->End = i + 6 + plen;
        memcpy(sub->Name, name, 4);
        sub->Name[4] = 0;
        return TRUE;
    }
    return FALSE;
}

BOOLEAN SdwAcpiSub(const AML_DEV *scope, const char *prop, AML_DEV *sub)
{
    AML_ELEM e;
    AML_DEV dev, cur;
    char name[5];
    ULONG k;
    if (AmlDsdPkg(scope, prop, &e, 1) != 1 || (e.Kind != AML_EL_STR && e.Kind != AML_EL_REF)) return FALSE;
    for (k = 0; k < 4; k++) name[k] = e.Name[k] ? e.Name[k] : '_';
    name[4] = 0;
    if (NamedPkg(scope, scope->Body, scope->End, name, sub)) return TRUE;
    /* the package belongs to an enclosing device (the scope may be a package itself) */
    cur = *scope;
    for (k = 0; k < 4 && AmlDeviceAt(cur.T, cur.Len, cur.Start, &dev); k++) {
        if (NamedPkg(&dev, dev.Body, dev.End, name, sub)) return TRUE;
        cur = dev;
    }
    return FALSE;
}

ULONG SdwAcpiBuf(const AML_DEV *scope, const char *prop, UCHAR *buf, ULONG max)
{
    const UCHAR *T = scope->T;
    ULONG p, hdr, plen, n = 0, q;
    if (!FindProp(T, scope->Body, scope->End, prop, &p) || p >= scope->End || T[p] != 0x11) return 0;
    plen = PkgLen(T + p + 1, scope->End - p - 1, &hdr);
    if (!plen || p + 1 + plen > scope->End) return 0;
    q = p + 1 + hdr;
    switch (T[q]) {                                           /* buffer size */
    case 0x00: n = 0; q += 1; break;
    case 0x01: n = 1; q += 1; break;
    case 0x0A: n = T[q + 1]; q += 2; break;
    case 0x0B: n = T[q + 1] | (T[q + 2] << 8); q += 3; break;
    case 0x0C: n = G32U(T + q + 1); q += 5; break;
    default: return 0;
    }
    n = MIN(n, p + 1 + plen - q);
    n = MIN(n, max);
    memcpy(buf, T + q, n);
    return n;
}

BOOLEAN SdwAcpiFunction(const SDW_DEV *d, ULONG fn, AML_DEV *out)
{
    return AmlChildByAdr(&d->Acpi, fn, out);
}

/* ------------------------------------------------------------------ */
/* ACPI discovery (hda.c hda_sdw_acpi_scan, intel-sdw-acpi.c)           */
/* ------------------------------------------------------------------ */
typedef struct { SDW *W; ULONG Hda; BOOLEAN Found, Strong; } CSCAN;

static BOOLEAN IsDevice(const AML_DEV *d) { return d->T[d->Start] == 0x5B; }

/* the SoundWire controller: _ADR 0x40000000 (link type 4 in bits 31:28) under the HD Audio device */
static BOOLEAN ScanCtrl(const AML_DEV *d, void *ctx)
{
    CSCAN *c = (CSCAN *)ctx;
    ULONGLONG a, pa;
    AML_DEV p;
    BOOLEAN strong;
    if (!AmlAdr(d, &a) || a != 0x40000000ULL) return TRUE;
    strong = AmlParent(d, &p) && IsDevice(&p) && AmlAdr(&p, &pa) && pa == c->Hda;
    if (!c->Found || (strong && !c->Strong)) { c->W->Ctrl = *d; c->Found = TRUE; c->Strong = strong; }
    return !strong;
}

static BOOLEAN ChildOfCtrl(SDW *w, const AML_DEV *d)
{
    AML_DEV p;
    if (!AmlParent(d, &p)) return FALSE;
    if (p.T == w->Ctrl.T && p.Start == w->Ctrl.Start) return TRUE;
    /* Scope (\_SB.PC00.HDAS.SNDW) { Device (..) } in an SSDT */
    return !IsDevice(&p) && !memcmp(p.Name, w->Ctrl.Name, 4);
}

static const SDW_DRV *FindDrv(SDW_DEV *d)
{
    ULONG k;
    for (k = 0; g_SdwDrvs[k]; k++) {
        const SDW_DRV *drv = g_SdwDrvs[k];
        const USHORT *p;
        BOOLEAN part = drv->Parts == NULL;
        if (drv->Mfg && drv->Mfg != d->Mfg) continue;
        for (p = drv->Parts; p && *p && !part; p++) part = *p == d->Part;
        if (!part) continue;
        d->Drv = drv;
        d->Kind = drv->Kind;
        if (!drv->Probe || drv->Probe(d)) return drv;
        d->Drv = NULL;
    }
    return NULL;
}

static BOOLEAN ScanPeriph(const AML_DEV *a, void *ctx)
{
    SDW *w = (SDW *)ctx;
    ULONGLONG adr;
    SDW_DEV *d;
    ULONG i, link;
    if (!AmlAdr(a, &adr) || !(adr >> 32) || !ChildOfCtrl(w, a)) return TRUE;
    link = (ULONG)(adr >> 48) & 0xF;
    for (i = 0; i < w->ND; i++) if (w->D[i].Adr == adr) return TRUE;   /* DSDT and SSDT */
    if (w->ND >= SDW_DEVS) return FALSE;
    if (link >= SDW_LINKS || !(w->Mask & (1UL << link))) {
        LOGI("sdw: peripheral %s on link %u (disabled)", a->Path, link);
        return TRUE;
    }
    d = &w->D[w->ND];
    memset(d, 0, sizeof(*d));
    d->Sst = w->Sst;
    d->Adr = adr;
    d->Acpi = *a;
    d->Link = (UCHAR)link;
    d->Version = (UCHAR)((adr >> 44) & 0xF);
    d->Unique = (UCHAR)((adr >> 40) & 0xF);
    d->Mfg = (USHORT)(adr >> 24);
    d->Part = (USHORT)(adr >> 8);
    d->Class = (UCHAR)adr;
    d->Bits = SDW_BITS;
    d->Rate = SDW_RATE;
    OsSprintf(d->Name, sizeof(d->Name), "%04x:%04x@%u", d->Mfg, d->Part, link);
    {
        ULONGLONG v;
        if (SdwAcpiInt(a, "mipi-sdw-paging-supported", &v)) d->Paging = v != 0;
    }
    if (!FindDrv(d)) {
        LOGI("sdw: peripheral %s (%s, class %u): no driver", d->Name, a->Path, d->Class);
        return TRUE;
    }
    OsSprintf(d->Name, sizeof(d->Name), "%s@%u", d->Drv->Name, link);
    if (d->Unique) OsSprintf(d->Name + strlen_s(d->Name), sizeof(d->Name) - strlen_s(d->Name), ".%u", d->Unique);
    LOGI("sdw: %s (%s) link %u uid %u, %s, DP %u / %u", d->Name, a->Path, link, d->Unique,
         d->Kind == SDW_KIND_HEADSET ? "headset codec" : d->Kind == SDW_KIND_AMP ? "amplifier" : "microphones",
         d->Play.Num, d->Rec.Num);
    w->ND++;
    return TRUE;
}

static void LinkProps(SDW *w, ULONG l)
{
    SDW_LINK *k = &w->L[l];
    AML_DEV sub;
    ULONGLONG v;
    ULONG f[8], n, i;
    char name[40];
    BOOLEAN have;
    k->IpClock = w->Chip->IpClock;
    k->FrameRate = SDW_RATE;
    k->Row = 50;
    k->Doaise = 1; k->Doais = 3; k->Dods = 1;                         /* intel_auxdevice.c hardware defaults */
    OsSprintf(name, sizeof(name), "mipi-sdw-link-%u-subproperties", l);
    have = SdwAcpiSub(&w->Ctrl, name, &sub);
    if (have) {
        if (SdwAcpiInt(&sub, "intel-sdw-ip-clock", &v) && v) k->IpClock = (ULONG)v;
        if (SdwAcpiInt(&sub, "mipi-sdw-max-clock-frequency", &v)) k->BusClock = (ULONG)v;
        n = SdwAcpiInts(&sub, "mipi-sdw-clock-frequencies-supported", f, 8);
        for (i = 0; i < n && !k->BusClock; i++) if (f[i] > k->BusClock) k->BusClock = f[i];
        if (SdwAcpiInt(&sub, "mipi-sdw-default-frame-rate", &v) && v) k->FrameRate = (ULONG)v;
        if (SdwAcpiInt(&sub, "mipi-sdw-default-frame-row-size", &v) && v) k->Row = (ULONG)v;
        if (SdwAcpiInt(&sub, "intel-sdw-clde", &v)) k->Clde = (USHORT)v;
        if (SdwAcpiInt(&sub, "intel-sdw-doaise2", &v)) k->Doaise2 = (USHORT)v;
        if (SdwAcpiInt(&sub, "intel-sdw-dodse2", &v)) k->Dodse2 = (USHORT)v;
        if (SdwAcpiInt(&sub, "intel-sdw-clds", &v)) k->Clds = (USHORT)v;
        if (SdwAcpiInt(&sub, "intel-sdw-clss", &v)) k->Clss = (USHORT)v;
        if (SdwAcpiInt(&sub, "intel-sdw-doaise", &v)) k->Doaise = (USHORT)v;
        if (SdwAcpiInt(&sub, "intel-sdw-doais", &v)) k->Doais = (USHORT)v;
        if (SdwAcpiInt(&sub, "intel-sdw-dodse", &v)) k->Dodse = (USHORT)v;
        if (SdwAcpiInt(&sub, "intel-sdw-dods", &v)) k->Dods = (USHORT)v;
        if (SdwAcpiInt(&sub, "intel-quirk-mask", &v) && (v & 2)) {      /* SDW_INTEL_QUIRK_MASK_BUS_DISABLE */
            LOGI("sdw: link %u disabled by the BIOS (intel-quirk-mask)", l);
            return;
        }
    } else return;                                                      /* is_link_enabled(): no node, no link */
    k->Mclk = k->IpClock / 2;                                           /* the BIOS reports twice the IP clock */
    if (!k->BusClock) k->BusClock = !(k->Mclk % 4800000) ? 4800000 : !(k->Mclk % 6000000) ? 6000000 : k->Mclk / 4;
    k->Acpi = TRUE;
}

/* frame shape for the bus clock: the ACPI rows if the columns come out valid, else the first shape that fits */
static BOOLEAN FrameShape(SDW_LINK *k)
{
    ULONG bits, r, c;
    k->DrFreq = 2 * k->BusClock;
    if (!k->Mclk || !k->DrFreq || (2 * k->Mclk) % k->DrFreq || k->DrFreq % k->FrameRate) return FALSE;
    bits = k->DrFreq / k->FrameRate;
    for (c = 0; c < 8; c++) if (k->Row * g_Cols[c] == bits) { k->Col = g_Cols[c]; return TRUE; }
    for (r = 0; r < 24; r++)
        for (c = 0; c < 8; c++)
            if (g_Rows[r] && g_Rows[r] * g_Cols[c] == bits) { k->Row = g_Rows[r]; k->Col = g_Cols[c]; return TRUE; }
    return FALSE;
}

static ULONG RowIdx(ULONG row) { ULONG i; for (i = 0; i < 24; i++) if (g_Rows[i] == row) return i; return 0; }
static ULONG ColIdx(ULONG col) { ULONG i; for (i = 0; i < 8; i++) if (g_Cols[i] == col) return i; return 0; }

/* ------------------------------------------------------------------ */
/* link power (intel.c intel_link_power_up, intel_ace2x.c)              */
/* ------------------------------------------------------------------ */
/* SYNCPRD and the clock source for the IP clock (the 4 kHz gsync counter) */
static void SyncPrd(ULONG mclk, BOOLEAN mlcs, ULONG *prd, ULONG *src)
{
    if (mclk % 6000000) {
        if (mclk % 2400000) { *prd = 24576 / 4 - 1; *src = MLCS_CARDINAL; }
        else { *prd = 38400 / 4 - 1; *src = MLCS_XTAL; }
    } else if (mlcs) { *prd = 96000 / 4 - 1; *src = MLCS_AUDIO_PLL; }
    else { *prd = 24000 / 4 - 1; *src = MLCS_XTAL; }
}

static void ShimInit(SDW *w, ULONG l)
{
    USHORT io = IOCTL_BKE, act;
    SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
    io |= IOCTL_WPDD; SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
    io |= IOCTL_DO; SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
    io |= IOCTL_DOE; SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
    /* glue logic -> master IP */
    io = SR16(w, SHIM_IOCTL(l));
    io &= ~IOCTL_DOE; SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
    io &= ~IOCTL_DO; SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
    io |= IOCTL_MIF; SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
    io &= ~(IOCTL_BKE | IOCTL_COE); SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
    act = SR16(w, SHIM_CTMCTL(l));
    act = (USHORT)((act & ~(3U << CTMCTL_DOAIS_SHIFT)) | (1U << CTMCTL_DOAIS_SHIFT) | CTMCTL_DACTQE | CTMCTL_DODS);
    SW16(w, SHIM_CTMCTL(l), act);
    OsStallUs(15);
}

static BOOLEAN ShimPowerUp(SDW *w)
{
    ULONG lcap = SR32(w, SHIM_LCAP), hw = lcap & 7, mask, prd, src, v, l;
    BOOLEAN mlcs = (lcap & LCAP_MLCS) != 0;
    SDW_LINK *k0 = NULL;
    if (lcap == 0xFFFFFFFFUL) { LOGE("sdw: shim not accessible"); return FALSE; }
    if (hw < w->Count) LOGI("sdw: ACPI has %u links, the hardware %u", w->Count, hw);
    w->Mask &= (1UL << hw) - 1;
    mask = w->Mask;
    for (l = 0; l < SDW_LINKS; l++) if (mask & (1UL << l)) { k0 = &w->L[l]; break; }
    if (!k0) return FALSE;
    SyncPrd(k0->Mclk, mlcs, &prd, &src);
    v = (SR32(w, SHIM_SYNC) & ~SYNC_SYNCPRD_MASK) | prd | SYNC_SYNCCPU;
    SW32(w, SHIM_SYNC, v);
    SW32(w, SHIM_LCTL, SR32(w, SHIM_LCTL) | LCTL_SPA(mask));
    if (!SWait(w, SHIM_LCTL, LCTL_CPA(mask), LCTL_CPA(mask), 5000)) {
        LOGE("sdw: link power up failed (LCTL %08x)", SR32(w, SHIM_LCTL));
        return FALSE;
    }
    if (!SWait(w, SHIM_SYNC, SYNC_SYNCCPU, 0, 5000)) LOGE("sdw: SYNCCPU did not clear (SYNC %08x)", SR32(w, SHIM_SYNC));
    if (mlcs) SW32(w, SHIM_LCTL, (SR32(w, SHIM_LCTL) & ~(7UL << LCTL_MLCS_SHIFT)) | (src << LCTL_MLCS_SHIFT));
    for (l = 0; l < SDW_LINKS; l++) {
        SDW_LINK *k = &w->L[l];
        if (!(mask & (1UL << l))) continue;
        k->Regs = w->Sst->Dsp + w->Chip->Link + w->Chip->LinkSize * l;
        k->Ip = 0;
        k->Powered = TRUE;
        ShimInit(w, l);
        k->Pdis = (SR16(w, SHIM_PCMSCAP(l)) >> 8) & 0x1F;              /* bidirectional PCM streams */
    }
    LOGI("sdw: links %x powered (LCAP %08x, SYNCPRD %u%s)", mask, lcap, prd, mlcs ? ", MLCS" : "");
    return TRUE;
}

static void ShimPowerDown(SDW *w, ULONG which)
{
    ULONG l, mask = 0;
    for (l = 0; l < SDW_LINKS; l++) {
        USHORT io;
        if (!w->L[l].Powered || !(which & (1UL << l))) continue;
        mask |= 1UL << l;
        io = (USHORT)(SR16(w, SHIM_IOCTL(l)) | IOCTL_BKE | IOCTL_COE);   /* master IP -> glue logic */
        SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
        io &= ~IOCTL_MIF; SW16(w, SHIM_IOCTL(l), io); OsStallUs(15);
        w->L[l].Powered = w->L[l].Up = FALSE;
    }
    if (!mask) return;
    SW32(w, SHIM_LCTL, SR32(w, SHIM_LCTL) & ~LCTL_SPA(mask));
    if (!SWait(w, SHIM_LCTL, LCTL_CPA(mask), 0, 5000)) LOGE("sdw: link power down failed (LCTL %08x)", SR32(w, SHIM_LCTL));
}

static BOOLEAN ExtPowerUp(SDW *w)
{
    SST *s = w->Sst;
    ULONG lcap, n, base, prd = 0, src, l, t;
    BOOLEAN first = TRUE;
    CARD *c = s->Card;
    w->MlOff = SstMlLink(s, ELID_SDW);
    if (!w->MlOff) { LOGE("sdw: no SoundWire extended link in the multi-link capability"); return FALSE; }
    lcap = HDA32(s, w->MlOff + ML_LCAP);
    n = ((lcap >> 20) & 7) + 1;
    base = HDA32(s, w->MlOff + ML_LEPTR) & 0xFFFFF;
    if (n < w->Count) LOGI("sdw: ACPI has %u links, the hardware %u", w->Count, n);
    w->Mask &= (1UL << n) - 1;
    if (!w->Mask) return FALSE;
    if (c->Bar[1] && (c->Bar[0] & 4)) { LOGE("sdw: HD Audio BAR above 4 GB"); return FALSE; }
    w->WinPa = (c->Bar[0] & ~0xFUL) + base;
    w->WinSize = w->Chip->LinkSize * n;
    w->Win = (PUCHAR)OsMapMmio(w->WinPa, w->WinSize);
    if (!w->Win) { LOGE("sdw: cannot map the SoundWire registers"); return FALSE; }
    for (l = 0; l < SDW_LINKS; l++) {
        SDW_LINK *k = &w->L[l];
        if (!(w->Mask & (1UL << l))) continue;
        k->Shim2 = w->Win + w->Chip->LinkSize * l;
        k->Regs = k->Shim2 + SHIM2_IP;
        k->Ip = CDNS_IP_OFFSET_ACE2;
        k->Vs = k->Shim2 + SHIM2_VS;
        SyncPrd(k->Mclk, TRUE, &prd, &src);
        if (!SstMlPower(s, ELID_SDW, l, TRUE)) continue;
        k->Powered = TRUE;
        MMIO32(k->Vs, VS_LVSCTL) = (MMIO32(k->Vs, VS_LVSCTL) & ~(7UL << 27)) | (src << 27);
        if (first) {
            ULONG v = (HDA32(s, w->MlOff + ML_LSYNC) & ~LSYNC_SYNCPRD_MASK) | prd | LSYNC_SYNCPU;
            HDAW32(s, w->MlOff + ML_LSYNC, v);
            for (t = 0; t < 100 && (HDA32(s, w->MlOff + ML_LSYNC) & LSYNC_SYNCPU); t++) OsStallUs(10);
            if (HDA32(s, w->MlOff + ML_LSYNC) & LSYNC_SYNCPU) LOGE("sdw: SYNCPU did not clear");
            first = FALSE;
        }
        {
            USHORT act = MMIO16(k->Vs, VS_ACTMCTL);
            act = (USHORT)((act & ~0xFFFEU) | ((k->Clde & 1) << 15) | ((k->Doaise2 & 1) << 14) | ((k->Dodse2 & 3) << 12) |
                           ((k->Clds & 0x1F) << 7) | ((k->Clss & 1) << 6) | ((k->Doaise & 1) << 5) |
                           ((k->Doais & 3) << 3) | ((k->Dodse & 1) << 2) | ((k->Dods & 1) << 1) | 1);   /* DACTQE */
            MMIO16(k->Vs, VS_ACTMCTL) = act;
            OsStallUs(15);
        }
        k->Pdis = (MMIO16(k->Shim2, SHIM2_PCMSCAP) >> 8) & 0x1F;
    }
    LOGI("sdw: ACE2 extended link at %x, %u sublinks at +%x, SYNCPRD %u", w->MlOff, n, base, prd);
    return TRUE;
}

static void ExtPowerDown(SDW *w, ULONG which)
{
    ULONG l;
    for (l = 0; l < SDW_LINKS; l++) {
        if (!w->L[l].Powered || !(which & (1UL << l))) continue;
        SstMlPower(w->Sst, ELID_SDW, l, FALSE);
        w->L[l].Powered = w->L[l].Up = FALSE;
    }
}

static void PowerDown(SDW *w, ULONG which)
{
    if (w->Chip->Ext) ExtPowerDown(w, which);
    else if (w->Sst->Dsp) ShimPowerDown(w, which);
}

/* ------------------------------------------------------------------ */
/* bus start (intel_start_bus, sdw_cdns_init)                           */
/* ------------------------------------------------------------------ */
static BOOLEAN CdnsStart(SDW_LINK *k, ULONG l)
{
    ULONG div, v, t;
    if (!FrameShape(k)) {
        LOGE("sdw: link %u: no frame shape for IP clock %u, bus clock %u", l, k->Mclk, k->BusClock);
        return FALSE;
    }
    /* soft reset */
    CU(k, MCP_CONTROL, MCP_CONTROL_SOFT_RST, MCP_CONTROL_SOFT_RST);
    ConfigUpdate(k);
    if (!CWait(k, MCP_CONTROL, MCP_CONTROL_SOFT_RST, 0, 2000)) { LOGE("sdw: link %u: soft reset timed out", l); return FALSE; }
    /* clock divider (1 / (N + 1) of the double-rate IP clock), frame shape, SSP interval */
    div = 2 * k->Mclk / k->DrFreq - 1;
    CU(k, MCP_CLK_CTRL0, 0xFF, div);
    CU(k, MCP_CLK_CTRL1, 0xFF, div);
    CW(k, MCP_FRAME_SHAPE_INIT, (RowIdx(k->Row) << 3) | ColIdx(k->Col));
    CW(k, MCP_SSP_CTRL0, k->FrameRate / GSYNC_HZ);
    CW(k, MCP_SSP_CTRL1, k->FrameRate / GSYNC_HZ);
    k->MsgCount = CR(k, MCP_FIFOLEVEL);
    CU(k, MCP_CONTROL, MCP_CONTROL_CMD_RST, MCP_CONTROL_CMD_RST);        /* flush the command FIFOs */
    CU(k, k->Ip + IP_MCP_CONTROL, IP_CONTROL_CMD_ACCEPT | IP_CONTROL_BLOCK_WAKEUP, IP_CONTROL_CMD_ACCEPT);
    CU(k, MCP_CONFIG, MCP_CONFIG_BUS_REL, 0);
    v = CR(k, k->Ip + IP_MCP_CONFIG);
    v &= ~(IP_CONFIG_OP_MASK | IP_CONFIG_CMD | IP_CONFIG_SNIFFER | IP_CONFIG_MMASTER);   /* normal, single master */
    CW(k, k->Ip + IP_MCP_CONFIG, v);
    if (!ConfigUpdate(k)) return FALSE;
    /* polled: all interrupts masked, slave status cleared */
    CW(k, MCP_SLAVE_INTSTAT0, CR(k, MCP_SLAVE_INTSTAT0));
    CW(k, MCP_SLAVE_INTSTAT1, CR(k, MCP_SLAVE_INTSTAT1));
    CW(k, MCP_SLAVE_INTMASK0, 0);
    CW(k, MCP_SLAVE_INTMASK1, 0);
    CW(k, MCP_INTMASK, 0);
    /* exit reset: hardware-generated bus reset, then frames */
    CU(k, MCP_CONTROL, MCP_CONTROL_HW_RST, MCP_CONTROL_HW_RST);
    if (!ConfigUpdate(k)) return FALSE;
    for (t = 0; t < 10 && (CR(k, MCP_CONTROL) & MCP_CONTROL_HW_RST); t++) OsSleepMs(1);
    if (CR(k, MCP_CONTROL) & (MCP_CONTROL_HW_RST | MCP_CONTROL_CMD_RST))
        LOGE("sdw: link %u: reset bits not cleared (MCP_CONTROL %08x)", l, CR(k, MCP_CONTROL));
    k->Bank = (CR(k, MCP_STAT) & MCP_STAT_ACTIVE_BANK) ? 1 : 0;
    k->Assigned = 0;
    k->Up = TRUE;
    LOGI("sdw: link %u up: IP clock %u, bus %u Hz, frame %u x %u, divider %u", l, k->Mclk, k->BusClock, k->Row,
         k->Col, div);
    return TRUE;
}

static ULONG SlaveStat(SDW_LINK *k, ULONG num) { return (CR(k, MCP_SLAVE_STAT) >> (2 * num)) & 3; }

/* ------------------------------------------------------------------ */
/* enumeration (bus.c sdw_program_device_num)                           */
/* ------------------------------------------------------------------ */
static void Enumerate(SDW *w, ULONG l)
{
    SDW_LINK *k = &w->L[l];
    ULONG t, guard, i;
    for (t = 0; t < 300 && !SlaveStat(k, 0); t++) OsSleepMs(1);       /* peripherals sync and report */
    for (guard = 0; guard < 2 * SDW_MAX_DEVNUM; guard++) {
        UCHAR id[6], num;
        USHORT mfg, part;
        UCHAR uid, ver, cls;
        SDW_DEV *d = NULL;
        int r = Xfer(k, 0, CMD_READ, SDW_SCP_DEVID_0, id, 6, FALSE);
        if (r == XFER_IGNORED) break;                                   /* nobody left on device 0 */
        if (r != XFER_OK) { OsSleepMs(2); if (Xfer(k, 0, CMD_READ, SDW_SCP_DEVID_0, id, 6, FALSE) != XFER_OK) break; }
        ver = id[0] >> 4; uid = id[0] & 0xF;
        mfg = (USHORT)((id[1] << 8) | id[2]);
        part = (USHORT)((id[3] << 8) | id[4]);
        cls = id[5];
        for (i = 0; i < w->ND && !d; i++) {
            SDW_DEV *c = &w->D[i];
            if (c->Link == l && !c->DevNum && c->Mfg == mfg && c->Part == part && c->Unique == uid) d = c;
        }
        for (num = 1; num <= SDW_MAX_DEVNUM && (k->Assigned & (1UL << num)); num++) ;
        if (num > SDW_MAX_DEVNUM) { LOGE("sdw: link %u: no device number left", l); break; }
        if (Xfer(k, 0, CMD_WRITE, SDW_SCP_DEVNUMBER, &num, 1, FALSE) != XFER_OK) {
            LOGE("sdw: link %u: device number %u not taken by %04x:%04x", l, num, mfg, part);
            break;
        }
        k->Assigned |= 1UL << num;
        for (t = 0; t < 50 && SlaveStat(k, num) != 1; t++) OsSleepMs(1);
        LOGI("sdw: link %u: device %u: SoundWire 1.%u, mfg %04x part %04x class %02x uid %u%s%s", l, num,
             ver ? ver - 1 : 0, mfg, part, cls, uid,
             d ? "" : " (not in ACPI)", SlaveStat(k, num) == 1 ? "" : ", not attached");
        if (!d) { k->Unknown++; continue; }
        d->DevNum = num;
        d->Version = ver;
        d->Class = cls;
        d->Attached = SlaveStat(k, num) == 1;
        d->PageValid = FALSE;
    }
}

/* SCP_BUS_CLOCK_BASE / BUSCLOCK_SCALE for SDCA peripherals (sdw_slave_get_scale_index) */
static BOOLEAN SetFrequency(SDW_DEV *d, SDW_LINK *k)
{
    ULONG mclk = k->Mclk, cur = k->BusClock, scale, idx;
    UCHAR base;
    if (!(19200000 % mclk)) { mclk = 19200000; base = 1; }
    else if (!(22579200 % mclk)) { mclk = 22579200; base = 4; }
    else if (!(24576000 % mclk)) { mclk = 24576000; base = 3; }
    else if (!(32000000 % mclk)) { mclk = 32000000; base = 5; }
    else if (!(96000000 % mclk)) { mclk = 24000000; base = 2; }
    else { LOGE("sdw: %s: no clock base for %u Hz", d->Name, k->Mclk); return FALSE; }
    if (!cur || mclk % cur) return FALSE;
    scale = mclk / cur;
    for (idx = 0; idx < 7 && (1UL << idx) != scale; idx++) ;
    if (idx == 7) { LOGE("sdw: %s: no clock scale for %u / %u", d->Name, mclk, cur); return FALSE; }
    return SdwWrite(d, SDW_SCP_BUS_CLOCK_BASE, base) && SdwWrite(d, SDW_SCP_BUSCLOCK_SCALE_B0, (UCHAR)(idx + 1)) &&
           SdwWrite(d, SDW_SCP_BUSCLOCK_SCALE_B1, (UCHAR)(idx + 1));
}

static BOOLEAN DevInit(SDW *w, SDW_DEV *d)
{
    SDW_LINK *k = &w->L[d->Link];
    UCHAR v;
    d->Caps = (d->Play.Num ? SDW_CAP_PLAY : 0) | (d->Rec.Num ? SDW_CAP_REC : 0);
    if (d->Class) SetFrequency(d, k);
    /* bus clash / parity flagged before the masks are set (SDW_MASTER_QUIRKS_CLEAR_INITIAL_*) */
    if (SdwRead(d, SDW_SCP_INT1, &v) && (v & (SDW_SCP_INT1_BUS_CLASH | SDW_SCP_INT1_PARITY)))
        SdwWrite(d, SDW_SCP_INT1, (UCHAR)(v & (SDW_SCP_INT1_BUS_CLASH | SDW_SCP_INT1_PARITY)));
    if (d->Drv->Init && !d->Drv->Init(d)) {
        LOGE("sdw: %s did not initialise (%u errors)", d->Name, d->Errors);
        return FALSE;
    }
    d->Ready = TRUE;
    LOGI("sdw: %s ready%s%s", d->Name, (d->Caps & SDW_CAP_PLAY) ? ", playback" : "", (d->Caps & SDW_CAP_REC) ? ", capture" : "");
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* streams                                                              */
/* ------------------------------------------------------------------ */
/* the ALH stream id of a PDI (intel_pdi_alh_configure: the bulk PDIs 0 / 1 and the PCM ones are not contiguous) */
static ULONG AlhId(ULONG link, ULONG pdi) { return link * 16 + pdi + 3 + (pdi >= 2 ? 2 : 0); }

static SDW_DPN *PortOf(SDW_DEV *d, ULONG dir) { return dir ? &d->Rec : &d->Play; }

/* transport parameters of the link's streams: generic_bandwidth_allocation.c for one 48 kHz group on lane 0.
   All streams of a link share the horizontal window at the right edge of the frame (hstop = last column,
   hwidth = columns needed for the summed payload); each stream gets the next block offset, starting at 1. */
static BOOLEAN Transport(SDW *w, ULONG l)
{
    SDW_LINK *k = &w->L[l];
    ULONG full = k->DrFreq / SDW_RATE, payload = 0, hw, i, bo = 1;
    for (i = 0; i < w->NS; i++) if (w->S[i].Up && w->S[i].Link == l) payload += w->S[i].Ch * SDW_BITS;
    if (!payload) return TRUE;
    hw = (k->Col * payload + full - 1) / full;
    if (hw > k->Col - 1) {
        LOGE("sdw: link %u: %u bits per frame do not fit a %u x %u frame", l, payload, k->Row, k->Col);
        return FALSE;
    }
    for (i = 0; i < w->NS; i++) {
        SDW_STRM *st = &w->S[i];
        if (!st->Up || st->Link != l) continue;
        st->X.SampleInterval = (USHORT)full;
        st->X.Offset = (USHORT)bo;
        st->X.HStop = (UCHAR)(k->Col - 1);
        st->X.HStart = (UCHAR)(k->Col - hw);
        st->X.Bits = SDW_BITS;
        st->X.Lane = 0;
        bo += st->Ch * SDW_BITS;
    }
    return TRUE;
}

/* master side: PCMSyCHM / ALH (cAVS, ACE 1.x), Cadence port direction and PDI -> port mapping */
static void PdiSetup(SDW *w, SDW_STRM *st)
{
    SDW_LINK *k = &w->L[st->Link];
    ULONG alh = AlhId(st->Link, st->Pdi), cap;
    if (!w->Chip->Ext) {
        ULONG v;
        cap = st->Pdi == 2 ? 8 : (SR16(w, SHIM_PCMSYCHC(st->Link, st->Pdi)) & 0xF) + 1;   /* PDI 2 reports 1 (Linux) */
        SW16(w, SHIM_PCMSYCHM(st->Link, st->Pdi),
             (USHORT)((st->Dir ? 0x8000 : 0) | (alh << 8) | ((st->Ch - 1) << 4)));
        v = DSP32(w->Sst, w->Chip->Alh + ALH_STRMZCFG(alh));
        v = (v & ~0xFFUL & ~(0xFUL << 16)) | 3 | ((ULONG)(st->Ch - 1) << 16);        /* DMAT 3, channels */
        DSPW32(w->Sst, w->Chip->Alh + ALH_STRMZCFG(alh), v);
    } else {
        cap = (MMIO16(k->Shim2, SHIM2_PCMSYCHC(st->Pdi)) & 0xF) + 1;
        MMIO16(k->Shim2, SHIM2_PCMSYCM(st->Pdi)) = (USHORT)(st->Dir ? 0x8000 : 0);   /* tag set at run time */
    }
    if (st->Ch > cap) LOGI("sdw: link %u PDI %u: %u channels, the PDI reports %u", st->Link, st->Pdi, st->Ch, cap);
    CU(k, DPN_PORTCTRL(st->Pdi), PORTCTRL_DIRN | PORTCTRL_TEST_FAILED | PORTCTRL_BULK_ENABLE, st->Dir ? PORTCTRL_DIRN : 0);
    CW(k, PDI_CONFIG(st->Pdi), st->Pdi | PDI_CONFIG_SOFT_RESET | (((1UL << st->Ch) - 1) << 8));
}

static void MasterPort(SDW_LINK *k, const SDW_STRM *st, ULONG b, BOOLEAN en)
{
    ULONG n = st->Pdi, cfg = CR(k, DPN_CONFIG(n, b));
    /* BGC 17:16 = 1 block group, BPM 18 = per port, WL 12:8, data mode 3:2 normal, flow 1:0 isochronous */
    cfg = (cfg & ~((3UL << 16) | (1UL << 18) | (0x1FUL << 8) | 0xFUL)) | ((ULONG)(st->X.Bits - 1) << 8);
    CW(k, DPN_CONFIG(n, b), cfg);
    CW(k, DPN_OFFSET_CTRL(n, b), (st->X.Offset & 0xFF) | (((ULONG)st->X.Offset >> 8) << 8));
    CW(k, DPN_HCTRL(n, b), ((ULONG)st->X.HStart << 4) | st->X.HStop | ((ULONG)st->X.Lane << 8));
    CW(k, DPN_SAMPLE_CTRL(n, b), st->X.SampleInterval - 1UL);
    CW(k, DPN_CH_EN(n, b), en ? (1UL << st->Ch) - 1 : 0);
}

/* both sides of every stream of the link into bank b; peripherals of a playback stream that take all its
   channels read the same bits (mirror mode, sdw_compute_slave_ports), others get consecutive offsets */
static BOOLEAN ProgramBank(SDW *w, ULONG l, ULONG b, BOOLEAN en)
{
    SDW_LINK *k = &w->L[l];
    ULONG i, j;
    BOOLEAN ok = TRUE;
    for (i = 0; i < w->NS; i++) {
        SDW_STRM *st = &w->S[i];
        ULONG bo, total = 0;
        if (!st->Up || st->Link != l) continue;
        MasterPort(k, st, b, en);
        bo = st->X.Offset;
        for (j = 0; j < st->NDev; j++) {
            SDW_DEV *d = &w->D[st->Dev[j]];
            SDW_DPN *p = PortOf(d, st->Dir);
            SDW_XPORT x = st->X;
            if (!d->Ready) continue;
            x.Offset = (USHORT)bo;
            ok &= SdwDpnProgram(d, p, &x, b);
            ok &= SdwDpnEnable(d, p, b, en);
            bo += p->Ch * SDW_BITS;
            total += p->Ch;
            if (!st->Dir && total == st->Ch) { bo = st->X.Offset; total = 0; }
        }
    }
    return ok;
}

/* broadcast SCP_FrameCtrl of the other bank with the SSP tag (sdw_bank_switch) */
static BOOLEAN BankSwitch(SDW_LINK *k, ULONG l)
{
    UCHAR v = (UCHAR)(ColIdx(k->Col) | (RowIdx(k->Row) << 3)), next = (UCHAR)!k->Bank;
    int r = Xfer(k, SDW_BROADCAST, CMD_WRITE, next ? SDW_SCP_FRAMECTRL_B1 : SDW_SCP_FRAMECTRL_B0, &v, 1, TRUE);
    if (r != XFER_OK && r != XFER_IGNORED) { LOGE("sdw: link %u: bank switch failed", l); return FALSE; }
    k->Bank = next;
    if (!CWait(k, MCP_STAT, MCP_STAT_ACTIVE_BANK, next ? MCP_STAT_ACTIVE_BANK : 0, 5000))
        LOGE("sdw: link %u: master still on bank %u (MCP_STAT %08x)", l, !next, CR(k, MCP_STAT));
    return TRUE;
}

/* the static streams of a link: prepare (params into the next bank, bank switch, channel prepare),
   then enable (params and channel enables into the other bank, bank switch) - sdw_prepare_stream and
   sdw_enable_stream for all streams at once */
static void StreamSetup(SDW *w, ULONG l)
{
    SDW_LINK *k = &w->L[l];
    ULONG i, j, n = 0;
    BOOLEAN ok;
    for (i = 0; i < w->NS; i++) {
        SDW_STRM *st = &w->S[i];
        BOOLEAN any = FALSE;
        if (!st->Up || st->Link != l) continue;
        for (j = 0; j < st->NDev; j++) any |= w->D[st->Dev[j]].Ready;
        if (!any) { st->Up = FALSE; continue; }
        if (st->Pdi >= k->Pdis) {
            LOGE("sdw: link %u: PDI %u not present (%u PCM PDIs)", l, st->Pdi, k->Pdis);
            st->Up = FALSE;
            continue;
        }
        n++;
    }
    if (!n) return;
    if (!Transport(w, l)) { for (i = 0; i < w->NS; i++) if (w->S[i].Link == l) w->S[i].Up = FALSE; return; }
    for (i = 0; i < w->NS; i++) if (w->S[i].Up && w->S[i].Link == l) PdiSetup(w, &w->S[i]);
    ok = ProgramBank(w, l, !k->Bank, FALSE) && BankSwitch(k, l);
    for (i = 0; ok && i < w->NS; i++) {
        SDW_STRM *st = &w->S[i];
        if (!st->Up || st->Link != l) continue;
        for (j = 0; j < st->NDev; j++)
            if (w->D[st->Dev[j]].Ready) SdwDpnPrepare(&w->D[st->Dev[j]], PortOf(&w->D[st->Dev[j]], st->Dir), TRUE);
    }
    ok = ok && ProgramBank(w, l, !k->Bank, TRUE) && BankSwitch(k, l);
    for (i = 0; i < w->NS; i++) {
        SDW_STRM *st = &w->S[i];
        if (!st->Up || st->Link != l) continue;
        if (!ok) { st->Up = FALSE; continue; }
        LOGI("sdw: link %u PDI %u (ALH %u) %s: %u ch %u bit, interval %u, offset %u, columns %u-%u, %u peripheral(s)",
             l, st->Pdi, AlhId(l, st->Pdi), st->Dir ? "capture" : "playback", st->Ch, st->X.Bits,
             st->X.SampleInterval, st->X.Offset, st->X.HStart, st->X.HStop, st->NDev);
    }
}

/* ------------------------------------------------------------------ */
/* endpoints                                                            */
/* ------------------------------------------------------------------ */
static SST_EP *NewEp(SDW *w, UCHAR dir, UCHAR link, const char *name, UCHAR line, ULONG dev)
{
    SST_EP *e = SstAddEp(w->Sst, EP_SDW, dir, link, name);
    if (!e) return NULL;
    e->Line = line;
    e->Be.Rate = SDW_RATE; e->Be.Ch = 2; e->Be.Bits = 32; e->Be.Valid = SDW_BITS;   /* 24 bits in 32 on the ALH */
    e->SdwDev = (UCHAR)dev;
    return e;
}

static SDW_STRM *NewStream(SDW *w, SST_EP *e, ULONG link, UCHAR dir, UCHAR ch)
{
    SDW_STRM *st;
    if (!e || w->NS >= SDW_STREAMS) return NULL;
    st = &w->S[w->NS++];
    memset(st, 0, sizeof(*st));
    st->Link = (UCHAR)link;
    st->Pdi = w->L[link].NextPdi++;
    st->Dir = dir;
    st->Ch = ch ? ch : 2;
    st->Ep = (UCHAR)(e - w->Sst->Ep);
    st->Up = TRUE;
    e->Pdi = st->Pdi;
    e->Alh = AlhId(link, st->Pdi);
    return st;
}

static void StreamDev(SDW_STRM *st, ULONG dev) { if (st && st->NDev < 4) st->Dev[st->NDev++] = (UCHAR)dev; }

static void BuildEndpoints(SDW *w)
{
    ULONG l, i, j, amps = 0, order[SDW_DEVS];
    SST_EP *spk = NULL;
    for (l = 0; l < SDW_LINKS; l++) w->L[l].NextPdi = 2;               /* SOC_SDW_INTEL_BIDIR_PDI_BASE */
    for (l = 0; l < SDW_LINKS; l++) {
        BOOLEAN hs = FALSE;
        for (i = 0; i < w->ND; i++) {
            SDW_DEV *d = &w->D[i];
            SST_EP *e;
            if (d->Link != l) continue;
            if (d->Kind == SDW_KIND_HEADSET) {
                if (hs) { LOGI("sdw: %s: second jack codec on link %u not used", d->Name, l); continue; }
                hs = TRUE;
                if (d->Play.Num) StreamDev(NewStream(w, e = NewEp(w, 0, (UCHAR)l, "Headphones", 0, i), l, 0, d->Play.Ch), i);
                if (d->Rec.Num) StreamDev(NewStream(w, e = NewEp(w, 1, (UCHAR)l, "Headset Microphone", LINE_MIC, i), l, 1,
                                                    d->Rec.Ch), i);
                (void)e;
            } else if (d->Kind == SDW_KIND_MIC && d->Rec.Num) {
                StreamDev(NewStream(w, NewEp(w, 1, (UCHAR)l, "Microphones", LINE_AUX, i), l, 1, d->Rec.Ch), i);
            }
        }
    }
    /* amplifiers: one speaker endpoint over all links (aggregated), ordered by link and unique id */
    for (i = 0; i < w->ND; i++) if (w->D[i].Kind == SDW_KIND_AMP && w->D[i].Play.Num) order[amps++] = i;
    for (i = 1; i < amps; i++)
        for (j = i; j > 0; j--) {
            SDW_DEV *a = &w->D[order[j - 1]], *b = &w->D[order[j]];
            if ((a->Link << 4 | a->Unique) <= (b->Link << 4 | b->Unique)) break;
            l = order[j]; order[j] = order[j - 1]; order[j - 1] = l;
        }
    for (i = 0; i < amps; i++) { w->D[order[i]].Index = (UCHAR)i; w->D[order[i]].Count = (UCHAR)amps; }
    if (amps) spk = NewEp(w, 0, w->D[order[0]].Link, "Speakers", 0, order[0]);
    for (l = 0; spk && l < SDW_LINKS; l++) {
        SDW_STRM *st = NULL;
        UCHAR ch = 0;
        for (i = 0; i < amps; i++) if (w->D[order[i]].Link == l) ch = MAX(ch, w->D[order[i]].Play.Ch);
        if (!ch) continue;
        st = NewStream(w, spk, l, 0, MAX(ch, 2));
        for (i = 0; i < amps; i++) if (w->D[order[i]].Link == l) StreamDev(st, order[i]);
    }
    if (spk) {
        /* the first stream's PDI / ALH on the endpoint (aggregation is described by the gateway blob) */
        for (i = 0; i < w->NS; i++) if (&w->Sst->Ep[w->S[i].Ep] == spk) { spk->Port = w->S[i].Link; spk->Pdi = w->S[i].Pdi;
                                                                           spk->Alh = AlhId(spk->Port, spk->Pdi); break; }
    }
}

/* drops endpoints without a running stream; the others get their ALH gateway blob (ipc4-topology.c
   sof_ipc4_alh_configuration_blob: gateway attributes, device count, { device, channel mask } per stream).
   A device is the ALH stream (node type ALH out 16 / in 17 << 8 | ALH id); on ACE 2.x+ the HD Audio link
   DMA mapping (link << 8 | PDI) of the stream's DMA configuration TLV.  More than one stream: an ALH
   multi-gateway, node index 0x50 (ALH_MULTI_GTW_BASE) */
static void Finish(SDW *w)
{
    SST *s = w->Sst;
    ULONG i, j, n = 0, map[SST_MAX_EP];
    for (i = 0; i < s->NEp; i++) {
        BOOLEAN alive = s->Ep[i].Kind != EP_SDW;
        for (j = 0; j < w->NS && !alive; j++) alive = w->S[j].Up && w->S[j].Ep == i;
        map[i] = alive ? n : 0xFF;
        if (!alive) { LOGI("sdw: endpoint %s dropped (no peripheral answered)", s->Ep[i].Name); continue; }
        if (n != i) s->Ep[n] = s->Ep[i];
        n++;
    }
    s->NEp = n;
    for (j = 0; j < w->NS; j++) {
        if (w->S[j].Ep < SST_MAX_EP && map[w->S[j].Ep] != 0xFF) w->S[j].Ep = (UCHAR)map[w->S[j].Ep];
        else { w->S[j].Ep = 0xFF; w->S[j].Up = FALSE; }
    }
    w->NG = 0;
    for (i = 0; i < s->NEp; i++) {
        SST_EP *e = &s->Ep[i];
        SDW_GW *g;
        ULONG c = 0;
        if (e->Kind != EP_SDW || w->NG >= SST_MAX_EP) continue;
        g = &w->G[w->NG++];
        g->Ep = (UCHAR)i;
        memset(g->Blob, 0, sizeof(g->Blob));
        for (j = 0; j < w->NS && c < 4; j++) {
            SDW_STRM *st = &w->S[j];
            if (!st->Up || st->Ep != i) continue;
            if (!c) { e->Port = st->Link; e->Pdi = st->Pdi; e->SdwDev = st->Dev[0]; e->Be.Ch = st->Ch; }
            g->Blob[2 + 2 * c] = w->Chip->Ext ? ((ULONG)st->Link << 8) | st->Pdi
                                              : ((ULONG)(st->Dir ? 17 : 16) << 8) | AlhId(st->Link, st->Pdi);
            g->Blob[3 + 2 * c] = (1UL << st->Ch) - 1;
            c++;
        }
        g->Blob[1] = c;
        e->Alh = c > 1 ? 0x50 : AlhId(e->Port, e->Pdi);
        e->Blob = (const UCHAR *)g->Blob;
        e->BlobLen = 8 + 8 * c;
        LOGI("sdw: endpoint %s: %s, link %u PDI %u, ALH %s%u, %u stream(s)", e->Name, e->Dir ? "capture" : "playback",
             e->Port, e->Pdi, c > 1 ? "multi-gateway " : "", e->Alh, c);
    }
}

/* ------------------------------------------------------------------ */
/* worker (the peripherals' power sequences sleep)                      */
/* ------------------------------------------------------------------ */
static void RunStream(SDW *w, ULONG dev, BOOLEAN rec, BOOLEAN on)
{
    SDW_DEV *d = &w->D[dev];
    BOOLEAN *st = rec ? &d->RecOn : &d->PlayOn;
    if (dev >= w->ND || !d->Ready || !d->Drv->Stream || *st == on) return;
    if (!(d->Caps & (rec ? SDW_CAP_REC : SDW_CAP_PLAY))) return;
    d->Drv->Stream(d, rec, on);
    *st = on;
}

static void Worker(void *ctx)
{
    SDW *w = (SDW *)ctx;
    for (;;) {
        OsEventWait(&w->Ev, 1000);
        for (;;) {
            UCHAR dev, rec, on;
            OsLockAcquire(&w->Lock);
            if (w->Head == w->Tail) { OsLockRelease(&w->Lock); break; }
            dev = w->Q[w->Head].Dev; rec = w->Q[w->Head].Rec; on = w->Q[w->Head].On;
            w->Head = (w->Head + 1) % QLEN;
            OsLockRelease(&w->Lock);
            RunStream(w, dev, rec, on);
        }
        if (w->Quit) break;
    }
    OsEventSet(&w->Done);
}

static void Queue(SDW *w, ULONG dev, BOOLEAN rec, BOOLEAN on)
{
    ULONG next;
    if (!w->Thread) {
        if (KeGetCurrentIrql() < DISPATCH_LEVEL) RunStream(w, dev, rec, on);
        return;
    }
    OsLockAcquire(&w->Lock);
    next = (w->Tail + 1) % QLEN;
    if (next != w->Head) {
        w->Q[w->Tail].Dev = (UCHAR)dev; w->Q[w->Tail].Rec = (UCHAR)rec; w->Q[w->Tail].On = (UCHAR)on;
        w->Tail = next;
    }
    OsLockRelease(&w->Lock);
    OsEventSet(&w->Ev);
}

/* ------------------------------------------------------------------ */
/* bring-up                                                             */
/* ------------------------------------------------------------------ */
static void BusUp(SDW *w)
{
    ULONG l, i, used = 0, alive = 0;
    w->Started = TRUE;
    for (i = 0; i < w->ND; i++) used |= 1UL << w->D[i].Link;
    w->Mask &= used;                                    /* only links with described peripherals are powered */
    if (!w->Mask) { Finish(w); return; }
    if (!(w->Chip->Ext ? ExtPowerUp(w) : ShimPowerUp(w))) { PowerDown(w, 0xFF); Finish(w); return; }
    for (l = 0; l < SDW_LINKS; l++) {
        SDW_LINK *k = &w->L[l];
        if (!k->Powered) continue;
        if (!CdnsStart(k, l)) { PowerDown(w, 1UL << l); continue; }
        Enumerate(w, l);
    }
    for (i = 0; i < w->ND; i++) {
        SDW_DEV *d = &w->D[i];
        if (!d->Attached) { LOGI("sdw: %s did not attach", d->Name); continue; }
        DevInit(w, d);
    }
    for (l = 0; l < SDW_LINKS; l++) {
        BOOLEAN any = FALSE;
        if (!w->L[l].Up) continue;
        StreamSetup(w, l);
        for (i = 0; i < w->NS; i++) any |= w->S[i].Up && w->S[i].Link == l;
        if (any) alive |= 1UL << l;
        else { LOGI("sdw: link %u: no stream, powered down", l); PowerDown(w, 1UL << l); }
    }
    for (i = 0; i < w->NS; i++) if (!(alive & (1UL << w->S[i].Link))) w->S[i].Up = FALSE;   /* links not running */
    Finish(w);
    OsLockInit(&w->Lock);
    OsEventInit(&w->Ev);
    OsEventInit(&w->Done);
    if (alive) w->Thread = OsThreadStart(Worker, w);
    LOGI("sdw: %u peripheral(s), links %x running", w->ND, alive);
}

/* ------------------------------------------------------------------ */
/* sst.c interface                                                      */
/* ------------------------------------------------------------------ */
BOOLEAN SdwProbe(SST *s)
{
    SDW *w;
    CSCAN c;
    ULONGLONG v;
    ULONG i, l, mask, over = OsRegGetDword(L"SdwLinkMask", 0);
    const SDW_CHIP *chip = NULL;
    if (!OsRegGetDword(L"Sdw", 1)) return FALSE;
    for (i = 0; i < ARRAYSIZE(g_Chips); i++) if (g_Chips[i].Gen == s->P->Gen) chip = &g_Chips[i];
    if (!chip) return FALSE;
    w = (SDW *)OsAlloc(sizeof(SDW));
    if (!w) return FALSE;
    w->Sst = s;
    w->Chip = chip;
    memset(&c, 0, sizeof(c));
    c.W = w;
    c.Hda = (s->Card->Dev << 16) | s->Card->Fn;
    AmlForEachDevice(ScanCtrl, &c);
    if (!c.Found) { OsFree(w); return FALSE; }
    if (SdwAcpiInt(&w->Ctrl, "mipi-sdw-manager-list", &v) && v) {
        mask = (ULONG)v & 0x1F;
        for (i = 0, w->Count = 0; i < 5; i++) if (mask & (1UL << i)) w->Count = i + 1;
    } else if (SdwAcpiInt(&w->Ctrl, "mipi-sdw-master-count", &v) && v) {
        w->Count = MIN((ULONG)v, SDW_LINKS);
        mask = (1UL << w->Count) - 1;
    } else {
        LOGI("sdw: controller %s has no mipi-sdw-master-count", w->Ctrl.Path);
        OsFree(w);
        return FALSE;
    }
    if (over) mask &= over;
    for (l = 0; l < SDW_LINKS; l++) {
        if (!(mask & (1UL << l))) continue;
        LinkProps(w, l);
        if (w->L[l].Acpi) w->Mask |= 1UL << l;
    }
    LOGI("sdw: controller %s%s, %u links, enabled %x", w->Ctrl.Path, c.Strong ? "" : " (parent not matched)", w->Count,
         w->Mask);
    AmlForEachDevice(ScanPeriph, w);
    if (!w->ND) { LOGI("sdw: no SoundWire peripherals with a driver"); OsFree(w); return FALSE; }
    s->Sdw = w;
    BuildEndpoints(w);
    if (chip->Ext) BusUp(w);                            /* ACE 2.x+: before the DSP boots (hda_dsp_probe) */
    for (i = 0; i < w->NS; i++) if (w->S[i].Up) return TRUE;
    return FALSE;
}

void SdwStart(SST *s)
{
    SDW *w = Of(s);
    if (!w || w->Started) return;
    BusUp(w);                                           /* cAVS, ACE 1.x: after the firmware runs */
}

void SdwShutdown(SST *s)
{
    SDW *w = Of(s);
    ULONG i;
    if (!w) return;
    if (w->Thread) {
        w->Quit = TRUE;
        OsEventSet(&w->Ev);
        OsEventWait(&w->Done, 5000);
    }
    for (i = 0; i < w->ND; i++) {
        SDW_DEV *d = &w->D[w->ND - 1 - i];
        if (d->Ready && d->Drv->Shutdown) d->Drv->Shutdown(d);
        d->Ready = FALSE;
    }
    PowerDown(w, 0xFF);
    if (w->Win) OsUnmapMmio(w->Win, w->WinSize);
    OsFree(w);
    s->Sdw = NULL;
}

void SdwPower(SST *s, SST_EP *e, BOOLEAN on)
{
    SDW *w = Of(s);
    ULONG i, j, idx;
    if (!w || !e || e->Kind != EP_SDW) return;
    idx = (ULONG)(e - s->Ep);
    for (i = 0; i < w->NS; i++) {
        SDW_STRM *st = &w->S[i];
        if (!st->Up || st->Ep != idx) continue;
        for (j = 0; j < st->NDev; j++) Queue(w, st->Dev[j], st->Dir, on);
    }
}

SST_EP *SdwPlayEp(SST *s)
{
    SDW *w = Of(s);
    SST_EP *hp = NULL;
    ULONG i;
    if (!w) return NULL;
    for (i = 0; i < s->NEp; i++) {
        SST_EP *e = &s->Ep[i];
        if (e->Kind != EP_SDW || e->Dir) continue;
        if (e->SdwDev < w->ND && w->D[e->SdwDev].Kind == SDW_KIND_AMP) return e;
        if (!hp) hp = e;
    }
    return hp;
}

ULONG SdwEpStreams(SST *s, const SST_EP *e, UCHAR *link, UCHAR *pdi, UCHAR *ch, ULONG max)
{
    SDW *w = Of(s);
    ULONG i, n = 0, idx;
    if (!w || !e) return 0;
    idx = (ULONG)(e - s->Ep);
    for (i = 0; i < w->NS && n < max; i++) {
        SDW_STRM *st = &w->S[i];
        if (!st->Up || st->Ep != idx) continue;
        if (link) link[n] = st->Link;
        if (pdi) pdi[n] = st->Pdi;
        if (ch) ch[n] = st->Ch;
        n++;
    }
    return n;
}

void SdwLinkStream(SST *s, ULONG link, ULONG pdi, ULONG ch, ULONG tag, BOOLEAN rec)
{
    SDW *w = Of(s);
    SDW_LINK *k;
    if (!w || !w->Chip->Ext || link >= SDW_LINKS || !(k = &w->L[link])->Shim2) return;
    /* hdac_bus_eml_sdw_map_stream_ch: lowest / highest channel, stream tag, direction */
    MMIO16(k->Shim2, SHIM2_PCMSYCM(pdi)) = (USHORT)((rec ? 0x8000 : 0) | ((tag & 0x3F) << 8) |
                                                   (tag ? ((ch - 1) & 0xF) << 4 : 0));
}

ULONG SdwDump(SST *s, char *buf, ULONG size)
{
    SDW *w = Of(s);
    ULONG n = 0, l, i;
    if (!w) return 0;
    n += OsSprintf(buf + n, size - n, "SoundWire: %s, links %x\r\n", w->Ctrl.Path, w->Mask);
    for (l = 0; l < SDW_LINKS && n + 160 < size; l++) {
        SDW_LINK *k = &w->L[l];
        if (!k->Acpi) continue;
        if (!k->Up && !k->Cmds) { n += OsSprintf(buf + n, size - n, "  link %u: off\r\n", l); continue; }
        n += OsSprintf(buf + n, size - n, "  link %u: %s, IP clock %u, bus %u Hz, frame %u x %u, bank %u, %u PDIs, "
                       "%u commands, %u errors%s\r\n", l, k->Up ? "up" : k->Powered ? "powered" : "off", k->Mclk,
                       k->BusClock, k->Row, k->Col, k->Bank, k->Pdis, k->Cmds, k->Errors,
                       k->Unknown ? ", unknown peripherals" : "");
    }
    for (i = 0; i < w->ND && n + 160 < size; i++) {
        SDW_DEV *d = &w->D[i];
        n += OsSprintf(buf + n, size - n, "  %s: mfg %04x part %04x, link %u dev %u, DP %u/%u%s%s%s%s, %u errors\r\n",
                       d->Name, d->Mfg, d->Part, d->Link, d->DevNum, d->Play.Num, d->Rec.Num,
                       d->Attached ? "" : ", not attached", d->Ready ? "" : ", not ready", d->PlayOn ? ", playing" : "",
                       d->RecOn ? ", recording" : "", d->Errors);
    }
    for (i = 0; i < w->NS && n + 160 < size; i++) {
        SDW_STRM *st = &w->S[i];
        if (!st->Up) continue;
        n += OsSprintf(buf + n, size - n, "  stream link %u PDI %u ALH %u %s: %u ch, offset %u, columns %u-%u\r\n",
                       st->Link, st->Pdi, AlhId(st->Link, st->Pdi), st->Dir ? "capture" : "playback", st->Ch,
                       st->X.Offset, st->X.HStart, st->X.HStop);
    }
    return n;
}
