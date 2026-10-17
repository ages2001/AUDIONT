#!/usr/bin/env python3
"""Analyse a captured wav: find steady tones (STFT based), frequency, level,
SNR and glitches.  Usage: wavan.py file.wav [expected freqs...]"""
import sys, wave, numpy as np

def bh(n):
    t = 2 * np.pi * np.arange(n) / (n - 1)
    return 0.35875 - 0.48829 * np.cos(t) + 0.14128 * np.cos(2 * t) - 0.01168 * np.cos(3 * t)

def load(path):
    w = wave.open(path)
    fs = w.getframerate(); ch = w.getnchannels()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(np.float64)
    return fs, x.reshape(-1, ch) / 32768.0

def tones(path, minlen=0.2):
    fs, x = load(path)
    mono = x.mean(axis=1)
    hop = int(fs * 0.02); N = hop * 2
    fr = []
    for i in range(0, len(mono) - N, hop):
        s = mono[i:i + N]
        r = np.sqrt((s ** 2).mean())
        if r < 0.002:
            fr.append(None); continue
        sp = np.abs(np.fft.rfft(s * np.hanning(N)))
        k = int(np.argmax(sp[1:])) + 1
        sig = (sp[max(k - 3, 0):k + 4] ** 2).sum()
        if sig < 0.8 * (sp ** 2).sum():
            fr.append(None); continue           # not a pure tone
        fr.append(k * fs / N)
    segs = []
    i = 0
    while i < len(fr):
        if fr[i] is None: i += 1; continue
        j = i
        while j + 1 < len(fr) and fr[j + 1] is not None and abs(fr[j + 1] - fr[i]) <= fs / N * 1.5:
            j += 1
        a, b = i * hop, j * hop + N
        if (b - a) / fs >= minlen: segs.append((a, b))
        i = j + 1
    res = []
    for a, b in segs:
        seg = mono[a:b]
        core = seg[int(len(seg) * 0.1): int(len(seg) * 0.9)]
        n = len(core)
        sp = np.abs(np.fft.rfft(core * bh(n)))
        k = int(np.argmax(sp[2:])) + 2
        al, be, ga = (np.log(sp[k + d] + 1e-12) for d in (-1, 0, 1))
        p = 0.5 * (al - ga) / (al - 2 * be + ga)
        f = (k + p) * fs / n
        pw = sp ** 2
        sig = pw[max(k - 6, 0):k + 7].sum()
        noise = pw[3:].sum() - sig
        snr = 10 * np.log10(sig / max(noise, 1e-20))
        amp = np.sqrt((core ** 2).mean()) * np.sqrt(2)
        lvl = 20 * np.log10(amp + 1e-12)
        d2 = np.abs(np.diff(core, 2))
        lim = (2 * np.pi * f / fs) ** 2 * amp * 2.5 + 0.004
        glitches = int((d2 > lim).sum())
        lr = 1.0
        if x.shape[1] == 2:
            l = x[a:b, 0]; r = x[a:b, 1]
            lr = float(np.corrcoef(l, r)[0, 1]) if l.std() > 0 and r.std() > 0 else 0
        res.append(dict(start=a / fs, dur=(b - a) / fs, freq=f, snr=snr, level=lvl,
                        glitches=glitches, lr=lr))
    return res

def show(res):
    for r in res:
        print('  %7.3fs  %5.3fs  %8.1f Hz  %6.1f dBFS  SNR %5.1f dB  glitches %d  L/R %.2f' % (
            r['start'], r['dur'], r['freq'], r['level'], r['snr'], r['glitches'], r['lr']))

if __name__ == '__main__':
    r = tones(sys.argv[1])
    show(r)
    exp = [float(v) for v in sys.argv[2:]]
    if exp:
        got = [t['freq'] for t in r]
        ok = True
        for e in exp:
            m = [t for t in r if abs(t['freq'] - e) < max(2, e * 0.002)]
            if not m:
                print('MISSING tone %g Hz' % e); ok = False; continue
            t = m[0]
            if t['snr'] < 40 or t['glitches'] > 0:
                print('BAD tone %g Hz: snr %.1f glitches %d' % (e, t['snr'], t['glitches'])); ok = False
        print('WAV-CHECK', 'PASS' if ok else 'FAIL')
