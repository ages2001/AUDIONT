#!/usr/bin/env python3
"""AUDIONT on Windows 2000 (x86) and Windows XP x64: Plug and Play
installation with setup.exe, then tones and MIDI through audiont.dll
(32 bit and, on x64, 64 bit programs), captured with QEMU's wav backend.

  python3 w5audio.py [w2k|xp64] [hda|ac97|ich9] [--keep] [--reg]

Also: tones in IEEE float, WAVE_FORMAT_EXTENSIBLE float / PCM and 5.1 (what VLC, browsers
and the DirectSound / WASAPI layers open), the Wave volume slider through the mixer API
(sndvol32's way).  --reg: a second boot after audiont.dll's Drivers32 entries were removed:
the driver must put them back (an installation with the INF alone) and a tone must play.

Uses the Windows 2000 / XP guests of USBNT's test/vm5.py.
"""
import os, re, sys, time, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
USBNT = os.environ.get('USBNT_DIR', '/home/claude/USBNT')
sys.path.insert(0, os.path.join(USBNT, 'test'))
import vm5
sys.path.insert(0, HERE)
import wavan

B = os.path.join(ROOT, 'build')
FST = os.path.join(USBNT, 'build', 'out', 'fstest.exe')
AUDIO = {
    'hda': ['-device', 'intel-hda', '-device', 'hda-duplex,audiodev=wav0'],
    'ich9': ['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=wav0'],
    'ac97': ['-device', 'AC97,audiodev=wav0'],
    # USB Audio 2.0 high speed device through USBNT (USBNT\\Audio), patched QEMU (usb-uac)
    'usb': ['-device', 'qemu-xhci,id=hc', '-device', 'usb-uac,bus=hc.0,uac=2,hs=on,audiodev=wav0'],
}


def files():
    f = [(os.path.join(B, 'setup.exe'), '/AUD/SETUP.EXE'), (os.path.join(B, 'audiont.dll'), '/AUD/AUDIONT.DLL'),
         (os.path.join(B, 'audmix.exe'), '/AUD/AUDMIX.EXE'), (os.path.join(B, 'attest.exe'), '/AUD/AUDTEST.EXE'),
         (os.path.join(ROOT, 'inf', 'audiont.inf'), '/AUD/2KXP/AUDIONT.INF'),
         (os.path.join(B, '2kxp', 'i386', 'audiont.sys'), '/AUD/2KXP/I386/AUDIONT.SYS'),
         (os.path.join(B, '2kxp', 'amd64', 'audiont.sys'), '/AUD/2KXP/AMD64/AUDIONT.SYS'),
         (os.path.join(B, 'audiont.dll'), '/AUD/2KXP/I386/AUDIONT.DLL'),
         (os.path.join(B, 'audiont.dll'), '/AUD/2KXP/AMD64/AUDNT32.DLL'),
         (FST, '/AUD/FSTEST.EXE')]
    for n in ('audiont.dll', 'setup.exe', 'audmix.exe', 'audtest.exe'):
        f.append((os.path.join(B, '2kxp', 'amd64', n), '/AUD/2KXP/AMD64/' + n.upper()))
    return f


def run(guest, audio, keep=False, reg=False):
    g = vm5.GUESTS[guest]
    par = 'SYSTEM\\CurrentControlSet\\Services\\audiont\\Parameters'
    bat = [
        'start /b C:\\AUD\\FSTEST.EXE clicker C:\\AUD\\STOP 600000',
    ]
    if audio == 'usb':
        bat += ['C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8']
    bat += [
        'C:\\AUD\\AUDTEST.EXE regdw %s DebugPorts 1' % par,
        'C:\\AUD\\AUDTEST.EXE regdw %s DebugLevel 2' % par,
        'start /wait C:\\AUD\\SETUP.EXE /s',
        'type C:\\%s\\AUDSETUP.LOG' % g['win'],
        'C:\\AUD\\FSTEST.EXE sleep 3000',
        'C:\\AUD\\AUDTEST.EXE info',
        'C:\\AUD\\FSTEST.EXE devlist AUDIONT',
        'C:\\AUD\\AUDTEST.EXE tone 440 1500 44100 16 2',
        'C:\\AUD\\FSTEST.EXE sleep 500',
        'C:\\AUD\\AUDTEST.EXE tone 1000 1000 22050 16 1',
        'C:\\AUD\\FSTEST.EXE sleep 500',
    ]
    want = [440, 1000]
    T = 'C:\\AUD\\AUDTEST.EXE'
    bat += [T + ' tonefmt 3 770 1200 44100 32 2', 'C:\\AUD\\FSTEST.EXE sleep 500',     # extensible float
            T + ' tonefmt 1 1100 1000 48000 32 2', 'C:\\AUD\\FSTEST.EXE sleep 500',    # IEEE float
            T + ' tonefmt 2 1300 1000 48000 16 6', 'C:\\AUD\\FSTEST.EXE sleep 500',    # extensible PCM 5.1
            T + ' mxvol wave 65535', T + ' tone 880 1000 48000 16 2', 'C:\\AUD\\FSTEST.EXE sleep 500',
            T + ' mxvol wave 16384', T + ' tone 880 1000 48000 16 2', 'C:\\AUD\\FSTEST.EXE sleep 500',
            T + ' mxvol wave 65535', T + ' drv32']
    want += [770, 1100, 1300]
    if g['x64']:
        bat += ['C:\\AUD\\2KXP\\AMD64\\AUDTEST.EXE info',
                'C:\\AUD\\2KXP\\AMD64\\AUDTEST.EXE tone 660 1200 48000 16 2',
                'C:\\AUD\\FSTEST.EXE sleep 500',
                'C:\\AUD\\2KXP\\AMD64\\AUDTEST.EXE tonefmt 3 1500 1000 48000 32 2',
                'C:\\AUD\\FSTEST.EXE sleep 500']
        want += [660, 1500]
    bat += ['C:\\AUD\\AUDTEST.EXE midi',
            'C:\\AUD\\AUDTEST.EXE stats',
            'C:\\AUD\\AUDTEST.EXE log']
    if reg:
        # what an installation through Device Manager left: no Drivers32 entries
        bat += [T + ' drv32 clear'] + (['C:\\AUD\\2KXP\\AMD64\\AUDTEST.EXE drv32 clear'] if g['x64'] else [])
    bat += [
            'C:\\AUD\\FSTEST.EXE write C:\\AUD\\STOP x',
            'C:\\AUD\\FSTEST.EXE sleep 2000']
    sys.path.insert(0, os.path.join(USBNT, 'test'))
    import w5usb
    fl = files() + (w5usb.driver_files() if audio == 'usb' else [])
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, 'work-aud-%s.raw' % guest), files=fl, bat=bat)
    w5usb.no_signing_prompt(work, g)
    name = 'aud-%s-%s' % (guest, audio)
    wav = os.path.join(vm5.OUT, name, 'out.wav')
    os.makedirs(os.path.dirname(wav), exist_ok=True)
    if os.path.exists(wav):
        os.remove(wav)
    dbg = os.path.join(vm5.OUT, name, 'debugcon.log')
    extra = ['-audiodev', 'wav,id=wav0,path=%s,out.frequency=48000' % wav, '-debugcon', 'file:' + dbg] + AUDIO[audio]
    vm = vm5.VM(guest, work, extra=extra, name=name)
    ok = vm.wait_serial('##AGENT-DONE##', 900, shots=60)
    time.sleep(2)
    vm.shot('end')
    out = vm.serial()
    vm.quit()
    print(out[-7000:])
    try:
        print('---- debugcon ----')
        print(open(dbg, 'rb').read().decode('latin1')[-5000:])
    except OSError:
        pass
    found = []
    try:
        for t in wavan.tones(wav):
            found.append(t)
            print('tone %.1f Hz %.2fs snr %.1f glitches %d' % (t['freq'], t['dur'], t['snr'], t['glitches']))
    except Exception as e:
        print('wav:', e)
    checks = {'agent finished': ok, 'installed': 'audiont.sys' in out or 'AUDIONT' in out}
    for w in want:
        checks['tone %d Hz' % w] = any(abs(t['freq'] - w) < w * 0.02 and t['dur'] > 0.5 and t['glitches'] == 0
                                       for t in found)
    checks['midi'] = 'midi phrase sent' in out
    if audio != 'usb':
        # Device Manager: the card's own name ("<codec> High Definition Audio (AUDIONT)"), not the INF's
        names = re.findall(r'FSTEST dev \S+ \| (.*?) \| svc=audiont', out)
        print('Device Manager:', names)
        checks['Device Manager name of the card'] = any(n.endswith('(AUDIONT)') for n in names)
    t880 = [t for t in found if abs(t['freq'] - 880) < 10 and t['dur'] > 0.5]
    checks['Wave slider (mixer API) lowers the wave level'] = (len(t880) >= 2 and t880[0]['level'] - t880[1]['level'] > 10
                                                              and 'volume 16384 -> 0' in out)
    if len(t880) >= 2:
        print('880 Hz levels %.1f / %.1f dBFS' % (t880[0]['level'], t880[1]['level']))
    if reg and ok:
        # boot 2: the driver puts audiont.dll back into Drivers32, a tone plays
        bat2 = ['start /b C:\\AUD\\FSTEST.EXE clicker C:\\AUD\\STOP 600000', 'C:\\AUD\\FSTEST.EXE sleep 8000',
                T + ' drv32', T + ' tone 523 1200 44100 16 2', 'C:\\AUD\\FSTEST.EXE sleep 500']
        if g['x64']:
            bat2 += ['C:\\AUD\\2KXP\\AMD64\\AUDTEST.EXE drv32']
        bat2 += [T + ' log', 'C:\\AUD\\FSTEST.EXE write C:\\AUD\\STOP x', 'C:\\AUD\\FSTEST.EXE sleep 2000']
        vm5.set_bat(work, bat2)
        wav2 = wav.replace('out.wav', 'out2.wav')
        if os.path.exists(wav2):
            os.remove(wav2)
        extra2 = ['-audiodev', 'wav,id=wav0,path=%s,out.frequency=48000' % wav2, '-debugcon', 'file:' + dbg + '2'] + AUDIO[audio]
        vm = vm5.VM(guest, work, extra=extra2, name=name)
        ok2 = vm.wait_serial('##AGENT-DONE##', 900, shots=60)
        out2 = vm.serial()
        vm.quit()
        print(out2[-3000:])
        t2 = wavan.tones(wav2) if os.path.exists(wav2) else []
        checks['boot 2: Drivers32 entries put back'] = ok2 and out2.count('drivers32: 4 audiont.dll entries') >= (2 if g['x64'] else 1)
        checks['boot 2: tone 523 Hz'] = any(abs(t['freq'] - 523) < 10 and t['dur'] > 0.5 for t in t2)
    for k, v in checks.items():
        print('%-16s %s' % (k, 'PASS' if v else 'FAIL'))
    if not keep:
        os.remove(work)
    return all(checks.values())


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    guest = a[0] if a else 'w2k'
    audio = a[1] if len(a) > 1 else 'hda'
    sys.exit(0 if run(guest, audio, '--keep' in sys.argv, '--reg' in sys.argv) else 1)
