/*
 * AUDIONT - SoundWire: the Intel SoundWire controller (Cadence masters next
 * to the Smart Sound DSP) and the API for SoundWire peripheral drivers.
 *
 *   sdw.c        ACPI discovery (controller _ADR 0x40000000 under the HD
 *                Audio device, peripherals with a 64-bit _ADR), link power
 *                (cAVS / ACE 1.x: SoundWire shim in the DSP BAR; ACE 2.x+:
 *                HD Audio multi-link SoundWire extended link, registers in
 *                the HD Audio BAR), Cadence MCP init, enumeration, a static
 *                stream per direction, ALH / PDI mapping, the endpoints for
 *                sst.c, a worker thread for the peripherals' power sequences
 *   sdw_sdca.c   generic SDCA peripheral driver (DisCo function / entity
 *                description from ACPI) and SDCA control helpers
 *   sdw_rt711.c  Realtek RT711-SDCA (jack codec)
 *   sdw_rt1316.c Realtek RT1316 (smart amplifier)
 *
 * The bus runs one fixed configuration from probe on: the frame shape and
 * clock the BIOS describes, one playback and one capture stream per link at
 * 48 kHz, 24-bit words, 2 channels, with transport parameters computed once
 * (generic_bandwidth_allocation.c of Linux for a single rate group).  The
 * data ports of master and peripherals are prepared and enabled at probe and
 * stay enabled; starting / stopping audio only runs the DSP's ALH copier and
 * the peripheral driver's Stream callback (power domains, mutes).
 *
 * WRITING A PERIPHERAL DRIVER
 *   - one SDW_DRV descriptor per chip family, listed in g_SdwDrvs (sdw.c)
 *     before the generic SDCA driver; matching is by the manufacturer id and
 *     a 0-terminated part id list from the peripheral's ACPI _ADR (MIPI
 *     manufacturer ids: 0x025D Realtek, 0x01FA Cirrus Logic, 0x019F Maxim;
 *     e.g. _ADR 0x000030025D071101 = link 0, SoundWire 1.2, unique id 0,
 *     Realtek, part 0x0711, class 1 = SDCA)
 *   - Probe(d): from ACPI only (no bus access): set d->Kind and the data
 *     ports d->Play / d->Rec (DP number, channels, port type, whether the
 *     port has the full channel-prepare state machine), d->Paging.  Return
 *     FALSE to leave the device to the next driver (e.g. the generic one).
 *   - Init(d): the peripheral is attached with its device number; reset it,
 *     write the vendor tables, set clocks / sample rate (48 kHz), routing and
 *     gains, leave power domains off.  May sleep (SdwDelay).  Return FALSE on
 *     bus errors.  The data ports are programmed by the bus core afterwards.
 *   - Stream(d, rec, on): runs on the SoundWire worker thread (passive
 *     level, may sleep) after sst.c started the DSP path (on) or stopped it
 *     (off): power domains to PS0 and unmute (on), mute and PS3 (off).
 *     d->PlayOn / d->RecOn still hold the state before the call.
 *   - Shutdown(d): everything off.
 *   - register access: SdwRead / SdwWrite / SdwUpdate on 32-bit SoundWire
 *     addresses (the core does the SCP paging for addresses >= 0x8000 when
 *     d->Paging); SdwReadMbq / SdwWriteMbq for SDCA multi-byte controls
 *     (volume, gain: MSB first through the MBQ alias, LSB last, as the Linux
 *     regmap-sdw-mbq); SdcaXxx helpers below for the standard controls.
 *   - amplifiers: d->Index / d->Count number the amplifiers of the speaker
 *     endpoint (ACPI link, then unique id order): Index 0 of 2 plays the left
 *     channel, 1 the right; a single amplifier plays both / the mix.  All
 *     amplifiers receive both channels (mirrored ports); the driver selects
 *     its channel (e.g. RT1316 UDMPU "RX Channel Select").
 */
#ifndef AUDIONT_SDW_H
#define AUDIONT_SDW_H

#include "sst.h"

/* ------------------------------------------------------------------ */
/* MIPI SoundWire 1.2 peripheral registers (Linux sdw_registers.h)      */
/* ------------------------------------------------------------------ */
#define SDW_REG_NO_PAGE             0x00008000UL    /* from here on: SCP paging (when supported) */
#define SDW_REG_MAX                 0x48000000UL
#define SDW_SCP_ADDRPAGE1_SHIFT     23              /* address bits 30:23 */
#define SDW_SCP_ADDRPAGE2_SHIFT     15              /* address bits 22:15 */

#define SDW_DP0_INT                 0x00
#define SDW_DP0_INTMASK             0x01
#define SDW_DP0_PORTCTRL            0x02
#define SDW_SCP_INT1                0x40
#define SDW_SCP_INTMASK1            0x41
#define SDW_SCP_INT1_PARITY         0x01
#define SDW_SCP_INT1_BUS_CLASH      0x02
#define SDW_SCP_INTSTAT2            0x42
#define SDW_SCP_INTSTAT3            0x43
#define SDW_SCP_CTRL                0x44            /* write: CTRL, read: STAT */
#define SDW_SCP_STAT_CURR_BANK      0x40
#define SDW_SCP_SYSTEMCTRL          0x45
#define SDW_SCP_DEVNUMBER           0x46
#define SDW_SCP_ADDRPAGE1           0x48
#define SDW_SCP_ADDRPAGE2           0x49
#define SDW_SCP_KEEPEREN            0x4A
#define SDW_SCP_BANKDELAY           0x4B
#define SDW_SCP_COMMIT              0x4C
#define SDW_SCP_BUS_CLOCK_BASE      0x4D
#define SDW_SCP_DEVID_0             0x50            /* .. 0x55 */
#define SDW_SCP_SDCA_INT1           0x58
#define SDW_SCP_SDCA_INTMASK1       0x5C
#define SDW_SCP_FRAMECTRL_B0        0x60
#define SDW_SCP_FRAMECTRL_B1        0x70
#define SDW_SCP_BUSCLOCK_SCALE_B0   0x62
#define SDW_SCP_BUSCLOCK_SCALE_B1   0x72

/* data port n (1..14) */
#define SDW_DPN_SIZE                0x100
#define SDW_DPN_INT(n)              (0x00 + SDW_DPN_SIZE * (n))
#define SDW_DPN_INTMASK(n)          (0x01 + SDW_DPN_SIZE * (n))
#define SDW_DPN_PORTCTRL(n)         (0x02 + SDW_DPN_SIZE * (n))     /* 3:2 data mode, 1:0 flow mode */
#define SDW_DPN_BLOCKCTRL1(n)       (0x03 + SDW_DPN_SIZE * (n))     /* word length - 1 */
#define SDW_DPN_PREPARESTATUS(n)    (0x04 + SDW_DPN_SIZE * (n))
#define SDW_DPN_PREPARECTRL(n)      (0x05 + SDW_DPN_SIZE * (n))
/* banked: bank 1 at + 0x10 */
#define SDW_DPN_CHANNELEN(n, b)     (0x20 + 0x10 * (b) + SDW_DPN_SIZE * (n))
#define SDW_DPN_BLOCKCTRL2(n, b)    (0x21 + 0x10 * (b) + SDW_DPN_SIZE * (n))
#define SDW_DPN_SAMPLECTRL1(n, b)   (0x22 + 0x10 * (b) + SDW_DPN_SIZE * (n))
#define SDW_DPN_SAMPLECTRL2(n, b)   (0x23 + 0x10 * (b) + SDW_DPN_SIZE * (n))
#define SDW_DPN_OFFSETCTRL1(n, b)   (0x24 + 0x10 * (b) + SDW_DPN_SIZE * (n))
#define SDW_DPN_OFFSETCTRL2(n, b)   (0x25 + 0x10 * (b) + SDW_DPN_SIZE * (n))
#define SDW_DPN_HCTRL(n, b)         (0x26 + 0x10 * (b) + SDW_DPN_SIZE * (n))
#define SDW_DPN_BLOCKCTRL3(n, b)    (0x27 + 0x10 * (b) + SDW_DPN_SIZE * (n))
#define SDW_DPN_LANECTRL(n, b)      (0x28 + 0x10 * (b) + SDW_DPN_SIZE * (n))

/* SDCA control address (SDCA 1.0 / Linux SDW_SDCA_CTL): function, entity, control selector, channel */
#define SDW_SDCA_CTL(fun, ent, ctl, ch) ((1UL << 30) |                                  \
                                         (((ULONG)(fun) & 0x7) << 22) |                 \
                                         (((ULONG)(ent) & 0x40) << 15) |                \
                                         (((ULONG)(ent) & 0x3F) << 7) |                 \
                                         (((ULONG)(ctl) & 0x30) << 15) |                \
                                         (((ULONG)(ctl) & 0x0F) << 3) |                 \
                                         (((ULONG)(ch) & 0x38) << 12) |                 \
                                         ((ULONG)(ch) & 0x7))
#define SDW_SDCA_MBQ_CTL(reg)       ((reg) | (1UL << 13))
#define SDW_SDCA_NEXT_CTL(reg)      ((reg) | (1UL << 14))

/* SDCA function types (SDCA 1.0 5.1.2) */
#define SDCA_FN_SMART_AMP           0x01
#define SDCA_FN_SIMPLE_AMP          0x02
#define SDCA_FN_SMART_MIC           0x03
#define SDCA_FN_SIMPLE_MIC          0x04
#define SDCA_FN_SPEAKER_MIC         0x05
#define SDCA_FN_UAJ                 0x06
#define SDCA_FN_RJ                  0x07
#define SDCA_FN_SIMPLE_JACK         0x08
#define SDCA_FN_HID                 0x0A
/* entity types (SDCA 1.0 6.1.2) */
#define SDCA_ENT_IT                 0x02
#define SDCA_ENT_OT                 0x03
#define SDCA_ENT_FU                 0x07
#define SDCA_ENT_XU                 0x0A
#define SDCA_ENT_CS                 0x0B
#define SDCA_ENT_PDE                0x11
#define SDCA_ENT_GE                 0x12
/* controls */
#define SDCA_CTL_ENT0_FUNCTION_TOPOLOGY 0x05
#define SDCA_CTL_FU_MUTE            0x01
#define SDCA_CTL_FU_VOLUME          0x02            /* MBQ 16 bit, Q7.8 dB */
#define SDCA_CTL_FU_GAIN            0x0B
#define SDCA_CTL_CS_SAMPLERATEINDEX 0x10
#define SDCA_CTL_PDE_REQUESTED_PS   0x01
#define SDCA_CTL_PDE_ACTUAL_PS      0x10
#define SDCA_CTL_GE_SELECTED_MODE   0x01
#define SDCA_CTL_GE_DETECTED_MODE   0x02
#define SDCA_PS0                    0
#define SDCA_PS3                    3

/* ------------------------------------------------------------------ */
/* peripherals                                                          */
/* ------------------------------------------------------------------ */
#define SDW_KIND_HEADSET  1         /* jack codec: headphone playback, headset microphone capture */
#define SDW_KIND_AMP      2         /* speaker amplifier (playback; feedback ports are not used) */
#define SDW_KIND_MIC      3         /* microphone array codec (capture) */

#define SDW_CAP_PLAY      1         /* d->Caps after Init */
#define SDW_CAP_REC       2

#define SDW_DP_FULL       0         /* DisCo mipi-sdw-data-port-type */
#define SDW_DP_SIMPLE     1
#define SDW_DP_REDUCED    2

typedef struct _SDW_DPN {
    UCHAR   Num;                    /* data port 1..14, 0: none */
    UCHAR   Ch;                     /* channels the port carries (mask (1 << Ch) - 1) */
    UCHAR   Type;                   /* SDW_DP_xxx */
    UCHAR   CpSm;                   /* TRUE: full channel prepare state machine (DPn_PrepareCtrl / Status) */
} SDW_DPN;

/* transport parameters of one port (bus core) */
typedef struct _SDW_XPORT {
    USHORT  SampleInterval;         /* bits per sample interval (bus double-rate clock / 48 kHz) */
    USHORT  Offset;                 /* block offset (OffsetCtrl1 / 2) */
    UCHAR   HStart, HStop;          /* horizontal window (columns) */
    UCHAR   Bits;                   /* word length */
    UCHAR   Lane;
} SDW_XPORT;

struct _SDW_DRV;

typedef struct _SDW_DEV {
    SST     *Sst;
    const struct _SDW_DRV *Drv;
    ULONGLONG Adr;                  /* ACPI _ADR */
    AML_DEV Acpi;                   /* the ACPI device (DisCo properties) */
    UCHAR   Link;                   /* SoundWire link (master) */
    UCHAR   DevNum;                 /* assigned device number 1..11, 0 before enumeration */
    UCHAR   Kind;                   /* SDW_KIND_xxx */
    UCHAR   Caps;                   /* SDW_CAP_xxx (the core sets them from Play / Rec; Init may clear) */
    UCHAR   Index, Count;           /* amplifiers: position on the speaker endpoint (0 = left) */
    UCHAR   Version, Unique, Class; /* from _ADR / DevId */
    USHORT  Mfg, Part;
    SDW_DPN Play, Rec;              /* data ports (set by Probe) */
    UCHAR   Bits;                   /* word length on the wire (24) */
    ULONG   Rate;                   /* 48000 */
    BOOLEAN Paging;                 /* SCP_AddrPage1/2 for addresses >= 0x8000 (DisCo mipi-sdw-paging-supported) */
    BOOLEAN Attached, Ready, PlayOn, RecOn;
    UCHAR   Page1, Page2;           /* cached SCP pages (core) */
    BOOLEAN PageValid;
    ULONG   Errors;
    char    Name[20];
    ULONG   Priv[24];               /* driver private */
} SDW_DEV;

typedef struct _SDW_DRV {
    const char *Name;
    USHORT  Mfg;                    /* 0: any manufacturer (Probe decides) */
    const USHORT *Parts;            /* 0-terminated part ids, NULL: any */
    UCHAR   Kind;                   /* default d->Kind */
    BOOLEAN (*Probe)(SDW_DEV *d);   /* ACPI only, no bus access: kind and data ports */
    BOOLEAN (*Init)(SDW_DEV *d);    /* attached: reset, vendor / SDCA setup, 48 kHz; may sleep */
    void    (*Stream)(SDW_DEV *d, BOOLEAN rec, BOOLEAN on);    /* worker thread: power / mute */
    void    (*Shutdown)(SDW_DEV *d);
} SDW_DRV;

extern const SDW_DRV * const g_SdwDrvs[];

/* register access (passive level: probe and the SoundWire worker thread only); FALSE on a NACK / no ACK */
BOOLEAN SdwRead(SDW_DEV *d, ULONG addr, UCHAR *val);
ULONG   SdwGet(SDW_DEV *d, ULONG addr);                     /* 0xFFFFFFFF on error */
BOOLEAN SdwWrite(SDW_DEV *d, ULONG addr, UCHAR val);
BOOLEAN SdwUpdate(SDW_DEV *d, ULONG addr, UCHAR mask, UCHAR val);
BOOLEAN SdwReadMbq(SDW_DEV *d, ULONG addr, ULONG bytes, ULONG *val);   /* SDCA multi-byte quantity */
BOOLEAN SdwWriteMbq(SDW_DEV *d, ULONG addr, ULONG bytes, ULONG val);
BOOLEAN SdwSeq(SDW_DEV *d, const ULONG *pairs, ULONG n);   /* { addr, 8-bit value } x n */
void    SdwDelay(ULONG ms);

/* data port helpers (bus core; drivers normally do not need them) */
BOOLEAN SdwDpnProgram(SDW_DEV *d, const SDW_DPN *p, const SDW_XPORT *x, ULONG bank);
BOOLEAN SdwDpnEnable(SDW_DEV *d, const SDW_DPN *p, ULONG bank, BOOLEAN on);
BOOLEAN SdwDpnPrepare(SDW_DEV *d, const SDW_DPN *p, BOOLEAN on);

/* ACPI DisCo helpers.  'Sub' resolves a "...-subproperties" reference (hierarchical _DSD data
   extension: a string naming a Name (XXXX, Package () { ToUUID (..), Package () {..} }) of the device)
   to a scope for the other calls; AcpiFunction finds the SDCA function child device (_ADR 1..7). */
BOOLEAN SdwAcpiInt(const AML_DEV *scope, const char *prop, ULONGLONG *v);
ULONG   SdwAcpiInts(const AML_DEV *scope, const char *prop, ULONG *v, ULONG max);   /* integer list */
BOOLEAN SdwAcpiSub(const AML_DEV *scope, const char *prop, AML_DEV *sub);
ULONG   SdwAcpiBuf(const AML_DEV *scope, const char *prop, UCHAR *buf, ULONG max);  /* Buffer () bytes */
BOOLEAN SdwAcpiFunction(const SDW_DEV *d, ULONG fn, AML_DEV *out);

/* SDCA control helpers (sdw_sdca.c) */
BOOLEAN SdcaPower(SDW_DEV *d, ULONG fn, ULONG pde, ULONG ps, ULONG waitMs);   /* waitMs: poll ACTUAL_PS */
BOOLEAN SdcaMute(SDW_DEV *d, ULONG fn, ULONG fu, ULONG chMask, BOOLEAN mute);
BOOLEAN SdcaVolume(SDW_DEV *d, ULONG fn, ULONG fu, ULONG ctl, ULONG chMask, LONG q78);   /* dB * 256 */
BOOLEAN SdcaRate(SDW_DEV *d, ULONG fn, ULONG cs, ULONG index);
extern const SDW_DRV g_SdwSdca;

/* ------------------------------------------------------------------ */
/* sst.c / IPC interface                                                */
/* ------------------------------------------------------------------ */
BOOLEAN SdwProbe(SST *s);           /* before the DSP boots: ACPI, endpoints (ACE 2.x+: bus bring-up) */
void    SdwStart(SST *s);           /* after the DSP booted (cAVS, ACE 1.x: bus bring-up); drops dead endpoints */
void    SdwShutdown(SST *s);
void    SdwPower(SST *s, SST_EP *e, BOOLEAN on);           /* any IRQL: queues the peripherals' Stream */
SST_EP *SdwPlayEp(SST *s);                                  /* speakers, else headphones */
ULONG   SdwDump(SST *s, char *buf, ULONG size);
/* the ALH streams of an endpoint: link and PDI per stream (aggregated amplifiers: one per link) */
ULONG   SdwEpStreams(SST *s, const SST_EP *e, UCHAR *link, UCHAR *pdi, UCHAR *ch, ULONG max);
/* ACE 2.x+: PCMSyCM of a PDI: the HD Audio link DMA stream tag it uses (tag 0: unmapped) */
void    SdwLinkStream(SST *s, ULONG link, ULONG pdi, ULONG ch, ULONG tag, BOOLEAN rec);

#endif
