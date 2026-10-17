/*
 * AUDIONT - Intel High Definition Audio back-end
 *
 * Controller: any HDA controller (PCI class 04/03) - Intel ICH6..PCH,
 *             AMD/ATI, nVidia, VIA, SiS, ULi, Creative, ... with the vendor
 *             snoop/coherency tweaks the BIOS may not have applied.
 * Codec     : generic parser.  The widget graph of the first codec with an
 *             analog output is walked: every analog output pin (line-out,
 *             speaker, headphone) gets a path to a DAC, all DACs listen to
 *             the same stream tag, amplifiers are set to 0 dB, EAPD is
 *             enabled.  An ADC path is built for every analog input pin so
 *             the record source can be switched.  Analog loopback inputs on
 *             the output mixers become the CD/Line/Mic/Aux volume controls.
 *             Headphone jacks are polled and speakers muted when a
 *             headphone is plugged in.
 */
#include "audiont.h"
#include "sst.h"

/* controller registers */
#define GCAP      0x00
#define GCTL      0x08
#define WAKEEN    0x0C
#define STATESTS  0x0E
#define INTCTL    0x20
#define INTSTS    0x24
#define LLCH      0x14
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4A
#define CORBCTL   0x4C
#define CORBSTS   0x4D
#define CORBSIZE  0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5A
#define RIRBCTL   0x5C
#define RIRBSTS   0x5D
#define RIRBSIZE  0x5E
#define ICOI      0x60
#define ICII      0x64
#define ICIS      0x68
#define SSYNC_OLD 0x34
#define SSYNC     0x38
#define DPLBASE   0x70
#define DPUBASE   0x74
#define SD_BASE   0x80
#define SD_CTL    0x00
#define SD_STS    0x03
#define SD_LPIB   0x04
#define SD_CBL    0x08
#define SD_LVI    0x0C
#define SD_FIFOS  0x10
#define SD_FMT    0x12
#define SD_BDPL   0x18
#define SD_BDPU   0x1C

#define SDCTL_SRST 0x01
#define SDCTL_RUN  0x02
#define SDCTL_IOCE 0x04
#define SDCTL_FEIE 0x08
#define SDCTL_DEIE 0x10

/* verbs */
#define V_GET_PARAM     0xF00
#define V_GET_CONN_SEL  0xF01
#define V_SET_CONN_SEL  0x701
#define V_GET_CONN_LIST 0xF02
#define V_SET_POWER     0x705
#define V_SET_CONV      0x706
#define V_SET_PIN_CTL   0x707
#define V_GET_PIN_CTL   0xF07
#define V_GET_SENSE     0xF09
#define V_SET_SENSE     0x709
#define V_GET_POWER     0xF05
#define V_SET_EAPD      0x70C
#define V_GET_EAPD      0xF0C
#define V_GET_CONFIG    0xF1C
#define V_SET_GPIO_DATA 0x715
#define V_SET_GPIO_MASK 0x716
#define V_SET_GPIO_DIR  0x717
#define V_SET_FMT       0x2      /* 4-bit verb, 16-bit payload */
#define V_SET_AMP       0x3
#define V_SET_PROC_COEF 0x4      /* vendor processing coefficients (Realtek) */
#define V_SET_COEF_IDX  0x5
#define V_GET_PROC_COEF 0xC
#define V_GET_GPIO_DATA 0xF15

/* parameters */
#define P_VENDOR   0x00
#define P_REV      0x02
#define P_NODES    0x04
#define P_FGTYPE   0x05
#define P_WCAPS    0x09
#define P_PCM      0x0A
#define P_PINCAPS  0x0C
#define P_INAMP    0x0D
#define P_CONNLEN  0x0E
#define P_POWER    0x0F
#define P_GPIO     0x11
#define P_OUTAMP   0x12

/* widget types */
#define W_OUT   0
#define W_IN    1
#define W_MIX   2
#define W_SEL   3
#define W_PIN   4
#define W_POWER 5
#define W_KNOB  6
#define W_BEEP  7

#define WC_STEREO   (1 << 0)
#define WC_INAMP    (1 << 1)
#define WC_OUTAMP   (1 << 2)
#define WC_AMPOVR   (1 << 3)
#define WC_CONNLIST (1 << 8)
#define WC_DIGITAL  (1 << 9)
#define WC_POWERCTL (1 << 10)

#define PC_TRIGGER  (1 << 1)
#define PC_PRESENCE (1 << 2)
#define PC_HPAMP    (1 << 3)
#define PC_OUT      (1 << 4)
#define PC_IN       (1 << 5)
#define PC_EAPD     (1 << 16)

/* default device (config default bits 23:20) */
#define DEV_LINEOUT 0x0
#define DEV_SPEAKER 0x1
#define DEV_HP      0x2
#define DEV_CD      0x3
#define DEV_LINEIN  0x8
#define DEV_AUX     0x9
#define DEV_MIC     0xA

#define MAX_WIDGETS 160
#define MAX_PATH    8
#define MAX_OUTPINS 8
#define MAX_CONN    32

typedef struct _HDA_WIDGET {
    UCHAR  Type;
    UCHAR  NConn;
    USHORT Nid;
    ULONG  Caps, PinCaps, Config, InAmp, OutAmp, Pcm;
    USHORT Conn[MAX_CONN];
} HDA_WIDGET;

typedef struct _HDA_PATH {
    UCHAR  N;
    USHORT Nid[MAX_PATH];
    UCHAR  Idx[MAX_PATH];
} HDA_PATH;

typedef struct _LOOPBACK {
    BOOLEAN Present;
    USHORT  MixNid;
    UCHAR   Index;
} LOOPBACK;

/* DMA position source: Linux uses the position buffer on Intel & co. and LPIB where
   the buffer is known to be unreliable; both are watched and the driver switches over
   when the selected one stops moving while the other one runs */
#define POS_POSBUF 0
#define POS_LPIB   1
typedef struct _POSTRACK {
    ULONG LastLpib, LastPb, LpibMs, PbMs;
} POSTRACK;

typedef struct _HDA_STATE {
    PUCHAR Mmio;
    USHORT Gcap;
    ULONG  Iss, Oss;
    ULONG  OutSd, InSd;             /* stream descriptor indices */
    ULONG  OutSdOff, InSdOff;       /* register offsets */
    volatile ULONG *Corb;           /* in c->Aux, shared with the controller's DMA */
    volatile ULONG *Rirb;
    volatile ULONG *PosBuf;
    ULONG  CorbPa, RirbPa, PosPa, BdlOutPa, BdlInPa;
    PULONG BdlOut, BdlIn;
    ULONG  CorbEntries, RirbEntries;
    USHORT CorbWp, RirbRp;
    BOOLEAN Immediate;
    BOOLEAN CorbOk;                 /* the CORB has delivered at least one response */
    BOOLEAN Probing;                /* no transport fallback while probing addresses */
    BOOLEAN SdDword;                /* SDnCTL dword access */
    ULONG  CorbFail;                /* consecutive CORB timeouts */
    ULONG  Flags;                   /* HF_xxx */
    ULONG  MlOff, PpOff;            /* Intel multi-link / processing pipe capabilities */
    ULONG  PosMode;                 /* POS_xxx */
    ULONG  OutFifo, InFifo;         /* SDnFIFOS + 1, bytes */
    ULONG  CodecMask;
    ULONG  Cad;                     /* codec address in use */
    ULONG  CodecVendor, CodecRev, CodecSub;
    USHORT Afg;
    ULONG  AfgInAmp, AfgOutAmp, AfgPcm;
    USHORT StartNid;
    ULONG  NumWidgets;
    HDA_WIDGET *W;
    /* output */
    ULONG  NOut;
    HDA_PATH OutPath[MAX_OUTPINS];
    UCHAR  OutDev[MAX_OUTPINS];
    BOOLEAN HpPresent;
    ULONG  LastJackMs;
    BOOLEAN JackPoll;
    /* input */
    USHORT Adc;
    HDA_PATH InPath[LINE_COUNT];    /* per record source line */
    LOOPBACK Loop[LINE_COUNT];
    USHORT RecAmpNid;               /* node carrying the record gain amp */
    BOOLEAN RecAmpIsInput;
    UCHAR  RecAmpIdx;
    POSTRACK PlayTrk, RecTrk;
    /* vendor codec initialisation (Realtek coefficients, amplifier GPIOs) */
    ULONG  Coef0;
    BOOLEAN Coef0Read;
    USHORT PllNid;                  /* analog PLL gating fix (Linux alc_fix_pll) */
    UCHAR  PllIdx, PllBit;
    ULONG  GpioMask, GpioDir, GpioData;
    ULONG  InitAmp;                 /* 0 none, 1 default (Realtek EAPD/coef 7) */
    /* Smart Sound DSP streams */
    BOOLEAN HaveCodec;
    BOOLEAN DspPlay, DspRec;
    ULONG  DspPlaySd, DspRecSd, DspRecCh, DspRecLast;
    DMA_BUFFER DspBdl;
} HDA_STATE;

#define R8(o)       MMIO8(h->Mmio, (o))
#define R16(o)      MMIO16(h->Mmio, (o))
#define R32(o)      MMIO32(h->Mmio, (o))
#define W8(o, v)    (MMIO8(h->Mmio, (o)) = (UCHAR)(v))
#define W16(o, v)   (MMIO16(h->Mmio, (o)) = (USHORT)(v))
#define W32(o, v)   (MMIO32(h->Mmio, (o)) = (ULONG)(v))

/* ------------------------------------------------------------------ */
/* controller identification and per-chipset quirks                    */
/* (flags follow the Linux snd-hda-intel driver_caps)                  */
/* ------------------------------------------------------------------ */
#define HF_ICH      0x00001UL   /* ICH6..ICH10: stream sync register at 0x34 */
#define HF_SCH      0x00002UL   /* Intel SCH/PCH: DEVC (0x78) bit 11 must be clear (snoop) */
#define HF_SKL      0x00004UL   /* Skylake+: CGCTL.MISCBDCGE around reset, multi-link LCTL */
#define HF_APL      0x00008UL   /* Apollo Lake: raise the DMA FIFO threshold (EM4L) */
#define HF_PIO      0x00010UL   /* use immediate commands instead of CORB/RIRB */
#define HF_NOTCSEL  0x00020UL   /* leave TCSEL (0x44) alone */
#define HF_ATISNOOP 0x00040UL   /* ATI/AMD southbridge: MISC_CNTR2 (0x42) snoop enable */
#define HF_NVSNOOP  0x00080UL   /* nVidia: coherency bits 0x4C/0x4D/0x4E */
#define HF_LPIB     0x00100UL   /* position buffer unreliable: use LPIB */
#define HF_FIFO     0x00200UL   /* AMD: LPIB, capture lags by the FIFO */
#define HF_UNCACHED 0x00400UL   /* controller does not snoop: uncached DMA memory */
#define HF_CTX      0x00800UL   /* Creative CA0110 (X-Fi): RINTCNT 0xC0 */
#define HF_SDDWORD  0x01000UL   /* SDnCTL may only be accessed as a dword */
#define HF_VIA      0x02000UL   /* VIA VT8251/8237A: snoop check */
#define HF_DSP      0x04000UL   /* may be enumerated as audio DSP (class 0401/0480) */
#define HF_ULI      0x08000UL   /* ULi: GCAP reads 0 */

#define HF_INTEL_SKL (HF_SKL | HF_SCH | HF_DSP)

typedef struct { USHORT Ven, Dev; ULONG Flags; const char *Name; } HDA_ID;
static const HDA_ID g_HdaIds[] = {
    { 0x8086, 0x2668, HF_ICH, "Intel ICH6" },
    { 0x8086, 0x27D8, HF_ICH, "Intel ICH7" },
    { 0x8086, 0x269A, HF_ICH, "Intel ESB2" },
    { 0x8086, 0x284B, HF_ICH, "Intel ICH8" },
    { 0x8086, 0x293E, HF_ICH, "Intel ICH9" },
    { 0x8086, 0x293F, HF_ICH, "Intel ICH9" },
    { 0x8086, 0x3A3E, HF_ICH, "Intel ICH10" },
    { 0x8086, 0x3A6E, HF_ICH, "Intel ICH10" },
    { 0x8086, 0x811B, HF_SCH | HF_LPIB, "Intel SCH (Poulsbo)" },
    { 0x8086, 0x080A, HF_SCH, "Intel SCH (Oaktrail)" },
    { 0x8086, 0x3B56, HF_SCH, "Intel 5 Series/3400" },
    { 0x8086, 0x3B57, HF_SCH, "Intel 5 Series/3400" },
    { 0x8086, 0x1C20, HF_SCH, "Intel 6 Series (Cougar Point)" },
    { 0x8086, 0x1D20, HF_SCH, "Intel C600 (Patsburg)" },
    { 0x8086, 0x1E20, HF_SCH, "Intel 7 Series (Panther Point)" },
    { 0x8086, 0x8C20, HF_SCH, "Intel 8 Series (Lynx Point)" },
    { 0x8086, 0x8CA0, HF_SCH, "Intel 9 Series" },
    { 0x8086, 0x8D20, HF_SCH, "Intel C610 (Wellsburg)" },
    { 0x8086, 0x8D21, HF_SCH, "Intel C610 (Wellsburg)" },
    { 0x8086, 0x9C20, HF_SCH, "Intel 8 Series LP" },
    { 0x8086, 0x9C21, HF_SCH, "Intel 8 Series LP" },
    { 0x8086, 0x9CA0, HF_SCH, "Intel 9 Series LP" },
    { 0x8086, 0xA1F0, HF_SCH, "Intel C620 (Lewisburg)" },
    { 0x8086, 0xA270, HF_SCH, "Intel C620 (Lewisburg)" },
    { 0x8086, 0x0F04, HF_SCH, "Intel Bay Trail" },
    { 0x8086, 0x2284, HF_SCH, "Intel Braswell" },
    { 0x8086, 0x0A0C, HF_SCH, "Intel Haswell HDMI" },
    { 0x8086, 0x0C0C, HF_SCH, "Intel Haswell HDMI" },
    { 0x8086, 0x0D0C, HF_SCH, "Intel Haswell HDMI" },
    { 0x8086, 0x160C, HF_SCH | HF_LPIB, "Intel Broadwell HDMI" },
    { 0x8086, 0xA170, HF_INTEL_SKL, "Intel 100 Series (Skylake)" },
    { 0x8086, 0x9D70, HF_INTEL_SKL, "Intel Skylake-LP" },
    { 0x8086, 0xA171, HF_INTEL_SKL, "Intel Kaby Lake" },
    { 0x8086, 0x9D71, HF_INTEL_SKL, "Intel Kaby Lake-LP" },
    { 0x8086, 0xA2F0, HF_INTEL_SKL, "Intel 200 Series (Kaby Lake-H)" },
    { 0x8086, 0xA348, HF_INTEL_SKL, "Intel 300 Series (Cannon Lake-H)" },
    { 0x8086, 0x9DC8, HF_INTEL_SKL, "Intel Cannon Lake-LP" },
    { 0x8086, 0x02C8, HF_INTEL_SKL, "Intel Comet Lake-LP" },
    { 0x8086, 0x06C8, HF_INTEL_SKL, "Intel Comet Lake-H" },
    { 0x8086, 0xA3F0, HF_INTEL_SKL, "Intel Comet Lake-S" },
    { 0x8086, 0xF0C8, HF_INTEL_SKL, "Intel Comet Lake-R" },
    { 0x8086, 0xF1C8, HF_INTEL_SKL, "Intel Rocket Lake-S" },
    { 0x8086, 0x34C8, HF_INTEL_SKL, "Intel Ice Lake-LP" },
    { 0x8086, 0x38C8, HF_INTEL_SKL, "Intel Ice Lake-N" },
    { 0x8086, 0x3DC8, HF_INTEL_SKL, "Intel Ice Lake-H" },
    { 0x8086, 0x4DC8, HF_INTEL_SKL, "Intel Jasper Lake" },
    { 0x8086, 0xA0C8, HF_INTEL_SKL, "Intel Tiger Lake-LP" },
    { 0x8086, 0x43C8, HF_INTEL_SKL, "Intel Tiger Lake-H" },
    { 0x8086, 0x98C8, HF_INTEL_SKL, "Intel Lakefield" },
    { 0x8086, 0x4B55, HF_INTEL_SKL, "Intel Elkhart Lake" },
    { 0x8086, 0x4B58, HF_INTEL_SKL, "Intel Elkhart Lake" },
    { 0x8086, 0x7AD0, HF_INTEL_SKL, "Intel 600 Series (Alder Lake-S)" },
    { 0x8086, 0x51C8, HF_INTEL_SKL, "Intel Alder Lake-P" },
    { 0x8086, 0x51C9, HF_INTEL_SKL, "Intel Alder Lake-PS" },
    { 0x8086, 0x51CC, HF_INTEL_SKL, "Intel Alder Lake-M" },
    { 0x8086, 0x51CD, HF_INTEL_SKL, "Intel Alder Lake-PX" },
    { 0x8086, 0x54C8, HF_INTEL_SKL, "Intel Alder Lake-N" },
    { 0x8086, 0x7A50, HF_INTEL_SKL, "Intel 700 Series (Raptor Lake-S)" },
    { 0x8086, 0x51CA, HF_INTEL_SKL, "Intel Raptor Lake-P" },
    { 0x8086, 0x51CB, HF_INTEL_SKL, "Intel Raptor Lake-P" },
    { 0x8086, 0x51CE, HF_INTEL_SKL, "Intel Raptor Lake-M" },
    { 0x8086, 0x51CF, HF_INTEL_SKL, "Intel Raptor Lake-PX" },
    { 0x8086, 0x7E28, HF_INTEL_SKL, "Intel Meteor Lake" },
    { 0x8086, 0x7F50, HF_INTEL_SKL, "Intel 800 Series (Arrow Lake-S)" },
    { 0x8086, 0x7728, HF_INTEL_SKL, "Intel Arrow Lake" },
    { 0x8086, 0xA828, HF_INTEL_SKL | HF_PIO, "Intel Lunar Lake" },
    { 0x8086, 0xE428, HF_INTEL_SKL | HF_PIO, "Intel Panther Lake" },
    { 0x8086, 0xE328, HF_INTEL_SKL | HF_PIO, "Intel Panther Lake-H" },
    { 0x8086, 0x4D28, HF_INTEL_SKL | HF_PIO, "Intel Wildcat Lake" },
    { 0x8086, 0xD328, HF_INTEL_SKL | HF_PIO, "Intel Nova Lake" },
    { 0x8086, 0x6E50, HF_INTEL_SKL | HF_PIO, "Intel Nova Lake-S" },
    { 0x8086, 0x5A98, HF_INTEL_SKL | HF_APL, "Intel Apollo Lake" },
    { 0x8086, 0x3198, HF_INTEL_SKL, "Intel Gemini Lake" },
    { 0x8086, 0x490D, HF_SKL | HF_SCH, "Intel DG1 HDMI" },
    { 0x8086, 0x4F90, HF_SKL | HF_SCH, "Intel DG2 HDMI" },
    { 0x8086, 0x4F91, HF_SKL | HF_SCH, "Intel DG2 HDMI" },
    { 0x8086, 0x4F92, HF_SKL | HF_SCH, "Intel DG2 HDMI" },
    { 0x8086, 0xE2F7, HF_SKL | HF_SCH, "Intel Battlemage HDMI" },
    { 0x1002, 0x437B, HF_NOTCSEL | HF_ATISNOOP | HF_LPIB, "ATI SB450" },
    { 0x1002, 0x4383, HF_NOTCSEL | HF_ATISNOOP | HF_LPIB, "ATI SB600/700/800/900" },
    { 0x1022, 0x780D, HF_NOTCSEL | HF_ATISNOOP | HF_LPIB, "AMD Hudson (FCH)" },
    { 0x1022, 0x157A, HF_NOTCSEL | HF_ATISNOOP | HF_LPIB, "AMD Family 15h" },
    { 0x1022, 0x1457, HF_NOTCSEL | HF_ATISNOOP | HF_FIFO, "AMD Family 17h (Zen)" },
    { 0x1022, 0x1487, HF_NOTCSEL | HF_ATISNOOP | HF_FIFO, "AMD Matisse/Starship" },
    { 0x1022, 0x15E3, HF_NOTCSEL | HF_ATISNOOP | HF_FIFO, "AMD Raven/Renoir" },
    { 0x10DE, 0x026C, HF_NVSNOOP, "nVidia MCP51" },
    { 0x10DE, 0x0371, HF_NVSNOOP, "nVidia MCP55" },
    { 0x10DE, 0x03E4, HF_NVSNOOP, "nVidia MCP61" },
    { 0x10DE, 0x03F0, HF_NVSNOOP, "nVidia MCP61" },
    { 0x10DE, 0x044A, HF_NVSNOOP, "nVidia MCP65" },
    { 0x10DE, 0x044B, HF_NVSNOOP, "nVidia MCP65" },
    { 0x10DE, 0x055C, HF_NVSNOOP, "nVidia MCP67" },
    { 0x10DE, 0x055D, HF_NVSNOOP, "nVidia MCP67" },
    { 0x10DE, 0x07FC, HF_NVSNOOP, "nVidia MCP73" },
    { 0x10DE, 0x07FD, HF_NVSNOOP, "nVidia MCP73" },
    { 0x10DE, 0x0774, HF_NVSNOOP, "nVidia MCP78" },
    { 0x10DE, 0x0775, HF_NVSNOOP, "nVidia MCP78" },
    { 0x10DE, 0x0776, HF_NVSNOOP, "nVidia MCP78" },
    { 0x10DE, 0x0777, HF_NVSNOOP, "nVidia MCP78" },
    { 0x10DE, 0x0AC0, HF_NVSNOOP, "nVidia MCP79" },
    { 0x10DE, 0x0AC1, HF_NVSNOOP, "nVidia MCP79" },
    { 0x10DE, 0x0AC2, HF_NVSNOOP, "nVidia MCP79" },
    { 0x10DE, 0x0AC3, HF_NVSNOOP, "nVidia MCP79" },
    { 0x10DE, 0x0D94, HF_NVSNOOP, "nVidia MCP89" },
    { 0x10DE, 0x0D95, HF_NVSNOOP, "nVidia MCP89" },
    { 0x10DE, 0x0D96, HF_NVSNOOP, "nVidia MCP89" },
    { 0x10DE, 0x0D97, HF_NVSNOOP, "nVidia MCP89" },
    { 0x1106, 0x3288, HF_VIA, "VIA VT8251/8237A" },
    { 0x1039, 0x7502, 0, "SiS 966" },
    { 0x10B9, 0x5461, HF_ULI, "ULi M5461" },
    { 0x1102, 0x0009, HF_CTX | HF_LPIB, "Creative X-Fi (CA0110)" },
    { 0x1102, 0x0010, HF_UNCACHED | HF_LPIB, "Creative Sound Core3D" },
    { 0x1102, 0x0012, HF_UNCACHED | HF_LPIB, "Creative Sound Core3D" },
    { 0x13F6, 0x5011, HF_UNCACHED | HF_LPIB, "C-Media CMI8888" },
    { 0x1D17, 0x3288, 0, "Zhaoxin" },
    { 0x1D94, 0x14A9, HF_LPIB | HF_SDDWORD, "Hygon Family 18h" },
    { 0x17F3, 0x3010, 0, "RDC R3010" },
    { 0x15AD, 0x1977, 0, "VMware" },
    { 0, 0, 0, NULL }
};

/* boards whose position buffer is wrong (Linux position_fix_list): LPIB */
static const struct { USHORT Ven, Dev; } g_LpibBoards[] = {
    { 0x1028, 0x01CC }, { 0x1028, 0x01DE }, { 0x103C, 0x306D }, { 0x1043, 0x813D },
    { 0x1043, 0x81B3 }, { 0x1043, 0x81E7 }, { 0x104D, 0x9069 }, { 0x10DE, 0xCB89 },
    { 0x1297, 0x3166 }, { 0x1458, 0xA022 }, { 0x1462, 0x1002 }, { 0x1565, 0x8218 },
    { 0x1849, 0x0888 }, { 0x8086, 0x2503 }, { 0, 0 }
};

/* boards where some codec slots must not be probed (Linux probe_mask_list);
   0x100 = use the mask even where STATESTS does not show the codec */
static const struct { USHORT Ven, Dev, Mask; } g_ProbeMask[] = {
    { 0x1014, 0x05B7, 0x001 },  /* ThinkPad Z60: modem slot breaks the link */
    { 0x17AA, 0x2010, 0x001 },  /* ThinkPad X/T/R60 */
    { 0x17AA, 0x20AC, 0x001 },  /* ThinkPad X/T/R61 */
    { 0x1028, 0x20AC, 0x001 },  /* Dell Studio Desktop */
    { 0x17C0, 0x4085, 0x001 },  /* Medion MD96630: bogus codec in slot 2 */
    { 0x1043, 0x1262, 0x103 },  /* ASUS W5Fm */
    { 0x1046, 0x1262, 0x103 },  /* ASUS W5F */
    { 0x1558, 0x0351, 0x105 },  /* Schenker Dock 15 */
    { 0x3A21, 0x040D, 0x101 },  /* WinFast VP200 H */
    { 0, 0, 0 }
};

/* flags for controllers not in the table, by vendor */
static ULONG VendorFlags(USHORT ven)
{
    switch (ven) {
    case 0x8086: return HF_SCH;                 /* newer than this table: PCH/SKL family */
    case 0x1002: case 0x1022: return HF_NOTCSEL | HF_LPIB;
    case 0x10DE: return HF_NVSNOOP;
    case 0x1102: return HF_CTX | HF_LPIB;
    case 0x6549: case 0x1D17: case 0x1D94: case 0x6766: return HF_LPIB;
    }
    return 0;
}

BOOLEAN HdaProbe(CARD *c)
{
    ULONG cls = PciRead32(c->Bus, c->Dev, c->Fn, 8) >> 8, i, flags;
    const HDA_ID *id = NULL;
    for (i = 0; g_HdaIds[i].Ven; i++)
        if (g_HdaIds[i].Ven == c->VendorId && g_HdaIds[i].Dev == c->DeviceId) { id = &g_HdaIds[i]; break; }
    flags = id ? id->Flags : VendorFlags(c->VendorId);
    /* HD Audio class; Intel Skylake+ controllers report the audio DSP class (0401/0480)
       when the BIOS enabled the DSP, the HDA register set is still there */
    /* newer Intel controllers not in the table may come up in DSP mode as well; the register
       set is checked in HdaInit (an I2S-only audio DSP has no HDA version register) */
    if (!id && c->VendorId == 0x8086 && (c->Bar[0] & 7) == 4) flags |= HF_DSP;   /* 64-bit memory BAR0 (not AC'97) */
    /* 04 03 00: HD Audio; 04 03 80: HD Audio with the DSP enabled (Intel Comet Lake .. current with
       digital microphones: the HDA link and codec are still there, Linux "DSP or legacy") */
    if ((cls >> 8) != 0x0403 && !((flags & HF_DSP) && ((cls >> 8) == 0x0401 || (cls >> 8) == 0x0480))) return FALSE;
    c->Type = HW_HDA;
    c->Ops = &g_HdaOps;
    for (i = 0; g_LpibBoards[i].Ven; i++)
        if (g_LpibBoards[i].Ven == c->SubVendor && g_LpibBoards[i].Dev == c->SubDevice) flags |= HF_LPIB;
    c->HwFlags = flags;
    if (id) OsSprintf(c->Desc, sizeof(c->Desc), "%s HD Audio", id->Name);
    else OsSprintf(c->Desc, sizeof(c->Desc), "HD Audio controller %04x:%04x", c->VendorId, c->DeviceId);
    /* VIA VT8251/8237A rev 20/30: the BIOS may have switched snooping off */
    if ((flags & HF_VIA) && (c->Revision == 0x20 || c->Revision == 0x30) &&
        !(PciRead8(c->Bus, c->Dev, c->Fn, 0x42) & 0x80))
        c->HwFlags |= HF_UNCACHED;
    i = OsRegGetDword(L"UncachedDma", 2);
    c->DmaUncached = i == 2 ? (c->HwFlags & HF_UNCACHED) != 0 : i != 0;
    return TRUE;
}

static void PciUpdate8(CARD *c, ULONG off, UCHAR mask, UCHAR val)
{
    UCHAR v = PciRead8(c->Bus, c->Dev, c->Fn, off), n = (UCHAR)((v & ~mask) | val);
    if (n != v) PciWrite8(c->Bus, c->Dev, c->Fn, off, n);
}

static void PciTweaks(CARD *c, HDA_STATE *h)
{
    ULONG f = c->HwFlags;
    (void)h;
    /* TCSEL = TC0: avoids static/clicks on many codecs */
    if (!(f & HF_NOTCSEL)) PciUpdate8(c, 0x44, 0x07, 0);
    if (f & HF_ATISNOOP) PciUpdate8(c, 0x42, 0x07, c->DmaUncached ? 0 : 0x02);
    if (f & HF_NVSNOOP) {
        PciUpdate8(c, 0x4E, 0x0F, 0x0F);
        PciUpdate8(c, 0x4D, 0x01, 0x01);
        PciUpdate8(c, 0x4C, 0x01, 0x01);
    }
    if (f & HF_SCH) {                   /* DEVC: NOSNOOP off */
        USHORT d = PciRead16(c->Bus, c->Dev, c->Fn, 0x78);
        if ((d & 0x0800) && !c->DmaUncached) PciWrite16(c->Bus, c->Dev, c->Fn, 0x78, (USHORT)(d & ~0x0800));
    }
    LOGV("hda: pci 40-4f %08x %08x %08x %08x, 78 %04x, flags %x%s", PciRead32(c->Bus, c->Dev, c->Fn, 0x40),
         PciRead32(c->Bus, c->Dev, c->Fn, 0x44), PciRead32(c->Bus, c->Dev, c->Fn, 0x48),
         PciRead32(c->Bus, c->Dev, c->Fn, 0x4C), PciRead16(c->Bus, c->Dev, c->Fn, 0x78), f,
         c->DmaUncached ? ", uncached DMA" : "");
}

/* ------------------------------------------------------------------ */
/* codec command transport                                             */
/* ------------------------------------------------------------------ */
static BOOLEAN CmdImmediate(HDA_STATE *h, ULONG cmd, ULONG *resp)
{
    ULONG t0 = OsTimeMs();
    while (R16(ICIS) & 1) {             /* ICB: previous command still busy */
        if (OsTimeMs() - t0 > 10) break;
        OsStallUs(1);
    }
    W16(ICIS, 2);                       /* clear IRV */
    W32(ICOI, cmd);
    W16(ICIS, 1);                       /* ICB */
    t0 = OsTimeMs();
    for (;;) {
        if ((R16(ICIS) & 3) == 2) { *resp = R32(ICII); return TRUE; }
        if (OsTimeMs() - t0 > 10) return FALSE;
        OsStallUs(1);
    }
}

/* consume RIRB entries up to the hardware write pointer; returns TRUE and the
   response when a solicited one from codec 'cad' was among them */
static BOOLEAN RirbTake(HDA_STATE *h, ULONG cad, ULONG *resp)
{
    USHORT rwp = (USHORT)(R16(RIRBWP) & 0xFF);
    if (rwp >= h->RirbEntries) return FALSE;
    while (h->RirbRp != rwp) {
        ULONG r, ex;
        h->RirbRp = (USHORT)((h->RirbRp + 1) % h->RirbEntries);
        r = h->Rirb[h->RirbRp * 2];
        ex = h->Rirb[h->RirbRp * 2 + 1];
        W8(RIRBSTS, 0x05);
        if (ex & 0x10) continue;                    /* unsolicited */
        if ((ex & 0x0F) != cad) continue;           /* late answer from another codec */
        if (resp) { *resp = r; return TRUE; }
    }
    return FALSE;
}

static BOOLEAN CmdCorb(HDA_STATE *h, ULONG cmd, ULONG *resp)
{
    ULONG t0, cad = cmd >> 28;
    USHORT wp;
    RirbTake(h, 0xFF, NULL);                        /* drop stale responses of timed-out commands */
    wp = (USHORT)((h->CorbWp + 1) % h->CorbEntries);
    h->Corb[wp] = cmd;
    MemoryBarrier();
    W16(CORBWP, wp);
    h->CorbWp = wp;
    t0 = OsTimeMs();
    for (;;) {
        if (RirbTake(h, cad, resp)) return TRUE;
        if (OsTimeMs() - t0 > 50) return RirbTake(h, cad, resp);
        OsStallUs(2);
    }
}

static ULONG SdCtlGet(HDA_STATE *h, ULONG sd);
static void SdCtlSet(HDA_STATE *h, ULONG sd, ULONG v);

static void StopCmdDma(HDA_STATE *h)
{
    ULONG i;
    W8(CORBCTL, 0); W8(RIRBCTL, 0);
    for (i = 0; i < 1000 && ((R8(CORBCTL) & 2) || (R8(RIRBCTL) & 2)); i++) OsStallUs(10);
}

#ifdef HDA_HOSTTEST
ULONG HostCmd(HDA_STATE *h, USHORT nid, ULONG verb, ULONG payload);
#define Cmd HostCmd
#else
static ULONG Cmd(HDA_STATE *h, USHORT nid, ULONG verb, ULONG payload)
{
    ULONG cmd, resp = 0xFFFFFFFF;
    if (verb >= 0x100) cmd = (h->Cad << 28) | ((ULONG)nid << 20) | (verb << 8) | (payload & 0xFF);
    else               cmd = (h->Cad << 28) | ((ULONG)nid << 20) | (verb << 16) | (payload & 0xFFFF);
    if (!h->Immediate) {
        if (CmdCorb(h, cmd, &resp)) {
            h->CorbOk = TRUE; h->CorbFail = 0;
            LOGT("hda: cmd %08x -> %08x", cmd, resp);
            return resp;
        }
        LOGT("hda: cmd %08x timeout corbwp %x corbrp %x rirbwp %x rirbsts %x corbctl %x rirbctl %x",
             cmd, R16(CORBWP), R16(CORBRP), R16(RIRBWP), R8(RIRBSTS), R8(CORBCTL), R8(RIRBCTL));
        if (h->Probing) return 0xFFFFFFFF;
        /* a codec may ignore a verb now and then; a dead CORB/RIRB never answers */
        if (++h->CorbFail >= (h->CorbOk ? 8UL : 2UL)) {
            LOGI("hda: CORB/RIRB not responding, switching to immediate commands");
            StopCmdDma(h);
            h->Immediate = TRUE;
        } else return 0xFFFFFFFF;
    }
    if (CmdImmediate(h, cmd, &resp)) { LOGT("hda: icmd %08x -> %08x", cmd, resp); return resp; }
    LOGT("hda: cmd %08x failed (icis %x)", cmd, R16(ICIS));
    return 0xFFFFFFFF;
}
#endif

static ULONG Param(HDA_STATE *h, USHORT nid, ULONG p) { return Cmd(h, nid, V_GET_PARAM, p); }

/* ------------------------------------------------------------------ */
/* controller bring-up                                                 */
/* ------------------------------------------------------------------ */
static BOOLEAN ControllerReset(HDA_STATE *h)
{
    ULONG i;
    StopCmdDma(h);
    if (R32(GCTL) & 1) W16(STATESTS, 0x7FFF);
    W32(GCTL, R32(GCTL) & ~1UL);
    for (i = 0; i < 1000 && (R32(GCTL) & 1); i++) OsStallUs(100);
    OsStallUs(1000);                    /* >= 100 us for the codec PLLs */
    W32(GCTL, R32(GCTL) | 1UL);
    for (i = 0; i < 1000 && !(R32(GCTL) & 1); i++) OsStallUs(100);
    if (!(R32(GCTL) & 1)) return FALSE;
    OsStallUs(1000);                    /* codecs need >= 521 us to request an address */
    for (i = 0; i < 100 && !R16(STATESTS); i++) OsStallUs(1000);
    return TRUE;
}

static ULONG RingSize(HDA_STATE *h, ULONG reg)
{
    UCHAR cap = R8(reg);
    /* size capability: bit 6 = 256, bit 5 = 16, bit 4 = 2 entries; a controller that
       reports nothing (ULi) wants 256 written explicitly */
    if ((cap & 0x40) || !(cap & 0x70)) { W8(reg, (cap & ~3) | 2); return 256; }
    if (cap & 0x20) { W8(reg, (cap & ~3) | 1); return 16; }
    W8(reg, cap & ~3);
    return 2;
}

static void RingSetup(HDA_STATE *h)
{
    ULONG i;
    StopCmdDma(h);
    /* CORB */
    h->CorbEntries = RingSize(h, CORBSIZE);
    W32(CORBLBASE, h->CorbPa); W32(CORBUBASE, 0);
    W16(CORBWP, 0);
    W16(CORBRP, 0x8000);
    for (i = 0; i < 1000 && !(R16(CORBRP) & 0x8000); i++) OsStallUs(1);   /* nVidia clears it itself */
    W16(CORBRP, 0);
    for (i = 0; i < 1000 && (R16(CORBRP) & 0x8000); i++) OsStallUs(1);
    h->CorbWp = 0;
    /* RIRB */
    h->RirbEntries = RingSize(h, RIRBSIZE);
    W32(RIRBLBASE, h->RirbPa); W32(RIRBUBASE, 0);
    W16(RIRBWP, 0x8000);
    /* one response per RINTFL (Creative CA0110 counts differently); the flag is cleared
       after every response, some controllers - and QEMU - stall the CORB until it is */
    W16(RINTCNT, (h->Flags & HF_CTX) ? 0xC0 : 1);
    h->RirbRp = 0;
    W8(RIRBSTS, 0x05);
    W8(CORBCTL, 0x02);
    W8(RIRBCTL, 0x03);                  /* DMA run + RINTFL status (CIE stays off: no interrupt) */
    for (i = 0; i < 1000 && !(R8(CORBCTL) & 2); i++) OsStallUs(1);
    h->CorbFail = 0;
}

/* Intel capability list in MMIO space (HDA 1.0a+): multi-link and processing pipe */
static void ParseCaps(CARD *c, HDA_STATE *h)
{
    ULONG off = R16(LLCH), n;
    for (n = 0; off && n < 10; n++) {
        ULONG hdr, id;
        if (off < 0x80 || off + 0x50 > c->MmioSize) break;
        hdr = R32(off);
        if (hdr == 0xFFFFFFFF) break;
        id = (hdr >> 16) & 0xFFF;
        if (id == 2 && !h->MlOff) h->MlOff = off;
        if (id == 3 && !h->PpOff) h->PpOff = off;
        off = hdr & 0xFFFF;
    }
    if (h->MlOff || h->PpOff) LOGV("hda: multi-link cap %x, processing pipe cap %x", h->MlOff, h->PpOff);
}

static BOOLEAN LinkPower(HDA_STATE *h, ULONG lctl, ULONG on)
{
    ULONG v = R32(lctl), i;
    W32(lctl, (v & ~(1UL << 16)) | (on << 16));                     /* SPA */
    for (i = 0; i < 50; i++) {
        if (((R32(lctl) >> 23) & 1) == on) return TRUE;             /* CPA */
        OsStallUs(10);
    }
    return FALSE;
}

/* Skylake+ multi-link: a link left at the 6 MHz bit clock (SCF = 0) needs the
   real HDA clock before codecs answer properly */
static void InitLinkClock(HDA_STATE *h)
{
    static const UCHAR pref[] = { 2, 3, 1, 4, 5 };
    ULONG lctl = h->MlOff + 0x40 + 0x04, lcap = h->MlOff + 0x40, v, i, scf = 0;
    v = R32(lctl);
    if (v == 0xFFFFFFFF || (v & 0xF)) return;
    if (((v >> 16) & 1) != ((v >> 23) & 1)) return;                /* SPA != CPA: in transition */
    for (i = 0; i < sizeof(pref); i++) if (R32(lcap) & (1UL << pref[i])) { scf = pref[i]; break; }
    if (LinkPower(h, lctl, 0)) {
        OsStallUs(100);
        W32(lctl, (R32(lctl) & ~0xFUL) | scf);
    }
    LinkPower(h, lctl, 1);
    OsStallUs(100);
    LOGI("hda: link clock select %u -> %u (lctl %08x)", 0, scf, R32(lctl));
}

/* reset the controller and (re)start command transport and position buffer */
static BOOLEAN BringUp(CARD *c, HDA_STATE *h)
{
    ULONG i, v;
    BOOLEAN ok;
    W32(INTCTL, 0);
    for (i = 0; i < h->Iss + h->Oss; i++) {
        ULONG sd = SD_BASE + i * 0x20;
        SdCtlSet(h, sd, SdCtlGet(h, sd) & ~(ULONG)(SDCTL_RUN | SDCTL_IOCE | SDCTL_FEIE | SDCTL_DEIE));
    }
    W32(DPLBASE, 0);
    if (h->PpOff && (R32(h->PpOff + 4) & (1UL << 30))) {
        LOGI("hda: audio DSP owns the streams (PPCTL %08x), returning them to HDA", R32(h->PpOff + 4));
        W32(h->PpOff + 4, R32(h->PpOff + 4) & ~((1UL << 30) | (1UL << 31)));
    }
    if (h->Flags & HF_SKL) {            /* misc BDF clock gating must be off around reset */
        v = PciRead32(c->Bus, c->Dev, c->Fn, 0x48);
        PciWrite32(c->Bus, c->Dev, c->Fn, 0x48, v & ~0x40UL);
    }
    ok = ControllerReset(h);
    if (h->Flags & HF_SKL) {
        v = PciRead32(c->Bus, c->Dev, c->Fn, 0x48);
        PciWrite32(c->Bus, c->Dev, c->Fn, 0x48, v | 0x40UL);
    }
    if (!ok) return FALSE;
    if (h->Flags & HF_APL) W32(0x1040, R32(0x1040) & (3UL << 20));   /* EM4L: earlier DMA requests */
    if (h->MlOff && c->VendorId == 0x8086) InitLinkClock(h);
    W32(INTSTS, 0xC0000000UL | 0x3FFFFFFFUL);
    if (!h->Immediate) RingSetup(h);
    W32(DPLBASE, h->PosPa | 1);
    W32(DPUBASE, 0);
    if (R32(SSYNC)) W32(SSYNC, 0);
    if ((h->Flags & HF_ICH) && R32(SSYNC_OLD)) W32(SSYNC_OLD, 0);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* widget graph                                                        */
/* ------------------------------------------------------------------ */
static HDA_WIDGET *Wg(HDA_STATE *h, USHORT nid)
{
    if (nid < h->StartNid || nid >= h->StartNid + h->NumWidgets) return NULL;
    return &h->W[nid - h->StartNid];
}

static void ReadConnList(HDA_STATE *h, HDA_WIDGET *w)
{
    ULONG len, lf, i, n = 0, per, shift, mask;
    USHORT prev = 0;
    w->NConn = 0;
    if (!(w->Caps & WC_CONNLIST)) return;
    len = Param(h, w->Nid, P_CONNLEN);
    if (len == 0xFFFFFFFF) return;
    lf = (len >> 7) & 1; len &= 0x7F;
    per = lf ? 2 : 4; shift = lf ? 16 : 8; mask = lf ? 0x7FFF : 0x7F;
    for (i = 0; i < len; i += per) {
        ULONG r = Cmd(h, w->Nid, V_GET_CONN_LIST, i), k;
        for (k = 0; k < per && i + k < len; k++) {
            ULONG e = (r >> (k * shift)) & ((1UL << shift) - 1);
            BOOLEAN range = (e & (lf ? 0x8000 : 0x80)) != 0;
            USHORT nid = (USHORT)(e & mask);
            if (range && prev) {
                USHORT x;
                for (x = (USHORT)(prev + 1); x <= nid && n < MAX_CONN; x++) w->Conn[n++] = x;
            } else if (n < MAX_CONN) w->Conn[n++] = nid;
            prev = nid;
        }
    }
    w->NConn = (UCHAR)n;
}

static ULONG AmpCaps(HDA_STATE *h, HDA_WIDGET *w, BOOLEAN out)
{
    if (w->Caps & WC_AMPOVR) return out ? w->OutAmp : w->InAmp;
    return out ? h->AfgOutAmp : h->AfgInAmp;
}

/* set amp: out/in, index, gain (absolute step), mute */
static void SetAmp(HDA_STATE *h, USHORT nid, BOOLEAN out, ULONG idx, ULONG gl, ULONG gr, BOOLEAN mute)
{
    ULONG base = (out ? 0x8000 : 0x4000) | ((idx & 0xF) << 8) | (mute ? 0x80 : 0);
    if (gl == gr) Cmd(h, nid, V_SET_AMP, base | 0x3000 | (gl & 0x7F));
    else {
        Cmd(h, nid, V_SET_AMP, base | 0x2000 | (gl & 0x7F));
        Cmd(h, nid, V_SET_AMP, base | 0x1000 | (gr & 0x7F));
    }
}

/* 0 dB step (offset) of an amp, clamped to the number of steps */
static ULONG ZeroDb(ULONG caps)
{
    ULONG off = caps & 0x7F, steps = (caps >> 8) & 0x7F;
    return off > steps ? steps : off;
}

/* volume slider -> amp step (squared-law dB curve, 0xFFFF = 0 dB) */
static ULONG VolStep(ULONG caps, USHORT v)
{
    ULONG off = ZeroDb(caps), size = ((caps >> 16) & 0x7F) + 1;   /* 0.25 dB units */
    ULONG db100 = VolToDb100(v), down;
    if (!v) return 0;
    down = (db100 * 4 / 100 + size / 2) / size;                    /* steps below 0 dB */
    return down >= off ? 0 : off - down;
}

static void UnmuteNode(HDA_STATE *h, HDA_WIDGET *w, LONG inIdx)
{
    ULONG ca;
    if (w->Caps & WC_OUTAMP) {
        ca = AmpCaps(h, w, TRUE);
        SetAmp(h, w->Nid, TRUE, 0, ZeroDb(ca), ZeroDb(ca), FALSE);
    }
    if ((w->Caps & WC_INAMP) && inIdx >= 0) {
        ca = AmpCaps(h, w, FALSE);
        SetAmp(h, w->Nid, FALSE, (ULONG)inIdx, ZeroDb(ca), ZeroDb(ca), FALSE);
        if (w->Type != W_MIX && inIdx != 0)
            SetAmp(h, w->Nid, FALSE, 0, ZeroDb(ca), ZeroDb(ca), FALSE);
    }
}

/* depth-first search from 'from' through connection lists for a node accepted by 'goal' */
typedef BOOLEAN (*GOALFN)(HDA_STATE *h, HDA_WIDGET *w, ULONG arg);

static BOOLEAN Dfs(HDA_STATE *h, USHORT nid, GOALFN goal, ULONG arg, HDA_PATH *p, ULONG depth,
                   ULONG limit)
{
    HDA_WIDGET *w = Wg(h, nid);
    ULONG i, k;
    if (!w || depth >= limit) return FALSE;
    for (k = 0; k < depth; k++) if (p->Nid[k] == nid) return FALSE;     /* cycle */
    p->Nid[depth] = nid;
    if (depth > 0 && goal(h, w, arg)) { p->N = (UCHAR)(depth + 1); return TRUE; }
    if (depth > 0 && (w->Type == W_PIN || w->Type == W_OUT || w->Type == W_IN)) return FALSE;
    for (i = 0; i < w->NConn; i++) {
        p->Idx[depth] = (UCHAR)i;
        if (Dfs(h, w->Conn[i], goal, arg, p, depth + 1, limit)) return TRUE;
    }
    return FALSE;
}

/* BFS-like: try increasing depth limits so the shortest path wins */
static BOOLEAN FindPath(HDA_STATE *h, USHORT from, GOALFN goal, ULONG arg, HDA_PATH *p)
{
    ULONG lim;
    for (lim = 2; lim <= MAX_PATH; lim++) {
        HDA_PATH t;
        memset(&t, 0, sizeof(t));
        if (Dfs(h, from, goal, arg, &t, 0, lim)) { *p = t; return TRUE; }
    }
    return FALSE;
}

static BOOLEAN GoalDac(HDA_STATE *h, HDA_WIDGET *w, ULONG preferred)
{
    (void)h;
    if (w->Type != W_OUT || (w->Caps & WC_DIGITAL)) return FALSE;
    return preferred == 0 || w->Nid == preferred;
}

static BOOLEAN GoalInPin(HDA_STATE *h, HDA_WIDGET *w, ULONG dev)
{
    (void)h;
    if (w->Type != W_PIN || !(w->PinCaps & PC_IN) || (w->Caps & WC_DIGITAL)) return FALSE;
    if (((w->Config >> 30) & 3) == 1) return FALSE;          /* not connected */
    return ((w->Config >> 20) & 0xF) == dev;
}

static void ActivatePath(HDA_STATE *h, HDA_PATH *p)
{
    ULONG i;
    for (i = 0; i < p->N; i++) {
        HDA_WIDGET *w = Wg(h, p->Nid[i]);
        if (!w) continue;
        Cmd(h, w->Nid, V_SET_POWER, 0);
        if (i + 1 < p->N && w->NConn > 1 && w->Type != W_MIX)
            Cmd(h, w->Nid, V_SET_CONN_SEL, p->Idx[i]);
        UnmuteNode(h, w, (i + 1 < p->N) ? (LONG)p->Idx[i] : -1);
    }
}

static const char *DevName(ULONG d)
{
    static const char *n[16] = { "line-out", "speaker", "headphone", "CD", "SPDIF-out", "digital-out",
        "modem-line", "modem-handset", "line-in", "aux", "mic", "telephony", "SPDIF-in",
        "digital-in", "reserved", "other" };
    return n[d & 15];
}

static void ScanWidgets(CARD *c, HDA_STATE *h)
{
    ULONG i;
    for (i = 0; i < h->NumWidgets; i++) {
        HDA_WIDGET *w = &h->W[i];
        w->Nid = (USHORT)(h->StartNid + i);
        w->Caps = Param(h, w->Nid, P_WCAPS);
        if (w->Caps == 0xFFFFFFFF) w->Caps = 0;
        w->Type = (UCHAR)((w->Caps >> 20) & 0xF);
        if (w->Caps & WC_AMPOVR) {
            w->InAmp = Param(h, w->Nid, P_INAMP);
            w->OutAmp = Param(h, w->Nid, P_OUTAMP);
        }
        if (w->Type == W_PIN) {
            w->PinCaps = Param(h, w->Nid, P_PINCAPS);
            w->Config = Cmd(h, w->Nid, V_GET_CONFIG, 0);
        }
        if (w->Type == W_OUT || w->Type == W_IN) w->Pcm = Param(h, w->Nid, P_PCM);
        ReadConnList(h, w);
        LOGT("hda: nid %02x type %u caps %08x pincaps %08x cfg %08x conn %u", w->Nid, w->Type,
             w->Caps, w->PinCaps, w->Config, w->NConn);
    }
    (void)c;
}

/* pin priority for outputs: line-out (assoc order) < speaker < headphone */
static BOOLEAN IsOutPin(HDA_WIDGET *w)
{
    ULONG dev = (w->Config >> 20) & 0xF, conn = (w->Config >> 30) & 3;
    if (w->Type != W_PIN || !(w->PinCaps & PC_OUT) || (w->Caps & WC_DIGITAL)) return FALSE;
    if (conn == 1) return FALSE;
    return dev == DEV_LINEOUT || dev == DEV_SPEAKER || dev == DEV_HP;
}

static void SetupOutputs(CARD *c, HDA_STATE *h)
{
    ULONG pass, i;
    USHORT firstDac = 0;
    h->NOut = 0;
    /* three passes: line-outs, speakers, headphones */
    for (pass = 0; pass < 3; pass++) {
        ULONG want = pass == 0 ? DEV_LINEOUT : pass == 1 ? DEV_SPEAKER : DEV_HP;
        for (i = 0; i < h->NumWidgets && h->NOut < MAX_OUTPINS; i++) {
            HDA_WIDGET *w = &h->W[i];
            HDA_PATH p;
            if (!IsOutPin(w) || ((w->Config >> 20) & 0xF) != want) continue;
            /* share the first DAC when reachable; otherwise any DAC */
            if (!(firstDac && FindPath(h, w->Nid, GoalDac, firstDac, &p)) &&
                !FindPath(h, w->Nid, GoalDac, 0, &p)) {
                LOGV("hda: no DAC path for pin %02x", w->Nid);
                continue;
            }
            if (!firstDac) firstDac = p.Nid[p.N - 1];
            h->OutPath[h->NOut] = p;
            h->OutDev[h->NOut] = (UCHAR)want;
            h->NOut++;
            LOGI("hda: %s pin %02x -> DAC %02x (%u hops)", DevName(want), w->Nid, p.Nid[p.N - 1], p.N - 1);
        }
    }
    /* no pin with a sane config default (some BIOSes leave them blank): take any output pin */
    if (!h->NOut) {
        for (i = 0; i < h->NumWidgets && h->NOut < MAX_OUTPINS; i++) {
            HDA_WIDGET *w = &h->W[i];
            HDA_PATH p;
            if (w->Type != W_PIN || !(w->PinCaps & PC_OUT) || (w->Caps & WC_DIGITAL)) continue;
            if (!FindPath(h, w->Nid, GoalDac, 0, &p)) continue;
            h->OutPath[h->NOut] = p;
            h->OutDev[h->NOut] = DEV_LINEOUT;
            h->NOut++;
            LOGI("hda: fallback pin %02x -> DAC %02x", w->Nid, p.Nid[p.N - 1]);
        }
    }
    for (i = 0; i < h->NOut; i++) {
        HDA_PATH *p = &h->OutPath[i];
        HDA_WIDGET *pin = Wg(h, p->Nid[0]);
        ULONG ctl = 0x40;
        ActivatePath(h, p);
        if (h->OutDev[i] == DEV_HP && (pin->PinCaps & PC_HPAMP)) ctl |= 0x80;
        Cmd(h, pin->Nid, V_SET_PIN_CTL, ctl);
        if (pin->PinCaps & PC_EAPD) Cmd(h, pin->Nid, V_SET_EAPD, 0x02);
        if (h->OutDev[i] == DEV_HP && (pin->PinCaps & PC_PRESENCE) && !(pin->Config & 0x100))
            h->JackPoll = TRUE;
    }
    (void)c;
}

/* loopback (analog monitor) inputs found on mixers of the output paths */
static void MapLoopPin(CARD *c, HDA_STATE *h, HDA_WIDGET *mx, ULONG idx, HDA_WIDGET *pin)
{
    static const struct { ULONG Dev; ULONG Line; } map[] = {
        { DEV_CD, LINE_CD }, { DEV_LINEIN, LINE_LINEIN }, { DEV_MIC, LINE_MIC }, { DEV_AUX, LINE_AUX } };
    ULONG m;
    if (!pin || pin->Type != W_PIN || !(pin->PinCaps & PC_IN) || ((pin->Config >> 30) & 3) == 1) return;
    for (m = 0; m < ARRAYSIZE(map); m++) {
        ULONG line = map[m].Line;
        if (((pin->Config >> 20) & 0xF) != map[m].Dev || h->Loop[line].Present) continue;
        h->Loop[line].Present = TRUE;
        h->Loop[line].MixNid = mx->Nid;
        h->Loop[line].Index = (UCHAR)idx;
        c->Lines[line].Present = TRUE;
        c->Lines[line].HwVolume = TRUE;
        Cmd(h, pin->Nid, V_SET_POWER, 0);
        LOGV("hda: loopback %s via mixer %02x[%u]", DevName(map[m].Dev), mx->Nid, idx);
    }
}

static void SetupLoopback(CARD *c, HDA_STATE *h)
{
    ULONG o, k, i, j;
    for (o = 0; o < h->NOut; o++) {
        HDA_PATH *p = &h->OutPath[o];
        for (k = 1; k < p->N; k++) {
            HDA_WIDGET *mx = Wg(h, p->Nid[k]);
            if (!mx || mx->Type != W_MIX) continue;
            for (i = 0; i < mx->NConn; i++) {
                HDA_WIDGET *src = Wg(h, mx->Conn[i]);
                if (!src || src->Nid == p->Nid[k + 1 < p->N ? k + 1 : k]) continue;
                if (src->Type == W_PIN) {
                    if (mx->Caps & WC_INAMP) MapLoopPin(c, h, mx, i, src);
                } else if (src->Type == W_MIX && (src->Caps & WC_INAMP)) {
                    /* dedicated loopback mixer (Realtek/VIA/IDT style): its inputs carry
                       the per-source volumes, the outer mixer input just has to be open */
                    ULONG before = 0, after = 0, l;
                    for (l = 0; l < LINE_COUNT; l++) before += h->Loop[l].Present;
                    for (j = 0; j < src->NConn; j++) MapLoopPin(c, h, src, j, Wg(h, src->Conn[j]));
                    for (l = 0; l < LINE_COUNT; l++) after += h->Loop[l].Present;
                    if (after > before) UnmuteNode(h, mx, (LONG)i);
                    if (after > before && (src->Caps & WC_OUTAMP)) UnmuteNode(h, src, -1);
                } else if (src->Type == W_SEL && src->NConn >= 1 && (mx->Caps & WC_INAMP)) {
                    MapLoopPin(c, h, mx, i, Wg(h, src->Conn[0]));
                }
            }
        }
    }
}

static void SetupInputs(CARD *c, HDA_STATE *h)
{
    static const struct { ULONG Dev; ULONG Line; } map[] = {
        { DEV_MIC, LINE_MIC }, { DEV_LINEIN, LINE_LINEIN }, { DEV_CD, LINE_CD }, { DEV_AUX, LINE_AUX } };
    ULONG i, m, best = 0, bestCount = 0;
    /* choose the ADC that reaches the most input pins */
    for (i = 0; i < h->NumWidgets; i++) {
        HDA_WIDGET *w = &h->W[i];
        ULONG cnt = 0;
        HDA_PATH p;
        if (w->Type != W_IN || (w->Caps & WC_DIGITAL)) continue;
        for (m = 0; m < ARRAYSIZE(map); m++)
            if (FindPath(h, w->Nid, GoalInPin, map[m].Dev, &p)) cnt++;
        if (cnt > bestCount) { bestCount = cnt; best = w->Nid; }
    }
    if (!best) { LOGI("hda: no usable ADC, recording disabled"); return; }
    h->Adc = (USHORT)best;
    for (m = 0; m < ARRAYSIZE(map); m++) {
        HDA_PATH *p = &h->InPath[map[m].Line];
        if (!FindPath(h, h->Adc, GoalInPin, map[m].Dev, p)) { p->N = 0; continue; }
        c->RecSrcMask |= 1UL << map[m].Line;
        {
            HDA_WIDGET *pin = Wg(h, p->Nid[p->N - 1]);
            ULONG vref = 0, vc = (pin->PinCaps >> 8) & 0xFF;
            if (map[m].Dev == DEV_MIC) vref = (vc & 0x10) ? 4 : (vc & 0x02) ? 1 : 0;
            Cmd(h, pin->Nid, V_SET_POWER, 0);
            Cmd(h, pin->Nid, V_SET_PIN_CTL, 0x20 | vref);
        }
        LOGI("hda: record source %s: ADC %02x <- pin %02x (%u hops)", DevName(map[m].Dev),
             h->Adc, p->Nid[p->N - 1], p->N - 1);
    }
    /* record gain: first amp on the ADC side */
    {
        HDA_WIDGET *a = Wg(h, h->Adc);
        if (a && (a->Caps & WC_INAMP) && ((AmpCaps(h, a, FALSE) >> 8) & 0x7F)) {
            h->RecAmpNid = a->Nid; h->RecAmpIsInput = TRUE;
        } else {
            for (m = 0; m < LINE_COUNT && !h->RecAmpNid; m++) {
                HDA_PATH *p = &h->InPath[m];
                ULONG k;
                for (k = 1; p->N && k < p->N && !h->RecAmpNid; k++) {
                    HDA_WIDGET *w = Wg(h, p->Nid[k]);
                    if (w && (w->Caps & WC_OUTAMP) && ((AmpCaps(h, w, TRUE) >> 8) & 0x7F) && w->Type != W_PIN) {
                        h->RecAmpNid = w->Nid; h->RecAmpIsInput = FALSE;
                    }
                }
            }
        }
    }
    c->Lines[LINE_RECORD].Present = TRUE;
    c->Lines[LINE_RECORD].HwVolume = h->RecAmpNid != 0;
    c->CanRecord = TRUE;
    c->RecSrc = (c->RecSrcMask & RECSRC_LINE) ? RECSRC_LINE :
                (c->RecSrcMask & RECSRC_MIC) ? RECSRC_MIC : (c->RecSrcMask & -(LONG)c->RecSrcMask);
}

/* ------------------------------------------------------------------ */
/* vendor codec initialisation                                         */
/* (what the Linux codec drivers do before/after the generic parser;   */
/*  without it many current Realtek codecs stay silent: analog PLL     */
/*  gated, EAPD under hardware control, headphone amp in low power,    */
/*  external speaker amplifier on a codec GPIO)                        */
/* ------------------------------------------------------------------ */
static void SleepMs(ULONG ms)
{
#ifndef HDA_HOSTTEST
    OsSleepMs(ms);
#else
    (void)ms;
#endif
}

static ULONG CoefReadEx(HDA_STATE *h, USHORT nid, ULONG idx)
{
    Cmd(h, nid, V_SET_COEF_IDX, idx);
    return Cmd(h, nid, V_GET_PROC_COEF, 0);
}

static void CoefWriteEx(HDA_STATE *h, USHORT nid, ULONG idx, ULONG val)
{
    Cmd(h, nid, V_SET_COEF_IDX, idx);
    Cmd(h, nid, V_SET_PROC_COEF, val & 0xFFFF);
}

static void CoefUpdateEx(HDA_STATE *h, USHORT nid, ULONG idx, ULONG mask, ULONG set)
{
    ULONG v = CoefReadEx(h, nid, idx);
    if (v != 0xFFFFFFFF) CoefWriteEx(h, nid, idx, (v & ~mask) | set);
}

#define CoefRead(h, i)            CoefReadEx(h, 0x20, i)
#define CoefWrite(h, i, v)        CoefWriteEx(h, 0x20, i, v)
#define CoefUpdate(h, i, m, s)    CoefUpdateEx(h, 0x20, i, m, s)

static ULONG Coef0(HDA_STATE *h)
{
    if (!h->Coef0Read) { h->Coef0 = CoefRead(h, 0); h->Coef0Read = TRUE; }
    return h->Coef0;
}

static BOOLEAN IdIn(ULONG id, const USHORT *list)
{
    for (; *list; list++) if ((id & 0xFFFF) == *list) return TRUE;
    return FALSE;
}

static void FixPll(HDA_STATE *h)
{
    if (h->PllNid) CoefUpdateEx(h, h->PllNid, h->PllIdx, 1UL << h->PllBit, 0);
}

static void SetPll(HDA_STATE *h, USHORT nid, UCHAR idx, UCHAR bit)
{
    h->PllNid = nid; h->PllIdx = idx; h->PllBit = bit;
    FixPll(h);
}

static BOOLEAN PinSense(HDA_STATE *h, USHORT nid)
{
    HDA_WIDGET *w = Wg(h, nid);
    ULONG r;
    if (!w || w->Type != W_PIN || !(w->PinCaps & PC_PRESENCE)) return FALSE;
    if (w->PinCaps & PC_TRIGGER) Cmd(h, nid, V_SET_SENSE, 0);
    r = Cmd(h, nid, V_GET_SENSE, 0);
    return r != 0xFFFFFFFF && (r & 0x80000000UL);
}

/* the headphone jack Linux would pick: first jack pin configured as headphone */
static USHORT HpPin(HDA_STATE *h)
{
    ULONG i;
    for (i = 0; i < h->NumWidgets; i++) {
        HDA_WIDGET *w = &h->W[i];
        if (w->Type == W_PIN && (w->PinCaps & PC_OUT) && ((w->Config >> 30) & 3) != 1 &&
            ((w->Config >> 20) & 0xF) == DEV_HP)
            return w->Nid;
    }
    return 0;
}

static void SetEapd(HDA_STATE *h, USHORT nid, BOOLEAN on)
{
    HDA_WIDGET *w = Wg(h, nid);
    if (!w || w->Type != W_PIN || !(w->PinCaps & PC_EAPD)) return;
    Cmd(h, nid, V_SET_EAPD, on ? 0x02 : 0x00);
}

/* Realtek: EAPD to verb control (Linux alc_fill_eapd_coef) */
static void AlcFillEapdCoef(HDA_STATE *h)
{
    static const USHORT g10_9[] = { 0x0230, 0x0233, 0x0235, 0x0236, 0x0245, 0x0255, 0x0256, 0x0257, 0x0282,
                                    0x0283, 0x0286, 0x0288, 0x0298, 0x0300, 0 };
    static const USHORT g10_15[] = { 0x0234, 0x0274, 0x0294, 0x0700, 0x0701, 0x0703, 0x0711, 0 };
    static const USHORT gd_14[] = { 0x0272, 0x0273, 0x0663, 0x0665, 0x0670, 0x0671, 0x0672, 0 };
    static const USHORT g7_1[] = { 0x0899, 0x0900, 0x0b00, 0x1168, 0x1220, 0 };
    ULONG id = h->CodecVendor & 0xFFFF, c0 = Coef0(h);
    switch (id) {
    case 0x0262: CoefUpdate(h, 0x7, 0, 1UL << 5); return;
    case 0x0267: case 0x0268: CoefUpdate(h, 0x7, 0, 1UL << 13); return;
    case 0x0269:
        if ((c0 & 0xF0) == 0x10) CoefUpdate(h, 0xD, 0, 1UL << 14);
        if ((c0 & 0xF0) == 0x20) CoefUpdate(h, 0x4, 1UL << 15, 0);
        if ((c0 & 0xF0) == 0x30) CoefUpdate(h, 0x10, 1UL << 9, 0);
        return;
    case 0x0280: case 0x0284: case 0x0290: case 0x0292: CoefUpdate(h, 0x4, 1UL << 15, 0); return;
    case 0x0225: case 0x0295: case 0x0299:
        CoefUpdate(h, 0x67, 0xF000, 0x3000);
        /* fall through */
    case 0x0215: case 0x0285: case 0x0289:
        CoefUpdate(h, 0x36, 1UL << 13, 0);
        CoefUpdate(h, 0x10, 1UL << 9, 0);
        return;
    case 0x0275: CoefUpdate(h, 0xE, 0, 1); return;
    case 0x0287: CoefUpdate(h, 0x10, 1UL << 9, 0); CoefWrite(h, 0x8, 0x4AB7); return;
    case 0x0293: CoefUpdate(h, 0xA, 1UL << 13, 0); return;
    case 0x0662: if ((c0 & 0xF0) == 0x30) CoefUpdate(h, 0x4, 1UL << 10, 0); return;
    case 0x0222: case 0x0623: CoefUpdate(h, 0x19, 1UL << 13, 0); return;
    case 0x0668: CoefUpdate(h, 0x7, 3UL << 13, 0); return;
    case 0x0867: CoefUpdate(h, 0x4, 1UL << 10, 0); return;
    case 0x0888: if ((c0 & 0xF0) == 0x20 || (c0 & 0xF0) == 0x30) CoefUpdate(h, 0x7, 1UL << 5, 0); return;
    case 0x0892: case 0x0897: CoefUpdate(h, 0x7, 1UL << 5, 0); return;
    }
    if (IdIn(id, g10_9)) CoefUpdate(h, 0x10, 1UL << 9, 0);
    else if (IdIn(id, g10_15)) CoefUpdate(h, 0x10, 1UL << 15, 0);
    else if (IdIn(id, gd_14)) CoefUpdate(h, 0xD, 0, 1UL << 14);
    else if (IdIn(id, g7_1)) CoefUpdate(h, 0x7, 1UL << 1, 0);
}

/* Realtek SKU word (codec SSID or pin 0x1d config): external amplifier on a GPIO */
static void AlcSku(CARD *c, HDA_STATE *h)
{
    ULONG ass = h->CodecSub & 0xFFFF, tmp, i, n;
    if (!(ass != c->SubDevice && (ass & 1))) {
        HDA_WIDGET *w = Wg(h, (USHORT)((h->CodecVendor & 0xFFFF) == 0x0260 ? 0x17 : 0x1D));
        if (!w) return;
        ass = w->Type == W_PIN ? w->Config : Cmd(h, w->Nid, V_GET_CONFIG, 0);
        if (ass == 0xFFFFFFFF || !(ass & 1) || (ass >> 30) != 1) return;
        for (i = 1, n = 0; i < 16; i++) if ((ass >> i) & 1) n++;
        if (((ass >> 16) & 0xF) != n) return;
    }
    tmp = (ass & 0x38) >> 3;
    switch (tmp) {
    case 1: h->GpioMask |= 1; break;
    case 3: h->GpioMask |= 2; break;
    case 7: h->GpioMask |= 4; break;
    default: h->InitAmp = 1; return;
    }
    h->GpioDir = h->GpioData = h->GpioMask;
    LOGI("hda: Realtek SKU %04x: external amplifier on GPIO mask %x", ass & 0xFFFF, h->GpioMask);
}

/* headphone amplifier bring-up of the ALC2xx laptop codecs (Linux init hooks) */
static void Alc256Init(HDA_STATE *h, USHORT hp)
{
    BOOLEAN s;
    SleepMs(30);
    s = PinSense(h, hp);
    if (s) SleepMs(2);
    CoefUpdateEx(h, 0x57, 0x04, 0x0007, 0x1);           /* low power */
    Cmd(h, hp, V_SET_AMP, 0xB080);                       /* out amp mute */
    if (s) SleepMs(85);
    Cmd(h, hp, V_SET_PIN_CTL, 0x40);
    if (s) SleepMs(100);
    CoefUpdate(h, 0x46, 3UL << 12, 0);
    CoefUpdateEx(h, 0x57, 0x04, 0x0007, 0x4);           /* high power */
    CoefUpdateEx(h, 0x53, 0x02, 0x8000, 0x8000);
    CoefUpdateEx(h, 0x53, 0x02, 0x8000, 0);
    CoefWrite(h, 0x36, 0x5757);                          /* no 1Ah loopback to the outputs */
}

static void Alc285HpInit(HDA_STATE *h, USHORT hp)
{
    ULONG c38, c0d, c36, v, i;
    CoefWriteEx(h, 0x58, 0x00, 0x1888);
    CoefUpdate(h, 0x4A, 1UL << 15, 1UL << 15);
    c38 = CoefRead(h, 0x38); c0d = CoefRead(h, 0x0D); c36 = CoefRead(h, 0x36);
    CoefUpdate(h, 0x38, 1UL << 4, 0);
    CoefUpdate(h, 0x0D, 0x110, 0);
    CoefUpdate(h, 0x67, 0xF000, 0x3000);
    Cmd(h, hp, V_SET_AMP, 0xB080);
    SleepMs(130);
    CoefUpdate(h, 0x36, 1UL << 14, 1UL << 14);
    CoefUpdate(h, 0x36, 1UL << 13, 0);
    Cmd(h, hp, V_SET_PIN_CTL, 0);
    SleepMs(10);
    CoefWrite(h, 0x67, 0);
    CoefWriteEx(h, 0x58, 0x00, 0x7880);
    CoefWriteEx(h, 0x58, 0x0F, 0xF049);
    CoefUpdateEx(h, 0x58, 0x03, 0x00F0, 0x00C0);
    CoefWriteEx(h, 0x58, 0x00, 0xF888);                  /* depop procedure */
    v = CoefReadEx(h, 0x58, 0x00);
    for (i = 0; i < 20 && v != 0xFFFFFFFF && (v & 0x8000); i++) { SleepMs(50); v = CoefReadEx(h, 0x58, 0x00); }
    if (v != 0xFFFFFFFF) CoefWriteEx(h, 0x58, 0x00, v);
    if (c38 != 0xFFFFFFFF) CoefUpdate(h, 0x38, 1UL << 4, c38 & (1UL << 4));
    if (c0d != 0xFFFFFFFF) CoefUpdate(h, 0x0D, 0x110, c0d & 0x110);
    if (c36 != 0xFFFFFFFF) CoefUpdate(h, 0x36, 3UL << 13, c36 & (3UL << 13));
    SleepMs(50);
    CoefUpdate(h, 0x4A, 1UL << 15, 0);
}

static void Alc225Init(HDA_STATE *h, USHORT hp, BOOLEAN hpinit)
{
    BOOLEAN s1, s2;
    if (hpinit) Alc285HpInit(h, hp);
    SleepMs(30);
    s1 = PinSense(h, hp);
    s2 = PinSense(h, 0x16);
    if (s1 || s2) SleepMs(2);
    CoefUpdateEx(h, 0x57, 0x04, 0x0007, 0x1);
    if (s1) Cmd(h, hp, V_SET_AMP, 0xB080);
    if (s2) Cmd(h, 0x16, V_SET_AMP, 0xB080);
    if (s1 || s2) SleepMs(85);
    if (s1) Cmd(h, hp, V_SET_PIN_CTL, 0x40);
    if (s2) Cmd(h, 0x16, V_SET_PIN_CTL, 0x40);
    if (s1 || s2) SleepMs(100);
    CoefUpdate(h, 0x4A, 3UL << 10, 0);
    CoefUpdateEx(h, 0x57, 0x04, 0x0007, 0x4);
}

static void Alc294HpInit(HDA_STATE *h, USHORT hp)
{
    ULONG v, i;
    Cmd(h, hp, V_SET_AMP, 0xB080);
    SleepMs(100);
    Cmd(h, hp, V_SET_PIN_CTL, 0);
    CoefUpdate(h, 0x6F, 0x000F, 0);
    CoefUpdateEx(h, 0x58, 0x00, 0x8000, 0x8000);
    v = CoefReadEx(h, 0x58, 0x01);
    for (i = 0; i < 20 && v != 0xFFFFFFFF && (v & 0x0080); i++) { SleepMs(50); v = CoefReadEx(h, 0x58, 0x01); }
    CoefUpdate(h, 0x6F, 0x000F, 0x000B);
    SleepMs(50);
}

/* before the output paths are built */
static void VendorPreInit(CARD *c, HDA_STATE *h)
{
    static const USHORT alc662[] = { 0x0272, 0x0273, 0x0662, 0x0663, 0x0665, 0x0667, 0x0668, 0x0670,
                                     0x0671, 0x0672, 0x0867, 0x0892, 0x0897, 0 };
    static const USHORT alc883[] = { 0x0883, 0x0887, 0x0888, 0x0889, 0x0899, 0x1168, 0 };
    static const USHORT a256[] = { 0x0230, 0x0235, 0x0236, 0x0255, 0x0256, 0x0257, 0 };
    static const USHORT a225[] = { 0x0215, 0x0225, 0x0245, 0x0285, 0x0287, 0x0289, 0x0295, 0x0299, 0 };
    static const USHORT a294[] = { 0x0234, 0x0274, 0x0294, 0x0700, 0x0701, 0x0703, 0x0711, 0 };
    ULONG ven = h->CodecVendor >> 16, id = h->CodecVendor & 0xFFFF;
    USHORT hp;
    HDA_WIDGET *v20 = Wg(h, 0x20);

    h->GpioMask = h->GpioDir = h->GpioData = 0;
    h->InitAmp = 0;
    h->PllNid = 0;
    h->Coef0Read = FALSE;
    if (ven == 0x10EC && !(v20 && v20->Type == 0xF)) {
        AlcSku(c, h);                            /* old codecs without the vendor widget (ALC260) */
        if (!h->GpioMask) h->InitAmp = 1;
    } else if (ven == 0x10EC) {
        LOGV("hda: Realtek coef0 %04x", Coef0(h));
        if (IdIn(id, alc662)) SetPll(h, 0x20, 0x04, 15);
        else if (IdIn(id, alc883)) SetPll(h, 0x20, 0x0A, 10);
        else if (id == 0x0269 && (Coef0(h) & 0xF0) != 0x10 && (Coef0(h) & 0xF0) != 0x20 &&
                 (Coef0(h) & 0xF0) != 0x30) SetPll(h, 0x20, 0x04, 15);
        if (id == 0x0269 && (Coef0(h) & 0xF0) == 0x10) {                 /* ALC269VB fill coef */
            ULONG r = Coef0(h) & 0xFF;
            if (r < 0x15) { CoefWrite(h, 0xF, 0x960B); CoefWrite(h, 0xE, 0x8817); }
            if (r == 0x16) { CoefWrite(h, 0xF, 0x960B); CoefWrite(h, 0xE, 0x8814); }
            if (r == 0x17) CoefUpdate(h, 0x04, 0, 1UL << 11);
            CoefUpdate(h, 0x4, 0, 1UL << 11);
        }
        if (id == 0x0234 || id == 0x0274 || id == 0x0294) CoefUpdate(h, 0x6B, 0x0018, 0x0018);   /* UAJ mic vref by verb */
        if (id >= 0x0700 && id <= 0x0711) CoefUpdate(h, 0x4A, 1UL << 15, 0);
        AlcFillEapdCoef(h);
        AlcSku(c, h);
        if (!h->GpioMask) h->InitAmp = 1;
        hp = HpPin(h);
        if (!hp) hp = 0x21;
        if (Wg(h, hp) && Wg(h, hp)->Type == W_PIN) {
            if (IdIn(id, a256)) Alc256Init(h, hp);
            else if (IdIn(id, a225)) {
                BOOLEAN v245 = (id == 0x0215 || id == 0x0245 || id == 0x0285 || id == 0x0289) && (Coef0(h) & 0x10);
                Alc225Init(h, hp, (BOOLEAN)(id != 0x0287 && !v245));
            }
            else if (IdIn(id, a294)) Alc294HpInit(h, hp);
            else if (id == 0x0283 || id == 0x0233) {
                CoefWrite(h, 0x43, 0x9004);              /* HP capless high power */
                Cmd(h, hp, V_SET_AMP, 0xB080);
                Cmd(h, hp, V_SET_PIN_CTL, 0x40);
                CoefUpdate(h, 0x46, 3UL << 12, 0);
                CoefWrite(h, 0x43, 0x9614);
            } else if (id == 0x0282) {
                ULONG c78 = CoefRead(h, 0x78);
                CoefWrite(h, 0x78, 0x9004);
                Cmd(h, hp, V_SET_AMP, 0xB080);
                Cmd(h, hp, V_SET_PIN_CTL, 0x40);
                if (c78 != 0xFFFFFFFF) CoefWrite(h, 0x78, c78);
            }
        }
    } else if ((ven == 0x111D || ven == 0x8384) && (Param(h, h->Afg, P_GPIO) & 0xFF)) {
        /* IDT/SigmaTel 92HD71/73, STAC927x/9205: GPIO0 is the EAPD line */
        if ((id & 0xFFF0) == 0x76B0 || (id & 0xFFF0) == 0x76C0 || (id & 0xFFF0) == 0x7670 ||
            (id & 0xFFF0) == 0x7620 || (id & 0xFFF0) == 0x76A0) {
            h->GpioMask = h->GpioDir = h->GpioData = 1;
        }
    }
}

/* after the output paths are set up (Linux alc_init: fix_pll + auto_init_amp) */
static void VendorPostInit(CARD *c, HDA_STATE *h)
{
    static const USHORT eapdPins[] = { 0x0F, 0x10, 0x14, 0x15, 0x17, 0 };
    ULONG ven = h->CodecVendor >> 16, id = h->CodecVendor & 0xFFFF, i;
    (void)c;
    if (ven == 0x10EC) {
        FixPll(h);
        for (i = 0; eapdPins[i]; i++) SetEapd(h, eapdPins[i], TRUE);
        if (h->InitAmp) {
            switch (id) {
            case 0x0260: CoefUpdateEx(h, 0x1A, 7, 0, 0x2010); break;
            case 0x0880: case 0x0882: case 0x0883: case 0x0885: CoefUpdate(h, 7, 0, 0x2030); break;
            case 0x0888: if ((Coef0(h) & 0xF0) == 0x00 || (Coef0(h) & 0xF0) == 0x10) CoefUpdate(h, 7, 0, 0x2030); break;
            }
        }
    } else if (ven == 0x14F1) {
        /* Conexant: every EAPD the codec has (cx_auto_turn_eapd) */
        for (i = 0; i < h->NumWidgets; i++)
            if (h->W[i].Type == W_PIN && (h->W[i].PinCaps & PC_EAPD)) SetEapd(h, h->W[i].Nid, TRUE);
    }
    if (h->GpioMask) {
        Cmd(h, h->Afg, V_SET_GPIO_MASK, h->GpioMask);
        Cmd(h, h->Afg, V_SET_GPIO_DIR, h->GpioDir);
        SleepMs(1);
        Cmd(h, h->Afg, V_SET_GPIO_DATA, h->GpioData);
        LOGI("hda: codec GPIO mask %x dir %x data %x", h->GpioMask, h->GpioDir, h->GpioData);
    }
}

/* every widget with its own power control to D0 (Linux snd_hda_codec_set_power_to_all) */
static void PowerAllWidgets(HDA_STATE *h)
{
    ULONG i;
    for (i = 0; i < h->NumWidgets; i++)
        if (h->W[i].Caps & WC_POWERCTL) Cmd(h, h->W[i].Nid, V_SET_POWER, 0);
    for (i = 0; i < 10; i++) {                   /* settle: AFG actual state D0, no error */
        ULONG ps = Cmd(h, h->Afg, V_GET_POWER, 0);
        if (ps == 0xFFFFFFFF || (!(ps & 0xF0) && !(ps & 0x100))) break;
        Cmd(h, h->Afg, V_SET_POWER, 0);
        SleepMs(1);
    }
}

/* ------------------------------------------------------------------ */
/* codec discovery                                                     */
/* ------------------------------------------------------------------ */
static const struct { USHORT Ven; const char *Name; } g_CodecVendors[] = {
    { 0x10EC, "Realtek" }, { 0x11D4, "Analog Devices" }, { 0x111D, "IDT" }, { 0x8384, "SigmaTel" },
    { 0x14F1, "Conexant" }, { 0x1106, "VIA" }, { 0x8086, "Intel" }, { 0x10DE, "NVIDIA" },
    { 0x1002, "ATI/AMD" }, { 0x1013, "Cirrus Logic" }, { 0x434D, "C-Media" }, { 0x1AEC, "Wolfson" },
    { 0x1057, "Motorola" }, { 0x11C1, "LSI" }, { 0x17E8, "Chrontel" }, { 0x1854, "LG" },
    { 0x1AF4, "QEMU" }, { 0x15AD, "VMware" }, { 0x6489, "Tempo" }, { 0x1102, "Creative" },
    { 0x13F6, "C-Media" }, { 0x19E5, "Huawei" }, { 0x1FA8, "Senary" }, { 0x1D17, "Zhaoxin" }, { 0, NULL } };

#include "hdanames.h"

/* Realtek parts sold under another name: by coefficient 0 bits, or (Dell, HP,
   Lenovo, Asus, Acer OEM numbers) by the subsystem vendor of the codec */
static const struct { ULONG Id; USHORT Mask, Bits; const char *Name; } g_AlcCoefNames[] = {
    { 0x10ec0221, 0xf00f, 0x1003, "ALC231" }, { 0x10ec0269, 0xfff0, 0x3010, "ALC277" },
    { 0x10ec0269, 0xf0f0, 0x2010, "ALC259" }, { 0x10ec0269, 0xf0f0, 0x3010, "ALC258" },
    { 0x10ec0269, 0x00f0, 0x0010, "ALC269VB" }, { 0x10ec0269, 0xffff, 0xa023, "ALC259" },
    { 0x10ec0269, 0xffff, 0x6023, "ALC281X" }, { 0x10ec0269, 0x00f0, 0x0020, "ALC269VC" },
    { 0x10ec0269, 0x00f0, 0x0030, "ALC269VD" }, { 0x10ec0662, 0xffff, 0x4020, "ALC656" },
    { 0x10ec0887, 0x00f0, 0x0030, "ALC887-VD" }, { 0x10ec0888, 0x00f0, 0x0030, "ALC888-VD" },
    { 0x10ec0888, 0xf0f0, 0x3020, "ALC886" }, { 0x10ec0899, 0x2000, 0x2000, "ALC899" },
    { 0x10ec0892, 0xffff, 0x8020, "ALC661" }, { 0x10ec0892, 0xffff, 0x8011, "ALC661" },
    { 0x10ec0892, 0xffff, 0x4011, "ALC656" }, { 0, 0, 0, NULL } };
static const struct { ULONG Id; USHORT SubVen; const char *Name; } g_AlcOemNames[] = {
    { 0x10ec0280, 0x1028, "ALC3220" }, { 0x10ec0282, 0x1028, "ALC3221" }, { 0x10ec0283, 0x1028, "ALC3223" },
    { 0x10ec0288, 0x1028, "ALC3263" }, { 0x10ec0292, 0x1028, "ALC3226" }, { 0x10ec0293, 0x1028, "ALC3235" },
    { 0x10ec0255, 0x1028, "ALC3234" }, { 0x10ec0668, 0x1028, "ALC3661" }, { 0x10ec0275, 0x1028, "ALC3260" },
    { 0x10ec0899, 0x1028, "ALC3861" }, { 0x10ec0298, 0x1028, "ALC3266" }, { 0x10ec0236, 0x1028, "ALC3204" },
    { 0x10ec0256, 0x1028, "ALC3246" }, { 0x10ec0225, 0x1028, "ALC3253" }, { 0x10ec0295, 0x1028, "ALC3254" },
    { 0x10ec0299, 0x1028, "ALC3271" }, { 0x10ec0670, 0x1025, "ALC669X" }, { 0x10ec0676, 0x1025, "ALC679X" },
    { 0x10ec0282, 0x1043, "ALC3229" }, { 0x10ec0233, 0x1043, "ALC3236" }, { 0x10ec0280, 0x103c, "ALC3228" },
    { 0x10ec0282, 0x103c, "ALC3227" }, { 0x10ec0286, 0x103c, "ALC3242" }, { 0x10ec0290, 0x103c, "ALC3241" },
    { 0x10ec0668, 0x103c, "ALC3662" }, { 0x10ec0283, 0x17aa, "ALC3239" }, { 0x10ec0292, 0x17aa, "ALC3232" },
    { 0, 0, NULL } };

/* "Realtek ALC1220", "Conexant CX20751-2", "IDT 92HD81B1X5", ... */
static void CodecName(HDA_STATE *h, char *buf, ULONG size)
{
    ULONG v = h->CodecVendor, i;
    const char *vn = NULL, *part = NULL;
    for (i = 0; g_CodecVendors[i].Ven; i++)
        if (g_CodecVendors[i].Ven == (v >> 16)) { vn = g_CodecVendors[i].Name; break; }
    if ((v >> 16) == 0x10EC) {
        ULONG c0 = Coef0(h);
        for (i = 0; g_AlcOemNames[i].Id && !part; i++)
            if (g_AlcOemNames[i].Id == v && g_AlcOemNames[i].SubVen == (h->CodecSub >> 16))
                part = g_AlcOemNames[i].Name;
        for (i = 0; g_AlcCoefNames[i].Id && !part; i++)
            if (g_AlcCoefNames[i].Id == v && (c0 & g_AlcCoefNames[i].Mask) == g_AlcCoefNames[i].Bits)
                part = g_AlcCoefNames[i].Name;
    }
    for (i = 0; g_CodecRevNames[i].Id && !part; i++)
        if (g_CodecRevNames[i].Id == v && g_CodecRevNames[i].Rev == (h->CodecRev & 0xFFFFFF))
            part = g_CodecRevNames[i].Name;
    for (i = 0; g_CodecNames[i].Id && !part; i++)
        if (g_CodecNames[i].Id == v) part = g_CodecNames[i].Name;
    if (part && vn) OsSprintf(buf, size, "%s %s", vn, part);
    else if (part) OsSprintf(buf, size, "%s", part);
    else if ((v >> 16) == 0x10EC) OsSprintf(buf, size, "Realtek ALC%x", v & 0xFFFF);
    else if (vn) OsSprintf(buf, size, "%s HDA codec %04x", vn, v & 0xFFFF);
    else OsSprintf(buf, size, "HDA codec %08x", v);
}

#ifdef HDA_HOSTTEST
BOOLEAN HostProbe(CARD *c, HDA_STATE *h, ULONG cad);
#endif
BOOLEAN ProbeCodecX(CARD *c, HDA_STATE *h, ULONG cad);
static BOOLEAN ProbeCodec(CARD *c, HDA_STATE *h, ULONG cad) { return ProbeCodecX(c, h, cad); }
BOOLEAN ProbeCodecX(CARD *c, HDA_STATE *h, ULONG cad)
{
    ULONG nodes, start, count, i, vendor;
    h->Cad = cad;
    vendor = Param(h, 0, P_VENDOR);
    if (vendor == 0xFFFFFFFF || vendor == 0) return FALSE;
    nodes = Param(h, 0, P_NODES);
    start = (nodes >> 16) & 0xFF; count = nodes & 0xFF;
    h->Afg = 0;
    for (i = 0; i < count; i++) {
        ULONG t = Param(h, (USHORT)(start + i), P_FGTYPE);
        if ((t & 0xFF) == 0x01) { h->Afg = (USHORT)(start + i); break; }
    }
    if (!h->Afg) { LOGV("hda: codec %u (%08x) has no audio function group", cad, vendor); return FALSE; }
    h->CodecVendor = vendor;
    h->CodecRev = Param(h, 0, P_REV);
    Cmd(h, h->Afg, V_SET_POWER, 0);              /* D0 */
    for (i = 0; i < 100; i++) {                  /* actual power state (bits 7:4) reaches D0 */
        ULONG ps = Cmd(h, h->Afg, V_GET_POWER, 0);
        if (ps == 0xFFFFFFFF || !(ps & 0xF0)) break;
        OsStallUs(1000);
    }
    h->CodecSub = Cmd(h, h->Afg, 0xF20, 0);
    h->AfgInAmp = Param(h, h->Afg, P_INAMP);
    h->AfgOutAmp = Param(h, h->Afg, P_OUTAMP);
    h->AfgPcm = Param(h, h->Afg, P_PCM);
    nodes = Param(h, h->Afg, P_NODES);
    h->StartNid = (USHORT)((nodes >> 16) & 0xFF);
    h->NumWidgets = nodes & 0xFF;
    if (h->NumWidgets > MAX_WIDGETS) h->NumWidgets = MAX_WIDGETS;
    if (!h->W) h->W = OsAlloc(sizeof(HDA_WIDGET) * MAX_WIDGETS);
    if (!h->W) return FALSE;
    memset(h->W, 0, sizeof(HDA_WIDGET) * MAX_WIDGETS);
    ScanWidgets(c, h);
    PowerAllWidgets(h);
    VendorPreInit(c, h);
    memset(h->OutPath, 0, sizeof(h->OutPath));
    SetupOutputs(c, h);
    if (!h->NOut) { LOGI("hda: codec %u (%08x): no analog output path", cad, vendor); return FALSE; }
    VendorPostInit(c, h);

    CodecName(h, c->Codec, sizeof(c->Codec));
    LOGI("hda: codec %u: %s (vendor %08x rev %08x sub %08x), AFG %02x, %u widgets from %02x",
         cad, c->Codec, vendor, h->CodecRev, h->CodecSub, h->Afg, h->NumWidgets, h->StartNid);
    return TRUE;
}

static void ApplyGpio(CARD *c, HDA_STATE *h)
{
    ULONG mask = OsRegGetDword(L"HdaGpioMask", 0);
    if (!mask) return;
    Cmd(h, h->Afg, V_SET_GPIO_MASK, mask);
    Cmd(h, h->Afg, V_SET_GPIO_DIR, OsRegGetDword(L"HdaGpioDir", mask));
    Cmd(h, h->Afg, V_SET_GPIO_DATA, OsRegGetDword(L"HdaGpioData", mask));
    LOGI("hda: GPIO mask %x applied", mask);
    (void)c;
}

/* ------------------------------------------------------------------ */
/* streams                                                             */
/* ------------------------------------------------------------------ */
#define FMT_48K_16_2 0x0011
#define OUT_TAG 1
#define IN_TAG  2

/* SDnCTL is 24 bits (+ SDnSTS in the top byte); a few controllers only take dword access */
static ULONG SdCtlGet(HDA_STATE *h, ULONG sd)
{
    if (h->SdDword) return R32(sd + SD_CTL) & 0xFFFFFF;
    return R8(sd + SD_CTL) | ((ULONG)R8(sd + SD_CTL + 2) << 16);
}

static void SdCtlSet(HDA_STATE *h, ULONG sd, ULONG v)
{
    if (h->SdDword) { W32(sd + SD_CTL, v & 0xFFFFFF); return; }
    W8(sd + SD_CTL + 2, v >> 16);
    W8(sd + SD_CTL, v & 0xFF);
}

static void SdReset(HDA_STATE *h, ULONG sd)
{
    ULONG i, v = SdCtlGet(h, sd);
    SdCtlSet(h, sd, v & ~(ULONG)(SDCTL_RUN | SDCTL_IOCE | SDCTL_FEIE | SDCTL_DEIE));
    for (i = 0; i < 1000 && (SdCtlGet(h, sd) & SDCTL_RUN); i++) OsStallUs(1);
    v = SdCtlGet(h, sd);
    SdCtlSet(h, sd, v | SDCTL_SRST);
    for (i = 0; i < 1000 && !(SdCtlGet(h, sd) & SDCTL_SRST); i++) OsStallUs(1);
    SdCtlSet(h, sd, SdCtlGet(h, sd) & ~(ULONG)SDCTL_SRST);
    for (i = 0; i < 1000 && (SdCtlGet(h, sd) & SDCTL_SRST); i++) OsStallUs(1);
    W8(sd + SD_STS, 0x1C);
}

static void SdStartEx(CARD *c, HDA_STATE *h, ULONG sd, ULONG sdIndex, PULONG bdl, ULONG bdlPa,
                      ULONG ringPa, ULONG tag, PULONG fifo, POSTRACK *t, ULONG ringBytes, USHORT fmt, BOOLEAN run)
{
    ULONG i, ctl, period = ringBytes / RING_PERIODS;
    SdReset(h, sd);
    for (i = 0; i < RING_PERIODS; i++) {          /* 2 KB periods (stereo): never cross a 4 KB page */
        bdl[i * 4 + 0] = ringPa + i * period;
        bdl[i * 4 + 1] = 0;
        bdl[i * 4 + 2] = period;
        bdl[i * 4 + 3] = c->UseInterrupt ? 1 : 0;
    }
    MemoryBarrier();
    W32(sd + SD_BDPL, bdlPa);
    W32(sd + SD_BDPU, 0);
    W32(sd + SD_CBL, ringBytes);
    W16(sd + SD_LVI, RING_PERIODS - 1);
    W16(sd + SD_FMT, fmt);
    ctl = (SdCtlGet(h, sd) & ~0xF00000UL) | (tag << 20);
    if (c->DmaUncached) ctl |= 1UL << 18;          /* traffic priority for non-snooped DMA */
    SdCtlSet(h, sd, ctl);
    for (i = 0; i < 100 && !R16(sd + SD_FIFOS); i++) OsStallUs(3);   /* set once FMT is valid */
    *fifo = (R16(sd + SD_FIFOS) & 0xFFFF) + 1;
    if (*fifo > 4096) *fifo = 256;
    if (c->UseInterrupt) W32(INTCTL, R32(INTCTL) | 0x80000000UL | (1UL << sdIndex));
    if (h->PosBuf) h->PosBuf[sdIndex * 2] = 0;
    memset(t, 0, sizeof(*t));
    t->LpibMs = t->PbMs = OsTimeMs();
    if (run) SdCtlSet(h, sd, ctl | SDCTL_RUN | (c->UseInterrupt ? (SDCTL_IOCE | SDCTL_FEIE | SDCTL_DEIE) : 0));
}

static void SdStart(CARD *c, HDA_STATE *h, ULONG sd, ULONG sdIndex, PULONG bdl, ULONG bdlPa,
                    ULONG ringPa, ULONG tag, PULONG fifo, POSTRACK *t)
{
    SdStartEx(c, h, sd, sdIndex, bdl, bdlPa, ringPa, tag, fifo, t, RING_BYTES, FMT_48K_16_2, TRUE);
}

static void SdStop(HDA_STATE *h, ULONG sd, ULONG sdIndex)
{
    W32(INTCTL, R32(INTCTL) & ~(1UL << sdIndex));
    SdReset(h, sd);
}

/* DMA position in frames.  Capture via LPIB is corrected by the FIFO: LPIB counts what
   came over the link, the last FIFO's worth may not be in memory yet. */
static ULONG SdPos(HDA_STATE *h, ULONG sd, ULONG sdIndex, POSTRACK *t, BOOLEAN capture)
{
    ULONG lpib = R32(sd + SD_LPIB), pb = h->PosBuf ? h->PosBuf[sdIndex * 2] : 0, now = OsTimeMs(), pos;
    if (lpib != t->LastLpib) { t->LastLpib = lpib; t->LpibMs = now; }
    if (pb != t->LastPb) { t->LastPb = pb; t->PbMs = now; }
    if (h->PosMode == POS_POSBUF && (pb >= RING_BYTES ||
        (now - t->PbMs > 120 && now - t->LpibMs < 30 && lpib < RING_BYTES))) {
        h->PosMode = POS_LPIB;
        LOGI("hda: DMA position buffer %s (%x), using LPIB", pb >= RING_BYTES ? "invalid" : "not updated", pb);
    } else if (h->PosMode == POS_LPIB && h->PosBuf && now - t->LpibMs > 120 && now - t->PbMs < 30 &&
               pb < RING_BYTES) {
        h->PosMode = POS_POSBUF;
        LOGI("hda: LPIB not advancing (%x), using the DMA position buffer", lpib);
    }
    if (h->PosMode == POS_POSBUF) pos = pb;
    else {
        pos = lpib % RING_BYTES;
        if (capture) {
            ULONG lag = (sd == h->InSdOff ? h->InFifo : h->OutFifo);
            if ((h->Flags & HF_FIFO) && lag < 128) lag = 128;          /* AMD: 32 frames */
            lag = (lag + 2 * ENG_FRAME_BYTES - 1) & ~(ENG_FRAME_BYTES - 1);
            pos = (pos + RING_BYTES - lag) % RING_BYTES;
        }
    }
    if (pos >= RING_BYTES) pos %= RING_BYTES;
    return pos / ENG_FRAME_BYTES;
}

static void ConfigConverters(HDA_STATE *h)
{
    ULONG i;
    for (i = 0; i < h->NOut; i++) {
        USHORT dac = h->OutPath[i].Nid[h->OutPath[i].N - 1];
        Cmd(h, dac, V_SET_FMT, FMT_48K_16_2);
        Cmd(h, dac, V_SET_CONV, (OUT_TAG << 4) | 0);
    }
    if (h->Adc) {
        Cmd(h, h->Adc, V_SET_FMT, FMT_48K_16_2);
        Cmd(h, h->Adc, V_SET_CONV, (IN_TAG << 4) | 0);
    }
}

/* ------------------------------------------------------------------ */
/* streams through the Smart Sound DSP: a decoupled host stream (the     */
/* second of its direction) feeding a DSP host gateway                   */
/* ------------------------------------------------------------------ */
static BOOLEAN DspStart(CARD *c, BOOLEAN rec)
{
    HDA_STATE *h = c->Hda;
    ULONG sd = rec ? SstRecSd(c) : SstPlaySd(c), off = SD_BASE + sd * 0x20;
    ULONG ch = rec ? SstRecChannels(c) : 2, tag = rec ? sd + 1 : sd - h->Iss + 1;
    DMA_BUFFER *ring = rec ? SstRecRing(c) : &c->PlayRing;
    PULONG bdl = (PULONG)((PUCHAR)h->DspBdl.Va + (rec ? 256 : 0));
    ULONG bdlPa = h->DspBdl.Pa + (rec ? 256 : 0);
    if (!h->DspBdl.Va && !OsAllocDma(&h->DspBdl, 4096, c->DmaUncached)) return FALSE;
    bdl = (PULONG)((PUCHAR)h->DspBdl.Va + (rec ? 256 : 0));
    bdlPa = h->DspBdl.Pa + (rec ? 256 : 0);
    SdStartEx(c, h, off, sd, bdl, bdlPa, ring->Pa, tag, rec ? &h->InFifo : &h->OutFifo, rec ? &h->RecTrk : &h->PlayTrk,
              RING_FRAMES * ch * 2, (USHORT)(0x0010 | (ch - 1)), FALSE);
    if (rec) { h->DspRecSd = sd; h->DspRecCh = ch; h->DspRecLast = 0; }
    else h->DspPlaySd = sd;
    SdCtlSet(h, off, SdCtlGet(h, off) | SDCTL_RUN);
    if (!SstStart(c, rec)) {
        SdStop(h, off, sd);
        LOGE("hda: DSP %s path did not start", rec ? "capture" : "playback");
        return FALSE;
    }
    return TRUE;
}

static void DspStop(CARD *c, BOOLEAN rec)
{
    HDA_STATE *h = c->Hda;
    ULONG sd = rec ? h->DspRecSd : h->DspPlaySd;
    SstStop(c, rec);
    SdStop(h, SD_BASE + sd * 0x20, sd);
}

/* host DMA position of a decoupled stream: the DPIB register (Intel vendor space),
   the position buffer when that does not move */
static ULONG DspPos(CARD *c, BOOLEAN rec)
{
    HDA_STATE *h = c->Hda;
    ULONG sd = rec ? h->DspRecSd : h->DspPlaySd, ch = rec ? h->DspRecCh : 2;
    ULONG bytes = RING_FRAMES * ch * 2, pos = R32(0x1084 + 0x20 * sd);
    if (pos >= bytes || (pos == 0 && h->PosBuf && h->PosBuf[sd * 2] && h->PosBuf[sd * 2] < bytes)) pos = h->PosBuf ? h->PosBuf[sd * 2] : 0;
    if (pos >= bytes) pos %= bytes;
    return pos / (ch * 2);
}

static BOOLEAN HdaStartPlay(CARD *c)
{
    HDA_STATE *h = c->Hda;
    if (SstPlayActive(c)) { h->DspPlay = DspStart(c, FALSE); return h->DspPlay; }
    ConfigConverters(h);
    SdStart(c, h, h->OutSdOff, h->OutSd, h->BdlOut, h->BdlOutPa, c->PlayRing.Pa, OUT_TAG, &h->OutFifo, &h->PlayTrk);
    SideAmpPower(c, TRUE);                        /* I2S speaker amplifiers on the codec (worker thread) */
    return TRUE;
}

static void HdaStopPlay(CARD *c)
{
    if (c->Hda->DspPlay) { DspStop(c, FALSE); c->Hda->DspPlay = FALSE; return; }
    SdStop(c->Hda, c->Hda->OutSdOff, c->Hda->OutSd);
    SideAmpPower(c, FALSE);
}

static ULONG HdaPlayPos(CARD *c)
{
    HDA_STATE *h = c->Hda;
    if (h->DspPlay) return DspPos(c, FALSE);
    return SdPos(h, h->OutSdOff, h->OutSd, &h->PlayTrk, FALSE);
}

static BOOLEAN HdaStartRec(CARD *c)
{
    HDA_STATE *h = c->Hda;
    if (SstRecActive(c)) { h->DspRec = DspStart(c, TRUE); return h->DspRec; }
    if (!h->Adc) return FALSE;
    ConfigConverters(h);
    SdStart(c, h, h->InSdOff, h->InSd, h->BdlIn, h->BdlInPa, c->RecRing.Pa, IN_TAG, &h->InFifo, &h->RecTrk);
    return TRUE;
}

static void HdaStopRec(CARD *c)
{
    if (c->Hda->DspRec) { DspStop(c, TRUE); c->Hda->DspRec = FALSE; return; }
    SdStop(c->Hda, c->Hda->InSdOff, c->Hda->InSd);
}

static ULONG HdaRecPos(CARD *c)
{
    HDA_STATE *h = c->Hda;
    if (h->DspRec) {
        ULONG pos = DspPos(c, TRUE);
        if (h->DspRecCh > 2) SstRecCopy(c, pos);
        return pos;
    }
    return SdPos(h, h->InSdOff, h->InSd, &h->RecTrk, TRUE);
}

/* headphone auto-mute (polled from the play tick) */
static void JackPoll(CARD *c)
{
    HDA_STATE *h = c->Hda;
    ULONG now = OsTimeMs(), i;
    BOOLEAN hp = FALSE;
    if (!h->JackPoll || now - h->LastJackMs < 500) return;
    h->LastJackMs = now;
    for (i = 0; i < h->NOut; i++) {
        HDA_WIDGET *pin = Wg(h, h->OutPath[i].Nid[0]);
        if (h->OutDev[i] != DEV_HP || !pin) continue;
        if (pin->PinCaps & PC_TRIGGER) Cmd(h, pin->Nid, V_SET_SENSE, 0);    /* start impedance sense */
        if (Cmd(h, pin->Nid, V_GET_SENSE, 0) & 0x80000000UL) hp = TRUE;
    }
    if (hp == h->HpPresent) return;
    h->HpPresent = hp;
    for (i = 0; i < h->NOut; i++)
        if (h->OutDev[i] == DEV_SPEAKER)
            Cmd(h, h->OutPath[i].Nid[0], V_SET_PIN_CTL, hp ? 0x00 : 0x40);
    LOGV("hda: headphone %s", hp ? "plugged, speakers muted" : "unplugged, speakers on");
}

static void HdaPlayTick(CARD *c) { JackPoll(c); }

static BOOLEAN HdaIsr(CARD *c)
{
    HDA_STATE *h = c->Hda;
    ULONG sts = R32(INTSTS), i;
    if (sts == 0 || sts == 0xFFFFFFFF) return FALSE;
    /* only sources we enabled count: CIS (RIRB status) is latched but not enabled */
    if (!(sts & R32(INTCTL) & 0x7FFFFFFFUL)) return FALSE;
    for (i = 0; i < 30; i++) {
        if (sts & (1UL << i)) {
            ULONG sd = SD_BASE + i * 0x20;
            W8(sd + SD_STS, R8(sd + SD_STS) & 0x1C);
        }
    }
    if (sts & 0x40000000UL) {
        W8(RIRBSTS, 0x05);
        W8(CORBSTS, 0x01);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* mixer lines                                                         */
/* ------------------------------------------------------------------ */
static void HdaApplyLine(CARD *c, ULONG line)
{
    HDA_STATE *h = c->Hda;
    LINE_STATE *l = &c->Lines[line];
    if (line == LINE_RECORD) {
        HDA_WIDGET *w = Wg(h, h->RecAmpNid);
        ULONG caps;
        if (!w) return;
        caps = AmpCaps(h, w, !h->RecAmpIsInput);
        {
            /* record gain: map the whole slider onto the amp range, 0xC000 ~ 0 dB */
            ULONG steps = (caps >> 8) & 0x7F;
            ULONG gl = ((ULONG)l->Left * steps + 32767) / 65535;
            ULONG gr = ((ULONG)l->Right * steps + 32767) / 65535;
            if (h->RecAmpIsInput) {
                ULONG idx = 0, k;
                for (k = 0; k < LINE_COUNT; k++)
                    if ((c->RecSrc & (1UL << k)) && h->InPath[k].N) idx = h->InPath[k].Idx[0];
                SetAmp(h, w->Nid, FALSE, idx, gl, gr, l->Mute);
                if (idx) SetAmp(h, w->Nid, FALSE, 0, gl, gr, l->Mute);
            } else SetAmp(h, w->Nid, TRUE, 0, gl, gr, l->Mute);
        }
        return;
    }
    if (line < LINE_COUNT && h->Loop[line].Present) {
        HDA_WIDGET *mx = Wg(h, h->Loop[line].MixNid);
        ULONG caps = AmpCaps(h, mx, FALSE);
        SetAmp(h, mx->Nid, FALSE, h->Loop[line].Index, VolStep(caps, l->Left), VolStep(caps, l->Right),
               (BOOLEAN)(l->Mute || (!l->Left && !l->Right)));
    }
}

static void HdaApplyRecSrc(CARD *c)
{
    HDA_STATE *h = c->Hda;
    ULONG k;
    if (SstRecActive(c)) { LOGV("hda: record source -> DSP endpoint"); return; }
    for (k = 0; k < LINE_COUNT; k++) {
        if (!(c->RecSrc & (1UL << k)) || !h->InPath[k].N) continue;
        ActivatePath(h, &h->InPath[k]);
        /* mute the other inputs of mixers on the ADC side */
        {
            HDA_WIDGET *a = Wg(h, h->InPath[k].Nid[0]);
            if (a && a->Type == W_IN && a->NConn > 1 && (a->Caps & WC_INAMP)) {
                ULONG i;
                for (i = 0; i < a->NConn; i++)
                    if (i != h->InPath[k].Idx[0]) SetAmp(h, a->Nid, FALSE, i, 0, 0, TRUE);
            }
        }
        LOGV("hda: record source -> line %u", k);
    }
    HdaApplyLine(c, LINE_RECORD);
}

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */
static ULONG HdaDump(CARD *c, char *buf, ULONG size)
{
    HDA_STATE *h = c->Hda;
    ULONG n = 0, i, k;
    static const char *tn[16] = { "DAC", "ADC", "Mixer", "Selector", "Pin", "Power", "Knob", "Beep",
                                  "?", "?", "?", "?", "?", "?", "?", "Vendor" };
    EngineLock(c);
    n += OsSprintf(buf + n, size - n, "%s [%04x:%04x sub %04x:%04x]\r\nGCAP %04x ISS %u OSS %u %s %s%s flags %x fifo %u/%u\r\n",
                   c->Desc, c->VendorId, c->DeviceId, c->SubVendor, c->SubDevice, h->Gcap, h->Iss, h->Oss,
                   h->Immediate ? "immediate-cmd" : "corb/rirb", h->PosMode == POS_POSBUF ? "posbuf" : "lpib",
                   c->DmaUncached ? " uncached" : "", h->Flags, h->OutFifo, h->InFifo);
    n += OsSprintf(buf + n, size - n, "Codec mask %x, using %u: %s vendor %08x rev %08x sub %08x AFG %02x\r\n",
                   h->CodecMask, h->Cad, c->Codec, h->CodecVendor, h->CodecRev, h->CodecSub, h->Afg);
    n += OsSprintf(buf + n, size - n, "AFG amp in %08x out %08x pcm %08x\r\n", h->AfgInAmp, h->AfgOutAmp, h->AfgPcm);
    for (i = 0; i < h->NumWidgets && n + 200 < size; i++) {
        HDA_WIDGET *w = &h->W[i];
        n += OsSprintf(buf + n, size - n, "NID %02x %-8s caps %08x", w->Nid, tn[w->Type & 15], w->Caps);
        if (w->Type == W_PIN)
            n += OsSprintf(buf + n, size - n, " pincaps %08x cfg %08x (%s) ctl %02x", w->PinCaps, w->Config,
                           DevName(w->Config >> 20), Cmd(h, w->Nid, V_GET_PIN_CTL, 0) & 0xFF);
        if (w->Caps & WC_AMPOVR) n += OsSprintf(buf + n, size - n, " amp in %08x out %08x", w->InAmp, w->OutAmp);
        if (w->NConn) {
            n += OsSprintf(buf + n, size - n, " conn[%u]:", Cmd(h, w->Nid, V_GET_CONN_SEL, 0) & 0xFF);
            for (k = 0; k < w->NConn && n + 8 < size; k++) n += OsSprintf(buf + n, size - n, " %02x", w->Conn[k]);
        }
        n += OsSprintf(buf + n, size - n, "\r\n");
    }
    for (i = 0; i < h->NOut && n + 100 < size; i++) {
        n += OsSprintf(buf + n, size - n, "out %s:", DevName(h->OutDev[i]));
        for (k = 0; k < h->OutPath[i].N; k++) n += OsSprintf(buf + n, size - n, " %02x", h->OutPath[i].Nid[k]);
        n += OsSprintf(buf + n, size - n, "\r\n");
    }
    for (i = 0; i < LINE_COUNT && n + 100 < size; i++) if (h->InPath[i].N) {
        n += OsSprintf(buf + n, size - n, "in line%u:", i);
        for (k = 0; k < h->InPath[i].N; k++) n += OsSprintf(buf + n, size - n, " %02x", h->InPath[i].Nid[k]);
        n += OsSprintf(buf + n, size - n, "\r\n");
    }
    EngineUnlock(c);
    n += SstDump(c, buf + n, size - n);
    n += SideAmpDump(c, buf + n, size - n);
    return n;
}

/* ------------------------------------------------------------------ */
/* init / shutdown                                                     */
/* ------------------------------------------------------------------ */
/* phase 1 of codec discovery (as Linux): every address that answers STATESTS must answer
   a vendor-id query; a phantom codec is dropped and the controller reset, because
   talking to an empty slot leaves some controllers confused */
static ULONG ProbeAddresses(CARD *c, HDA_STATE *h, ULONG mask)
{
    ULONG cad, ok = 0;
    for (cad = 0; cad < 15; cad++) {
        ULONG v;
        if (!(mask & (1UL << cad))) continue;
        h->Cad = cad;
        h->Probing = TRUE;
        v = Param(h, 0, P_VENDOR);
        h->Probing = FALSE;
        if (v != 0xFFFFFFFF && v != 0) { ok |= 1UL << cad; continue; }
        LOGI("hda: codec address %u does not answer, ignoring it", cad);
        BringUp(c, h);
    }
    return ok;
}

static NTSTATUS HdaInit(CARD *c)
{
    HDA_STATE *h;
    ULONG i, bar = c->Bar[0], cad, mask, forced;
    PUCHAR aux;

    h = c->Hda = OsAlloc(sizeof(HDA_STATE));
    if (!h) return STATUS_INSUFFICIENT_RESOURCES;
    h->Flags = c->HwFlags;
    if ((bar & 1) || !(bar & ~0xFUL)) { LOGE("hda: BAR0 %08x not usable", bar); return STATUS_DEVICE_DOES_NOT_EXIST; }
    if ((bar & 6) == 4 && c->Bar[1]) { LOGE("hda: BAR0 above 4 GB, unsupported"); return STATUS_DEVICE_DOES_NOT_EXIST; }
    c->MmioSize = 0x4000;
    c->Mmio = h->Mmio = OsMapMmio(bar & ~0xFUL, c->MmioSize);
    if (!h->Mmio) return STATUS_INSUFFICIENT_RESOURCES;
    if (R32(GCAP) == 0xFFFFFFFF) { LOGE("hda: registers read all ones (powered off?)"); return STATUS_DEVICE_DOES_NOT_EXIST; }
    if ((PciRead32(c->Bus, c->Dev, c->Fn, 8) >> 16) != 0x0403 && (R8(0x03) != 1 || R8(0x02) != 0)) {
        /* not an HD Audio register set: e.g. an Intel SST/LPE audio DSP with I2S codecs */
        LOGE("hda: %04x:%04x is not an HD Audio controller (version %u.%u) - an audio DSP with I2S/SoundWire "
             "codecs needs DSP firmware", c->VendorId, c->DeviceId, R8(0x03), R8(0x02));
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    PciTweaks(c, h);
    h->SdDword = (h->Flags & HF_SDDWORD) != 0;
    h->PosMode = (h->Flags & (HF_LPIB | HF_FIFO)) ? POS_LPIB : POS_POSBUF;
    i = OsRegGetDword(L"HdaPosBuf", 2);          /* 0 = LPIB, 1 = position buffer, 2 = auto */
    if (i < 2) h->PosMode = i ? POS_POSBUF : POS_LPIB;

    h->Gcap = R16(GCAP);
    h->Oss = (h->Gcap >> 12) & 0xF;
    h->Iss = (h->Gcap >> 8) & 0xF;
    if (!h->Oss && !h->Iss) {                     /* GCAP not implemented */
        if (h->Flags & HF_ULI) { h->Oss = 6; h->Iss = 5; }
        else { h->Oss = 4; h->Iss = 4; }
        LOGI("hda: GCAP reads 0, assuming %u output / %u input streams", h->Oss, h->Iss);
    }
    if (!h->Oss) { LOGE("hda: no output streams (GCAP %04x)", h->Gcap); return STATUS_DEVICE_DOES_NOT_EXIST; }
    h->InSd = 0;
    h->OutSd = h->Iss;
    h->InSdOff = SD_BASE + h->InSd * 0x20;
    h->OutSdOff = SD_BASE + h->OutSd * 0x20;
    h->OutFifo = h->InFifo = 256;

    /* aux DMA page: CORB 1K @0, RIRB 2K @1K, BDL out @3K, BDL in @3.25K, posbuf @3.5K */
    if (!OsAllocDma(&c->Aux, 4096, c->DmaUncached)) return STATUS_INSUFFICIENT_RESOURCES;
    aux = c->Aux.Va;
    h->Corb = (PULONG)aux;            h->CorbPa = c->Aux.Pa;
    h->Rirb = (PULONG)(aux + 1024);   h->RirbPa = c->Aux.Pa + 1024;
    h->BdlOut = (PULONG)(aux + 3072); h->BdlOutPa = c->Aux.Pa + 3072;
    h->BdlIn = (PULONG)(aux + 3328);  h->BdlInPa = c->Aux.Pa + 3328;
    h->PosBuf = (PULONG)(aux + 3584); h->PosPa = c->Aux.Pa + 3584;

    if ((h->Flags & HF_SKL) || (c->VendorId == 0x8086 && !(h->Flags & HF_ICH))) ParseCaps(c, h);
    /* an Intel controller newer than the table with the multi-link capability is Skylake+ */
    if (c->VendorId == 0x8086 && h->MlOff && !(h->Flags & HF_SKL)) h->Flags |= HF_SKL;
    /* stop everything the BIOS may have running */
    W32(INTCTL, 0);
    for (i = 0; i < h->Iss + h->Oss; i++) SdReset(h, SD_BASE + i * 0x20);
    i = OsRegGetDword(L"HdaImmediate", 2);       /* 0 = CORB/RIRB, 1 = immediate, 2 = auto */
    h->Immediate = i == 2 ? (h->Flags & HF_PIO) != 0 : i != 0;

    if (!BringUp(c, h)) { LOGE("hda: controller reset failed"); return STATUS_DEVICE_DOES_NOT_EXIST; }
    h->CodecMask = R16(STATESTS) & 0x7FFF;
    W16(STATESTS, 0x7FFF);
    LOGI("hda: GCAP %04x (ISS %u OSS %u), version %u.%u, codecs %x, %s commands", h->Gcap, h->Iss, h->Oss,
         R8(0x03), R8(0x02), h->CodecMask, h->Immediate ? "immediate" : "CORB/RIRB");
    forced = OsRegGetDword(L"HdaCodec", 0xFF);
    mask = forced < 15 ? (1UL << forced) : h->CodecMask;
    for (i = 0; g_ProbeMask[i].Ven; i++)
        if (g_ProbeMask[i].Ven == c->SubVendor && g_ProbeMask[i].Dev == c->SubDevice && forced >= 15) {
            ULONG pm = g_ProbeMask[i].Mask;
            mask = (pm & 0x100) ? (pm & 0xFF) : (mask & pm);
            LOGI("hda: board quirk, codec probe mask %x", mask);
        }
    i = OsRegGetDword(L"HdaProbeMask", 0) & 0x7FFF;   /* like Linux probe_mask */
    if (i) mask = i;
    if (!mask) mask = 1;                         /* some controllers don't latch STATESTS */
    {
        ULONG alive = ProbeAddresses(c, h, mask);
        if (!alive && !h->Immediate && !h->CorbOk) {
            /* nothing came back through the CORB/RIRB at all: try immediate commands */
            LOGI("hda: no response through CORB/RIRB, retrying with immediate commands");
            StopCmdDma(h);
            h->Immediate = TRUE;
            BringUp(c, h);
            alive = ProbeAddresses(c, h, mask);
        }
        mask = alive;
    }
    {
        BOOLEAN found = FALSE;
        for (cad = 0; cad < 15 && !found; cad++)
            if (mask & (1UL << cad)) found = ProbeCodec(c, h, cad);
        h->HaveCodec = found;
        if (!found) LOGI("hda: no HD Audio codec with an analog output%s", mask ? "" : " (no codec answers)");
    }
    if (h->HaveCodec) {
        ApplyGpio(c, h);
        c->Lines[LINE_MASTER].Present = TRUE;
        c->Lines[LINE_MASTER].HwVolume = FALSE;
        SetupLoopback(c, h);
        if (h->Iss) SetupInputs(c, h);
        ConfigConverters(h);
    }
    /* Intel Smart Sound: digital microphones, I2S and SoundWire through the DSP */
    if (SstProbe(c, h->Mmio, h->PpOff, h->MlOff, h->Iss, h->Oss, h->HaveCodec)) {
        if (!h->HaveCodec) {
            OsSprintf(c->Codec, sizeof(c->Codec), "Smart Sound DSP");
            c->Lines[LINE_MASTER].Present = TRUE;
        }
        if (c->RecSrc == 0 || !(c->RecSrc & c->RecSrcMask)) {
            ULONG k;
            for (k = 0; k < LINE_COUNT; k++) if (c->RecSrcMask & (1UL << k)) { c->RecSrc = 1UL << k; break; }
        }
    } else if (!h->HaveCodec) {
        LOGE("hda: no codec with an analog output");
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    /* speaker amplifiers the codec feeds over I2S, controlled over I2C / SPI (CS35L41, CS35L56) */
    if (h->HaveCodec) SideAmpProbe(c, h->CodecVendor, h->CodecSub);
    return STATUS_SUCCESS;
}

static void HdaShutdown(CARD *c)
{
    HDA_STATE *h = c->Hda;
    ULONG i;
    if (!h || !h->Mmio) return;
    SideAmpShutdown(c);                           /* before SstShutdown: the I2C / GPIO mappings are shared */
    SstShutdown(c);
    if (h->DspBdl.Va) OsFreeDma(&h->DspBdl);
    W32(INTCTL, 0);
    for (i = 0; i < h->Iss + h->Oss; i++) SdReset(h, SD_BASE + i * 0x20);
    StopCmdDma(h);
    W32(DPLBASE, 0);
    OsFree(h->W); h->W = NULL;
}

const HWOPS g_HdaOps = {
    "hda", HdaInit, HdaShutdown,
    HdaStartPlay, HdaStopPlay, HdaPlayPos, HdaPlayTick,
    HdaStartRec, HdaStopRec, HdaRecPos, NULL,
    HdaIsr, HdaApplyLine, HdaApplyRecSrc, HdaDump
};
