/*
 * AUDIONT - helpers shared by the Realtek SoundWire peripheral drivers
 * (sdw_rt700.c, sdw_rt712.c, sdw_rt715.c, sdw_rt722.c, sdw_rt5682.c),
 * implemented in sdw_rtcom.c.
 */
#ifndef AUDIONT_SDW_RTCOM_H
#define AUDIONT_SDW_RTCOM_H

#include "sdw.h"

/* SDCA parts: vendor "index" registers of the HDA-like NIDs, 16-bit MBQ quantities at (nid << 20) | reg */
BOOLEAN SdwRtIdxWr(SDW_DEV *d, ULONG nid, ULONG reg, ULONG v);
BOOLEAN SdwRtIdxRd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG *v);
BOOLEAN SdwRtIdxUpd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG mask, ULONG val);

/*
 * HDA-verb parts (RT700, RT711, RT715 of SoundWire 1.1): the regmap address space of Linux
 * rt7xx_sdw_write / rt7xx_sdw_read on the 8-bit SoundWire registers.
 *   reg > 0xffff          index register ((0x7500 | nid) << 8 | index), 16-bit value
 *   reg < 0x4fff, 0xff01  12-bit verb (verb << 8 | nid), 8-bit payload
 *   0x7xxx / 0x83xx       4-bit verb, 16-bit payload (high byte at reg, low at reg + 0x1000 | 0x80)
 * Reads put the payload *val in, then return the 32-bit response (READ_HDA_3..0, 0x2012..0x2015).
 */
BOOLEAN SdwRtHdaWr(SDW_DEV *d, ULONG reg, ULONG val);
BOOLEAN SdwRtHdaRd(SDW_DEV *d, ULONG reg, ULONG *val);
BOOLEAN SdwRtHdaSeq(SDW_DEV *d, const ULONG *pairs, ULONG n);       /* { reg, value } x n */
BOOLEAN SdwRtHdaIdxWr(SDW_DEV *d, ULONG nid, ULONG reg, ULONG v);
BOOLEAN SdwRtHdaIdxRd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG *v);
BOOLEAN SdwRtHdaIdxUpd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG mask, ULONG val);
#define RT_VERB_SET_CONNECT_SEL     0x3100
#define RT_VERB_SET_EAPD_BTLENABLE  0x3c00
#define RT_VERB_SET_POWER_STATE     0x3500
#define RT_VERB_SET_CHANNEL_STREAMID 0x3600
#define RT_VERB_SET_PIN_WIDGET_CONTROL 0x3700
#define RT_VERB_SET_UNSOLICITED_ENABLE 0x3800
#define RT_SET_AMP_GAIN_MUTE_H      0x7300      /* + nid: payload [15] out [14] in [13] L [12] R [7] mute [6:0] gain */
#define RT_FUNC_RESET               0xff01
#define RT_AMP_OUT_LR               0xb000      /* output amplifier, both channels */
#define RT_AMP_IN_LR                0x7000      /* input amplifier, both channels */
#define RT_AMP_MUTE                 0x0080

/* SoundWire 1.1 Realtek parts: the bus clock code in the vendor registers 0xe0 / 0xf0 (rt7xx_clock_config) */
ULONG   SdwRtBusClock(SDW_DEV *d);              /* the link's bus clock from the ACPI link properties, 0 unknown */
BOOLEAN SdwRtClockConfig(SDW_DEV *d);

/* SDCA functions the ACPI DisCo description lists (Linux is_sdca_endpoint_present) */
#define RT_FN_JACK      1           /* UAJ / RJ / simple jack */
#define RT_FN_AMP       2           /* smart / simple amplifier */
#define RT_FN_MIC       4           /* smart / simple / speaker microphone */
#define RT_FN_ALL       7
ULONG   SdwRtFuncs(SDW_DEV *d);     /* RT_FN_xxx mask; RT_FN_ALL when the BIOS has no SDCA function properties */

/* registry "SdwDmic": 0 headset microphone (default), 1 the codec's digital microphones */
BOOLEAN SdwRtPreferDmic(void);

/*
 * A second data port of the peripheral that reads the same samples as the one the bus core
 * programmed (playback to an additional output of a multi-function codec, e.g. the speaker
 * amplifier next to the headphones): DPn registers of both banks copied from 'src' to port
 * 'to', the active bank last.  Only for ports with the simplified channel prepare.
 */
BOOLEAN SdwRtMirror(SDW_DEV *d, const SDW_DPN *src, ULONG to);
void    SdwRtMirrorOff(SDW_DEV *d, ULONG port);

/* Group Entity of a jack function: the detected mode as the selected mode, headset (5) when none is
   detected - the "headset inserted" state without jack detection */
void    SdwRtJackMode(SDW_DEV *d, ULONG fn, ULONG ge);

#endif
