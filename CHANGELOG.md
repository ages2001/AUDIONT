# Changelog

## v1.1

Changelog:

- AMD Ryzen (AM4 / AM5) HD Audio: a codec that does not answer at once is probed again for a few seconds with a controller reset in between, as Linux does (it was not found before, so there was no sound).
- Added more HD Audio controller IDs (AMD / ATI Radeon HDMI, VIA and Zhaoxin HDMI, Teradici) with their settings; every other class 04/03 controller is still used.
- Fixed no sound on AC'97 codecs whose mixer takes writes only a while after the reset (Cirrus Logic CS4299 in the IBM ThinkPad T23: PCM out stayed muted): the driver waits until a mixer write sticks, and when playback starts a muted PCM out is set again.
- Headphone / alternate line out and mono out are unmuted on every codec (they were left muted where the start-up probe missed them).
- Cirrus Logic CS4297A / CS4299 / CS4201 / CS4205 (IBM ThinkPad T2x, X2x, A2x and others): the AC mode register is set so the DACs go through the mixer (the BIOS may leave it otherwise and a codec reset does not clear it).
- AC'97: when playback starts the codec is checked; registers the BIOS (notebook volume buttons, power management) or a suspend reset are set again, a powered down codec and a stopped AC-link are brought up.
- Fixed sound repeating (a loop) or stuttering when the system is busy: the DMA ring after the mixed data is kept silent, so a late DPC gives a short gap instead of old sound again; the new LatencyMaxMs registry value lets the latency grow by itself after underruns.
- AC'97 (Intel, SiS, nVidia, AMD) and VIA: the DMA position is read until it is stable (at a buffer change the controller shows a stale value for a moment, which skipped or repeated part of the sound); a position that steps back is not taken for a lap of the ring, also when recording.
- Fixed the headphone and mono outputs being louder than the master output at low volume on codecs with a 6-bit master volume.
- Fixed no sound in VLC, web browsers (Supermium, MyPal and others) and other programs that open the sound card with WAVE_FORMAT_EXTENSIBLE or 32-bit float, also through DirectSound / one-core-api on XP x64 (GitHub issue #1): 32 / 64-bit float and WAVE_FORMAT_EXTENSIBLE (16 / 24 / 32-bit PCM or float, 1 to 8 channels, mixed down to stereo) are accepted for playback and recording; before only plain PCM was, so these programs found no device that would play.
- Windows 2000 / XP: fixed "No audio devices" after installing AUDIONT through Device Manager or Found New Hardware with the INF (no setup program ran, so the WinMM driver audiont.dll was not registered): the INF copies audiont.dll (on XP x64 also the 32-bit one to SysWOW64) and audiont.sys writes the Drivers32 entries itself when it starts (RegisterWinmm = 0 turns this off).
- Windows 2000 / XP Device Manager shows every card by its own name ("Realtek ALC3234 High Definition Audio (AUDIONT)", "Cirrus Logic CS4299 AC'97 Audio (AUDIONT)") instead of the INF's generic one (DeviceNames = 0 keeps the INF's).

## v1.0

Changelog:

- Initial Release!
