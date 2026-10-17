#!/usr/bin/env python3
"""Run the playback test sequence on several emulated sound devices."""
import subprocess, sys, os, re, shutil
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import wavan

TONES = [440, 1000, 1500, 2000, 2500, 3000, 3500, 750]
BAT = '@echo off\r\nset T=C:\\AUDIONT\\TEST\\ATTEST.EXE\r\n%T% seq\r\n%T% stats\r\n'

PARAMS = 'SYSTEM\\CurrentControlSet\\Services\\audiont\\Parameters'

def run(audio, extra_bat='', qemu=None, label=None, sysfile=None, files=(), reg=None):
    bat = '/tmp/suite.bat'
    open(bat, 'w').write(extra_bat + BAT)
    args = ['python3', os.path.join(ROOT, 'tools/vm.py'), 'run', '--audio', audio, '--bat', bat,
            '--timeout', '300', '--files', 'build/attest.exe=/AUDIONT/TEST/ATTEST.EXE'] + list(files)
    if reg:
        # registry values are set in a first boot, the driver sees them in the second
        pre = '/tmp/suite-pre.bat'
        open(pre, 'w').write('@echo off\r\n' + ''.join('C:\\AUDIONT\\TEST\\ATTEST.EXE regdw %s %s %s\r\n'
                             % (PARAMS, k, v) for k, v in reg.items()))
        args += ['--prebat', pre]
    if qemu: args += ['--qemu', qemu]
    if sysfile: args += ['--sys', sysfile]
    out = subprocess.run(args, cwd=ROOT, capture_output=True, text=True).stdout
    ser = open(os.path.join(ROOT, 'vmout/serial.log'), encoding='latin1').read()
    res = wavan.tones(os.path.join(ROOT, 'vmout/out.wav'))
    ok = True; notes = []
    for e in TONES:
        m = [t for t in res if abs(t['freq'] - e) < max(2, e * 0.002)]
        if not m: ok = False; notes.append('missing %d' % e); continue
        t = m[0]
        if t['glitches'] or t['snr'] < 40: ok = False; notes.append('%d: snr %.0f gl %d' % (e, t['snr'], t['glitches']))
    if 'RESULT FAIL' in ser: ok = False; notes.append('attest reported failure')
    st = re.findall(r'stats: .*', ser)
    name = label or audio
    shutil.copy(os.path.join(ROOT, 'vmout/debugcon.log'), os.path.join(ROOT, 'vmout/debugcon-%s.log' % name))
    print('%-22s %s  %s %s' % (name, 'PASS' if ok else 'FAIL', st[0] if st else '', '; '.join(notes)))
    return ok

def parse(cfg):
    """'audio' or 'audio:Key=1,Key2=0' -> (audio, label, {key: value})"""
    if ':' not in cfg: return cfg, cfg, None
    audio, kv = cfg.split(':', 1)
    reg = dict(x.split('=', 1) for x in kv.split(','))
    return audio, cfg.replace(':', '-').replace(',', '-').replace('=', ''), reg

if __name__ == '__main__':
    cfgs = sys.argv[1:] or ['ac97', 'hda', 'ich9', 'hda-dsp', 'hda-output', 'hda-micro']
    allok = True
    for c in cfgs:
        audio, label, reg = parse(c)
        allok &= run(audio, label=label, reg=reg)
    print('SUITE', 'PASS' if allok else 'FAIL')
