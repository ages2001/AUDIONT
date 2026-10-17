/*
 * AUDIONT - Intel Smart Sound Technology (audio DSP) support.
 *
 *   sst.c      platform table, DSP power / ROM / firmware loading (CLDMA on
 *              Skylake / Kaby Lake, HD Audio host DMA on Apollo Lake ..
 *              Tiger Lake, ACE on Meteor Lake and later), IPC transport,
 *              the firmware manifest, endpoint discovery (NHLT, ACPI)
 *   sstipc4.c  IPC4 (Intel cAVS firmware and SOF IPC4): copier pipelines
 *              host DMA <-> DMIC / SSP / HD Audio link / SoundWire ALH
 *   sstipc3.c  SOF IPC3 (Ice Lake, Jasper Lake, Elkhart Lake, Bay Trail,
 *              Cherry Trail, Broadwell): component pipelines
 *
 * HD Audio codecs keep their direct (coupled) stream; the DSP carries the
 * endpoints the host cannot reach without it: digital microphones, I2S
 * codecs and amplifiers on SSP ports, SoundWire peripherals.
 */
#ifndef AUDIONT_SST_H
#define AUDIONT_SST_H

#include "audiont.h"
#include "acpi.h"

/* generations */
#define SST_SKL   1     /* cAVS 1.5: Skylake, Kaby Lake, Comet Lake-S PCH-H (CLDMA loader) */
#define SST_APL   2     /* cAVS 1.5+: Apollo Lake, Gemini Lake */
#define SST_CNL   3     /* cAVS 1.8: Cannon Lake, Coffee Lake, Comet Lake, Rocket Lake-S */
#define SST_ICL   4     /* cAVS 2.0: Ice Lake, Jasper Lake */
#define SST_TGL   5     /* cAVS 2.5: Tiger Lake, Elkhart Lake, Alder Lake, Raptor Lake */
#define SST_MTL   6     /* ACE 1.x: Meteor Lake, Arrow Lake */
#define SST_LNL   7     /* ACE 2.0: Lunar Lake */
#define SST_PTL   8     /* ACE 3.0: Panther Lake, Wildcat Lake */
#define SST_NVL   9     /* ACE 4.0: Nova Lake */
/* the older audio DSPs with their own DMA (lpe.c), SOF IPC3 */
#define SST_BYT   0x21  /* Bay Trail (LPE) */
#define SST_CHT   0x22  /* Cherry Trail, Braswell */
#define SST_BDW   0x23  /* Broadwell (Wildcat Point-LP) */
#define SST_LPE(s) ((s)->P->Gen >= SST_BYT)

/* firmware / protocol */
#define SST_FW_AVS   1  /* Intel AudioDSP base firmware (IPC4) */
#define SST_FW_SOF4  2  /* Sound Open Firmware, IPC4 */
#define SST_FW_SOF3  3  /* Sound Open Firmware, IPC3 */

typedef struct _SST_PLAT {
    USHORT Dev;
    UCHAR  Gen;
    UCHAR  Fw;
    const char *File;           /* below <SystemRoot>\System32\drivers\audiont\ */
    const char *Name;
    UCHAR  Cores;
    UCHAR  InitMask;            /* cores powered for the ROM */
    UCHAR  HostCores;           /* cores the host powers (else the firmware) */
    UCHAR  Ssp;                 /* SSP ports */
} SST_PLAT;

/* endpoint kinds */
#define EP_HDA   1              /* HD Audio link codec through the DSP (option DspHdaLink) */
#define EP_DMIC  2
#define EP_SSP   3
#define EP_SDW   4

typedef struct _SST_FMT {
    ULONG  Rate;
    USHORT Ch, Bits, Valid;
} SST_FMT;

struct _SST_CODEC;

typedef struct _SST_EP {
    UCHAR   Kind;
    UCHAR   Dir;                /* 0 playback, 1 capture */
    UCHAR   Port;               /* SSP port, SoundWire link, HDA link stream */
    UCHAR   Line;               /* LINE_xxx it is offered as (capture) */
    SST_FMT Be;                 /* format on the wire / gateway */
    const UCHAR *Blob;          /* NHLT gateway configuration */
    ULONG   BlobLen;
    ULONG   Alh;                /* SoundWire: IPC4 ALH gateway node index (ALH stream id, 0x50: multi-gateway) */
    UCHAR   Pdi;                /* SoundWire: PDI of the first stream on link Port (sdw.c) */
    UCHAR   SdwDev;             /* SoundWire: peripheral index (sdw.c) */
    struct _SST_CODEC *Codec;   /* I2C / SoundWire codec or amplifier behind it */
    char    Name[32];
} SST_EP;

/* one direction's pipelines */
typedef struct _SST_PATH {
    BOOLEAN Created;
    BOOLEAN Running;
    SST_EP *Ep;
    UCHAR   PplFe, PplBe;       /* pipeline instance ids */
    UCHAR   InstFe, InstBe;     /* copier instance ids */
    ULONG   HostDma;            /* host DMA channel (stream index within the direction) */
    ULONG   HostBufPa, HostBufPages, HostBufSize;   /* LPE: page table of the host ring */
    ULONG   PosnOff;            /* LPE: stream position in the mailbox (PCM_PARAMS reply) */
    ULONG   LinkDma;            /* link DMA stream (HD Audio link, ACE 2+ gateways) */
    SST_FMT Fe;                 /* host side format */
    /* IPC3 component ids */
    ULONG   Comp[6];
    ULONG   NComp;
    ULONG   Pipe[2];
} SST_PATH;

#define SST_MAX_EP 16
#define SST_MBOX   4096

typedef struct _SST {
    CARD   *Card;
    const SST_PLAT *P;
    PUCHAR  Hda;                /* HD Audio registers (shared with hda.c) */
    PUCHAR  Dsp;                /* DSP BAR (BAR4) */
    ULONG   DspPa, DspSize;
    ULONG   PpOff, MlOff, SpbOff;
    ULONG   Iss, Oss;
    /* IPC registers */
    ULONG   ReqOff, ReqExt, ReqBusy;
    ULONG   AckOff, AckDone;
    ULONG   RspOff, RspExt, RspBusy, RspAck;   /* RspAck: CNL HIPCTDA / ACE HfIPCxTDA */
    ULONG   CtlOff;
    ULONG   SramBase, SramWin;
    ULONG   StatusOff;          /* ROM / firmware status */
    ULONG   UpOff, DownOff;     /* mailboxes (IPC3: from FW_READY) */
    ULONG   UpSize, DownSize;
    BOOLEAN Ready;
    ULONG   Errors;
    /* firmware */
    UCHAR  *Img;
    ULONG   ImgLen;
    ULONG   Payload;            /* image offset that goes to the DSP */
    ULONG   ModCopier, ModPeak, ModGain, ModMixin, ModMixout;
    char    FwVer[32];
    ULONG   MaxPpl;
    /* IPC4 / IPC3 replies */
    ULONGLONG RxHdr;
    ULONG   RxLen;
    UCHAR   Rx[SST_MBOX];
    UCHAR   Tx[SST_MBOX];
    /* endpoints and paths */
    SST_EP  Ep[SST_MAX_EP];
    ULONG   NEp;
    SST_EP *PlayEp;             /* NULL: playback on the HD Audio codec (coupled) */
    SST_EP *RecEp[LINE_COUNT];  /* capture endpoint per record source line */
    SST_PATH Play, Rec;
    UCHAR   NextPpl, NextInst;
    /* capture ring with more than two channels (4-mic arrays): copied to the engine ring */
    DMA_BUFFER RecWide;
    ULONG   RecCh;
    ULONG   RecDone;            /* frames converted */
    /* IPC3 */
    BOOLEAN Lpe;                /* lpe.c: shim IPC registers (DONE cleared by writing 0) */
    void   *LpeCtx;
    ULONG   RspDone;            /* LPE: IPCD DONE */
    ULONG   StreamOff;          /* SOF IPC3: stream position window */
    ULONG   NextComp;
    ULONG   Ipc3Abi;
    /* SoundWire / codecs (sdw.c, sstcodec.c) */
    void   *Sdw;                /* sdw.c (sdw.h: SdwProbe / SdwStart / SdwPower ..) */
    void   *Codecs;
} SST;

/* sst.c */
BOOLEAN SstProbe(CARD *c, PUCHAR hda, ULONG ppOff, ULONG mlOff, ULONG iss, ULONG oss, BOOLEAN haveCodec);
void    SstShutdown(CARD *c);
ULONG   SstDump(CARD *c, char *buf, ULONG size);
BOOLEAN SstPlayActive(CARD *c);                     /* playback goes through the DSP */
BOOLEAN SstRecActive(CARD *c);                      /* the current record source does */
ULONG   SstPlaySd(CARD *c);                         /* stream descriptor index to use */
ULONG   SstRecSd(CARD *c);
ULONG   SstRecChannels(CARD *c);                    /* host channels of the record stream */
BOOLEAN SstStart(CARD *c, BOOLEAN rec);             /* after the host stream is set up */
void    SstStop(CARD *c, BOOLEAN rec);
void    SstRecCopy(CARD *c, ULONG toFrame);         /* wide capture ring -> engine ring */
DMA_BUFFER *SstRecRing(CARD *c);                    /* ring for the host DMA (engine ring or wide) */
void    SstVolume(CARD *c, ULONG line);             /* codec / amplifier volume */

/* register access */
#define DSP32(s, o)      MMIO32((s)->Dsp, (o))
#define DSPW32(s, o, v)  (MMIO32((s)->Dsp, (o)) = (ULONG)(v))
#define HDA32(s, o)      MMIO32((s)->Hda, (o))
#define HDAW32(s, o, v)  (MMIO32((s)->Hda, (o)) = (ULONG)(v))
#define HDA16(s, o)      MMIO16((s)->Hda, (o))
#define HDAW16(s, o, v)  (MMIO16((s)->Hda, (o)) = (USHORT)(v))
#define HDA8(s, o)       MMIO8((s)->Hda, (o))
#define HDAW8(s, o, v)   (MMIO8((s)->Hda, (o)) = (UCHAR)(v))

/* sst.c internals used by the IPC layers */
BOOLEAN SstIpc(SST *s, ULONG primary, ULONG ext, const void *data, ULONG len, ULONG timeoutMs);
void    SstMemFromDsp(SST *s, ULONG off, void *dst, ULONG len);
BOOLEAN SstRxPending(SST *s, ULONG *pri, ULONG *ext);
void    SstRxDone(SST *s);
void    SstMemToDsp(SST *s, ULONG off, const void *src, ULONG len);
void    SstLinkDmaSetup(SST *s, ULONG stream, ULONG tag, USHORT fmt, BOOLEAN run);
BOOLEAN SstMlPower(SST *s, ULONG elid, ULONG sublink, BOOLEAN on);
ULONG   SstMlLink(SST *s, ULONG elid);              /* ML link offset of an extended link type */
USHORT  SstHdaFmt(const SST_FMT *f);
SST_EP *SstAddEp(SST *s, UCHAR kind, UCHAR dir, UCHAR port, const char *name);

/* sstipc4.c */
BOOLEAN Ipc4Init(SST *s);                           /* after FW_READY: config, manifest */
BOOLEAN Ipc4PathCreate(SST *s, SST_PATH *p, SST_EP *ep, BOOLEAN rec);
void    Ipc4PathDelete(SST *s, SST_PATH *p);
BOOLEAN Ipc4PathRun(SST *s, SST_PATH *p, BOOLEAN run, BOOLEAN rec);
BOOLEAN Ipc4WaitReady(SST *s, ULONG ms);

/* sstcodec.c: SSP frame format for SOF IPC3 (no NHLT blob): MCLK, BCLK, slot width, valid bits */
void    SstSspFormat(SST *s, UCHAR port, ULONG *mclk, ULONG *bclk, UCHAR *slot, UCHAR *valid);

/* sstipc3.c */
BOOLEAN Ipc3WaitReady(SST *s, ULONG ms);
void    Ipc3Notify(SST *s);                         /* a DSP -> host message in the up box */
BOOLEAN Ipc3Init(SST *s);
BOOLEAN Ipc3PathCreate(SST *s, SST_PATH *p, SST_EP *ep, BOOLEAN rec);
void    Ipc3PathDelete(SST *s, SST_PATH *p);
BOOLEAN Ipc3PathRun(SST *s, SST_PATH *p, BOOLEAN run, BOOLEAN rec);


#endif
