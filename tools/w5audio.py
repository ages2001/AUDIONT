#!/usr/bin/env python3
"""AUDIONT on Windows 2000 (x86) and Windows XP x64: Plug and Play
installation with setup.exe, then tones and MIDI through audiont.dll
(32 bit and, on x64, 64 bit programs), captured with QEMU's wav backend.

  python3 w5audio.py [w2k|xp64] [hda|ac97|ich9] [--keep]

Uses the Windows 2000 / XP guests of USBNT's test/vm5.py.
"""
import os, sys, time, subprocess
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
         (FST, '/AUD/FSTEST.EXE')]
    for n in ('audiont.dll', 'setup.exe', 'audmix.exe', 'audtest.exe'):
        f.append((os.path.join(B, '2kxp', 'amd64', n), '/AUD/2KXP/AMD64/' + n.upper()))
    return f


def run(guest, audio, keep=False):
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
        'C:\\AUD\\AUDTEST.EXE tone 440 1500 44100 16 2',
        'C:\\AUD\\FSTEST.EXE sleep 500',
        'C:\\AUD\\AUDTEST.EXE tone 1000 1000 22050 16 1',
        'C:\\AUD\\FSTEST.EXE sleep 500',
    ]
    want = [440, 1000]
    if g['x64']:
        bat += ['C:\\AUD\\2KXP\\AMD64\\AUDTEST.EXE info',
                'C:\\AUD\\2KXP\\AMD64\\AUDTEST.EXE tone 660 1200 48000 16 2',
                'C:\\AUD\\FSTEST.EXE sleep 500']
        want.append(660)
    bat += ['C:\\AUD\\AUDTEST.EXE midi',
            'C:\\AUD\\AUDTEST.EXE stats',
            'C:\\AUD\\AUDTEST.EXE log',
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
    for k, v in checks.items():
        print('%-16s %s' % (k, 'PASS' if v else 'FAIL'))
    if not keep:
        os.remove(work)
    return all(checks.values())


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    guest = a[0] if a else 'w2k'
    audio = a[1] if len(a) > 1 else 'hda'
    sys.exit(0 if run(guest, audio, '--keep' in sys.argv) else 1)
