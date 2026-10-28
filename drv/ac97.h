/*
 * AUDIONT - AC'97 shared definitions (controller back-ends + codec layer)
 */
#ifndef AUDIONT_AC97_H
#define AUDIONT_AC97_H

typedef struct _AC97_STATE {
    USHORT (*CodecRead)(CARD *c, ULONG reg);
    void   (*CodecWrite)(CARD *c, ULONG reg, USHORT v);
    PVOID  NamMmio, BmMmio;
    ULONG  RegSr, RegPicb, PosShift;
    ULONG  VendorId;           /* codec 0x7C:0x7E */
    USHORT ExtId;              /* register 0x28 */
    UCHAR  MasterBits;         /* 5 or 6 */
    UCHAR  HpBits;
    BOOLEAN HasAux, HasPcBeep, HasHp, HasMono;
    BOOLEAN InvEapd;
    BOOLEAN RefreshLogged;
    USHORT PcmMark;            /* what the PCM register reads after the driver set it */
    BOOLEAN HwOk;              /* register file located and verified: Shutdown may touch it */
    /* VIA */
    USHORT SgdBase;
    ULONG  ViaType;
    UCHAR  ViaPlayBase, ViaRecBase;
} AC97_STATE;

/* codec registers */
#define AC97_RESET        0x00
#define AC97_MASTER       0x02
#define AC97_HEADPHONE    0x04
#define AC97_MASTER_MONO  0x06
#define AC97_PC_BEEP      0x0A
#define AC97_PHONE        0x0C
#define AC97_MIC          0x0E
#define AC97_LINE         0x10
#define AC97_CD           0x12
#define AC97_VIDEO        0x14
#define AC97_AUX          0x16
#define AC97_PCM          0x18
#define AC97_REC_SEL      0x1A
#define AC97_REC_GAIN     0x1C
#define AC97_GENERAL      0x20
#define AC97_POWERDOWN    0x26
#define AC97_EXT_ID       0x28
#define AC97_EXT_STATUS   0x2A
#define AC97_PCM_FRONT_RATE 0x2C
#define AC97_PCM_SURR_RATE  0x2E
#define AC97_PCM_LFE_RATE   0x30
#define AC97_PCM_ADC_RATE   0x32
#define AC97_CENTER_LFE   0x36
#define AC97_SURROUND     0x38
#define AC97_VENDOR_ID1   0x7C
#define AC97_VENDOR_ID2   0x7E

NTSTATUS Ac97CodecInit(CARD *c);
void     Ac97ApplyLine(CARD *c, ULONG line);
void     Ac97Refresh(CARD *c);
void     Ac97ApplyRecSrc(CARD *c);
ULONG    Ac97Dump(CARD *c, char *buf, ULONG size);

#endif
