/*
 * AUDIONT - generic AC'97 / Intel High Definition Audio driver for
 * Windows NT 3.1, 3.5, 3.51 and 4.0
 *
 * Central definitions.
 *
 * Architecture
 * ------------
 *   hardware back-ends (ac97.c, via.c, hda.c, usbaud.c) -> HWOPS
 *   usbaud.c   : USB Audio 1.0 / 2.0 devices through the USBNT USB stack
 *   engine.c   : 48 kHz/16-bit/stereo software mixer, DMA ring management,
 *                play/record streams, IRP queues, positions
 *   convert.c  : PCM format conversion and sample-rate conversion
 *   synth.c    : General MIDI FM synthesizer rendered into the mixer
 *   ctrl.c     : mixer controls (software + hardware), aux devices
 *   main.c     : DriverEntry, PCI discovery, device objects, IRP dispatch
 */
#ifndef AUDIONT_H
#define AUDIONT_H

#include "os.h"
#include "ntsnd.h"

#define AUDIONT_VERSION       0x0100      /* 1.0 */
#define AUDIONT_VERSION_STR   "1.0"
#define MAX_CARDS             8
#define MAX_PLAY_STREAMS      8
#define MAX_REC_STREAMS       4

/* engine format: fixed */
#define ENG_RATE              48000
#define ENG_CH                2
#define ENG_FRAME_BYTES       4

/* DMA ring geometry */
#define RING_PERIOD_FRAMES    512                       /* 10.67 ms */
#define RING_PERIODS          16
#define RING_FRAMES           (RING_PERIOD_FRAMES * RING_PERIODS)
#define RING_BYTES            (RING_FRAMES * ENG_FRAME_BYTES)
#define PERIOD_BYTES          (RING_PERIOD_FRAMES * ENG_FRAME_BYTES)
#define MIX_CHUNK_FRAMES      128

/* ------------------------------------------------------------------ */
/* controls                                                            */
/* ------------------------------------------------------------------ */
enum {
    LINE_MASTER = 0, LINE_WAVE, LINE_SYNTH, LINE_CD, LINE_LINEIN, LINE_MIC,
    LINE_AUX, LINE_PCSPK, LINE_RECORD, LINE_REC_WAVEIN, LINE_COUNT
};
/* record source selector values (bit mask of selectable lines) */
#define RECSRC_MIC     (1 << LINE_MIC)
#define RECSRC_LINE    (1 << LINE_LINEIN)
#define RECSRC_CD      (1 << LINE_CD)
#define RECSRC_AUX     (1 << LINE_AUX)
#define RECSRC_MIX     (1 << LINE_MASTER)          /* "stereo mix" */

typedef struct _LINE_STATE {
    BOOLEAN Present;        /* line exists on this card */
    BOOLEAN HwVolume;       /* volume implemented in hardware */
    BOOLEAN HasMute;
    BOOLEAN Mute;
    USHORT  Left, Right;    /* 0..65535 */
} LINE_STATE;

/* ------------------------------------------------------------------ */
/* streams                                                             */
/* ------------------------------------------------------------------ */
typedef struct _PCM_FORMAT {
    ULONG Rate;
    USHORT Channels;
    USHORT Bits;
    USHORT BlockAlign;
    USHORT Float;           /* reserved, always 0 */
} PCM_FORMAT;

/* polyphase windowed-sinc resampler (convert.c) */
#define RS_HIST      256              /* history ring (frames), power of two */
#define RS_MAX_HALF  120              /* max half filter length (input frames) */
typedef struct _RESAMPLER {
    ULONG  InRate, OutRate;
    ULONG  Phase;                     /* 0..OutRate-1, fraction = Phase/OutRate */
    ULONG  Half;                      /* taps = 2*Half */
    ULONG  ScaleQ16;                  /* min(1, Out/In) in Q16 */
    ULONG  Base;                      /* absolute index of the current input frame */
    ULONG  Written;                   /* absolute count of input frames pushed */
    ULONG  Gcd, NPhases;
    SHORT *Cache;                     /* NPhases * 2*Half weights (Q14) or NULL */
    BOOLEAN Passthrough;
    SHORT  Hist[RS_HIST][2];
} RESAMPLER;

typedef struct _CONVERTER {
    PCM_FORMAT Fmt;
    RESAMPLER  Rs;
    UCHAR  Carry[8];                  /* partial input frame spanning buffers (play) */
    ULONG  CarryLen;
    UCHAR  Pend[8];                   /* encoded frame not yet delivered (record) */
    ULONG  PendLen, PendOff;
} CONVERTER;

#define POSHIST 64
typedef struct _POSMAP { ULONGLONG OutFrame; ULONG StreamBytes; } POSMAP;

typedef struct _PLAY_STREAM {
    BOOLEAN    InUse;
    BOOLEAN    Paused;      /* WAVE_DD_STOP */
    BOOLEAN    Started;     /* has received data since open/reset */
    PVOID      FileObject;
    CONVERTER  Conv;
    LIST_ENTRY Queue;       /* IRPs waiting / being consumed */
    LIST_ENTRY Done;        /* IRPs fully rendered, waiting to be heard */
    PIRP       Cur;         /* IRP currently being consumed (head of Queue) */
    ULONG      CurOffset;   /* bytes consumed of Cur */
    ULONG      BytesRendered; /* total stream bytes consumed by the mixer */
    POSMAP     Hist[POSHIST];
    ULONG      HistHead;
    ULONG      BytesPlayedAtReset;
} PLAY_STREAM;

typedef struct _REC_STREAM {
    BOOLEAN    InUse;
    BOOLEAN    Running;
    PVOID      FileObject;
    CONVERTER  Conv;
    LIST_ENTRY Queue;
    ULONG      CurOffset;
    ULONG      BytesRecorded;
} REC_STREAM;

/* ------------------------------------------------------------------ */
/* hardware abstraction                                                */
/* ------------------------------------------------------------------ */
struct _CARD;
typedef struct _HWOPS {
    const char *Name;
    NTSTATUS (*Init)(struct _CARD *c);
    void     (*Shutdown)(struct _CARD *c);
    BOOLEAN  (*StartPlay)(struct _CARD *c);
    void     (*StopPlay)(struct _CARD *c);
    ULONG    (*PlayPos)(struct _CARD *c);       /* frame index within ring */
    void     (*PlayTick)(struct _CARD *c);      /* keep DMA running (AC97 LVI) */
    BOOLEAN  (*StartRec)(struct _CARD *c);
    void     (*StopRec)(struct _CARD *c);
    ULONG    (*RecPos)(struct _CARD *c);
    void     (*RecTick)(struct _CARD *c);
    BOOLEAN  (*Isr)(struct _CARD *c);           /* returns TRUE if interrupt was ours */
    void     (*ApplyLine)(struct _CARD *c, ULONG line);  /* push a hw line volume */
    void     (*ApplyRecSrc)(struct _CARD *c);
    ULONG    (*Dump)(struct _CARD *c, char *buf, ULONG size);  /* diagnostics text */
    ULONGLONG (*PlayCount)(struct _CARD *c);    /* optional: frames consumed since StartPlay */
} HWOPS;

typedef enum { HW_AC97_ICH = 1, HW_AC97_SIS, HW_AC97_NV, HW_AC97_ALI, HW_AC97_VIA,
               HW_HDA, HW_USB, HW_LPE } HWTYPE;

/* device kinds created per card */
enum { DEV_WAVEOUT = 0, DEV_WAVEIN, DEV_MIDIOUT, DEV_AUX, DEV_CTRL, DEV_KINDS };

typedef struct _DEV_EXT {
    ULONG Magic;
    struct _CARD *Card;
    ULONG Kind;
    ULONG Index;            /* number in device name */
} DEV_EXT;
#define DEVEXT_MAGIC 0x41554E54

struct _SYNTH;
struct _HDA_STATE;
struct _AC97_STATE;
struct _USB_CARD;

/* the interrupt of a card (Windows 2000 / XP: from Plug and Play) */
typedef struct _CARD_INT {
    BOOLEAN Valid;
    BOOLEAN Shared;
    KINTERRUPT_MODE Mode;
    ULONG   Vector;
    KIRQL   Irql;
    KAFFINITY Affinity;
} CARD_INT;

typedef struct _CARD {
    ULONG   Index;
    HWTYPE  Type;
    const HWOPS *Ops;
    char    Desc[64];       /* controller description */
    char    Codec[64];      /* codec description */
    /* PCI */
    ULONG   Bus, Dev, Fn;
    USHORT  VendorId, DeviceId;
    USHORT  SubVendor, SubDevice;
    UCHAR   Revision;
    UCHAR   Irq;
    ULONG   HwFlags;            /* back-end specific quirk flags */
    BOOLEAN DmaUncached;        /* DMA memory mapped uncached (non-snooping controller) */
    ULONG   Bar[6];
    /* resources */
    USHORT  IoBase0, IoBase1;   /* AC97: NAM / NABM */
    PVOID   Mmio;
    ULONG   MmioSize;
    DMA_BUFFER PlayRing, RecRing, Aux;   /* Aux: BDLs, CORB/RIRB, position buffer */
    BOOLEAN CanRecord;
    /* interrupts / timing */
    PKINTERRUPT Interrupt;
    KSPIN_LOCK  IntLock;
    CARD_INT    Int;
    ULONG   Vector;
    KIRQL   IrqlLevel;
    KDPC    IsrDpc;
    KDPC    TimerDpc;
    KTIMER  Timer;
    BOOLEAN TimerRunning;
    BOOLEAN Stopping;
    ULONG   IntCount;
    ULONG   TickCount;
    /* engine */
    OSLOCK  Lock;
    BOOLEAN Playing;            /* play DMA running */
    BOOLEAN Recording;          /* record DMA running */
    ULONG   LastPlayPos;        /* last hw frame position in ring */
    ULONGLONG PlayedFrames;     /* frames the hardware has consumed */
    ULONGLONG WrittenFrames;    /* frames the mixer has produced */
    ULONG   LatencyFrames;
    ULONG   IdleMs;             /* time since last audible output */
    ULONG   LastTickMs;
    ULONG   Underruns;
    ULONG   LastRecPos;
    ULONGLONG RecFrames;
    LONG    MixBuf[MIX_CHUNK_FRAMES * 2];
    LIST_ENTRY CompleteList;    /* IRPs to complete once the lock is dropped */
    BOOLEAN UseInterrupt;
    LONG    OpenHandles;            /* open file objects on the card's devices */
    PLAY_STREAM Play[MAX_PLAY_STREAMS];
    REC_STREAM  Rec[MAX_REC_STREAMS];
    struct _SYNTH *Synth;
    ULONG   SynthOpen;
    /* controls */
    LINE_STATE Lines[LINE_COUNT];
    char    LineShort[LINE_COUNT][16];  /* a line's own name (empty: the usual one), see AUDIONT_LINENAMES */
    char    LineLong[LINE_COUNT][32];
    ULONG   RecSrcMask;         /* selectable sources */
    ULONG   RecSrc;             /* current source (single bit) */
    ULONG   ChangeCount;
    /* hw-specific */
    struct _AC97_STATE *Ac97;
    struct _HDA_STATE  *Hda;
    struct _USB_CARD   *Usb;
    void   *Sst;                /* Intel Smart Sound DSP (sst.c) */
    void   *SideAmp;            /* speaker amplifiers behind the HD Audio codec (sideamp.c) */
    /* devices */
    PDEVICE_OBJECT Devs[DEV_KINDS];
    ULONG   DevIndex[DEV_KINDS];
} CARD;

extern CARD *g_Cards[MAX_CARDS];
CARD   *CardProbe(ULONG bus, ULONG dev, ULONG fn, const CARD_INT *Int);
CARD   *CardProbeAcpi(USHORT devId, ULONG mem0, ULONG mem1, BOOLEAN flag);   /* ACPI-only audio DSPs */
BOOLEAN LpeProbe(CARD *c);               /* lpe.c */
void    LpeScanAcpi(void);
void    AudiontRelocateAll(void);        /* main.c: PCI BARs above 4 GB -> below 4 GB */
void    CardRemove(CARD *c);
void    AudiontUnloadAll(void);
void    AudiontSetDriver(PDRIVER_OBJECT drv);
NTSTATUS NTAPI AudiontDispatch(PDEVICE_OBJECT dev, PIRP irp);
extern BOOLEAN g_StdNames;
extern ULONG g_CardCount;

/* engine.c */
NTSTATUS EngineInit(CARD *c);
void     EngineShutdown(CARD *c);
void     EngineProcess(CARD *c);         /* called at DISPATCH_LEVEL with lock held */
void     EngineKick(CARD *c);            /* ensure DMA running; lock held */
NTSTATUS WaveOutIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS WaveOutWrite(CARD *c, PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS WaveInIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS WaveInRead(CARD *c, PIRP irp, PIO_STACK_LOCATION sp);
void     StreamCleanup(CARD *c, PVOID fileObject);
void     EngineStopAll(CARD *c);
void     EngineLock(CARD *c);
void     EngineUnlock(CARD *c);          /* releases lock, then completes queued IRPs */
NTSTATUS EngineStart(CARD *c);           /* interrupts + timer after hw init */
void     IrpQueue(CARD *c, PLIST_ENTRY list, PIRP irp);  /* lock held */
void     IrpFinish(CARD *c, PIRP irp, NTSTATUS st, ULONG info); /* lock held */

/* convert.c */
BOOLEAN  ConvValidFormat(const WAVEFORMATEX *wf, ULONG len, PCM_FORMAT *out);
BOOLEAN  ConvSetup(CONVERTER *cv, const PCM_FORMAT *f, BOOLEAN record);  /* PASSIVE: may allocate */
void     ConvReset(CONVERTER *cv);
void     ConvFree(CONVERTER *cv);
ULONG    ConvPlay(CONVERTER *cv, const UCHAR *src, ULONG srcBytes, ULONG *consumed,
                  LONG *dst, ULONG frames, LONG volL, LONG volR);
ULONG    ConvRecord(CONVERTER *cv, const SHORT *src, ULONG frames, UCHAR *dst,
                    ULONG dstBytes, ULONG *produced);

/* synth.c */
NTSTATUS SynthCreate(CARD *c);
void     SynthDestroy(CARD *c);
void     SynthMidiBytes(CARD *c, const UCHAR *p, ULONG n);   /* lock held */
void     SynthReset(CARD *c);                                 /* lock held */
BOOLEAN  SynthActive(CARD *c);
void     SynthRender(CARD *c, LONG *mix, ULONG frames);
NTSTATUS MidiOutIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp);

/* ctrl.c */
void     CtrlInit(CARD *c);
void     CtrlLoad(CARD *c);
void     CtrlSave(CARD *c);
LONG     CtrlGain(CARD *c, ULONG line, int right);  /* software gain, Q16 */
NTSTATUS AuxIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS CtrlIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp);
ULONG    VolToDb100(USHORT v);    /* 0..65535 -> attenuation in 1/100 dB */

/* convert.c: resampler core, also used by the USB back-end */
void     RsInit(RESAMPLER *rs, ULONG in, ULONG out);
void     RsReset(RESAMPLER *rs);
void     RsFree(RESAMPLER *rs);
BOOLEAN  RsCanOut(const RESAMPLER *rs);
void     RsPush(RESAMPLER *rs, LONG l, LONG r);
void     RsOut(RESAMPLER *rs, LONG *l, LONG *r);
void     RsFromRing(RESAMPLER *rs, const SHORT *ring, ULONG ringFrames, ULONGLONG *pos, ULONGLONG limit,
                    LONG *out, ULONG frames, ULONG *starved);
void     RsToRing(RESAMPLER *rs, const LONG *in, ULONG frames, SHORT *ring, ULONG ringFrames, ULONGLONG *pos);

/* usbaud.c */
BOOLEAN  UsbAudInit(void);            /* TRUE if the USBNT audio interface is there */
void     UsbAudShutdown(void);
void     UsbAudFreeCard(CARD *c);
BOOLEAN  UsbMidiOut(CARD *c, const UCHAR *p, ULONG n);   /* TRUE if the card is a USB MIDI port */
void     UsbMidiReset(CARD *c);

/* main.c */
void     CardCreateDevs(CARD *c);
void     CardDestroy(CARD *c);
BOOLEAN  UsbConnected(CARD *c);
ULONG    CardNameFor(CARD *c, ULONG kind, char *buf, ULONG size);

/* back-ends */
extern const HWOPS g_Ac97Ops;
extern const HWOPS g_ViaOps;
extern const HWOPS g_HdaOps;
BOOLEAN Ac97Probe(CARD *c);   /* fills Type/Ops/Desc based on ids */
BOOLEAN HdaProbe(CARD *c);

/* sideamp.c: I2C / SPI speaker amplifiers fed over I2S by the HD Audio codec (CS35L41, CS35L56) */
BOOLEAN SideAmpProbe(CARD *c, ULONG codecVendor, ULONG codecSubsys);   /* passive level, after the codec probe */
void    SideAmpPower(CARD *c, BOOLEAN on);          /* any IRQL <= DISPATCH_LEVEL: queued to a worker thread */
void    SideAmpShutdown(CARD *c);
ULONG   SideAmpDump(CARD *c, char *buf, ULONG size);

/* os.c */
UCHAR  *OsLoadFile(const char *rel, ULONG *len);   /* below SystemRoot\System32\drivers\audiont */

/* main.c helpers */
static __inline__ ULONG IrpInLen(PIO_STACK_LOCATION sp)  { return sp->Parameters.DeviceIoControl.InputBufferLength; }
static __inline__ ULONG IrpOutLen(PIO_STACK_LOCATION sp) { return sp->Parameters.DeviceIoControl.OutputBufferLength; }
static __inline__ ULONG IrpCode(PIO_STACK_LOCATION sp)   { return sp->Parameters.DeviceIoControl.IoControlCode; }

#endif
