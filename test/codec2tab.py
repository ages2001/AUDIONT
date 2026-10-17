#!/usr/bin/env python3
"""Convert a Linux /proc/asound/card*/codec#* dump into a flat table for thda."""
import re, sys
def conv(path):
    txt = open(path, errors='replace').read()
    # only the first codec section with an audio function group
    out = []
    vendor = re.search(r'Vendor Id:\s*(0x[0-9a-fA-F]+)', txt)
    rev = re.search(r'Revision Id:\s*(0x[0-9a-fA-F]+)', txt)
    sub = re.search(r'Subsystem Id:\s*(0x[0-9a-fA-F]+)', txt)
    if not vendor: return None
    def amp(s):
        if s is None or 'N/A' in s: return 0
        m = re.search(r'ofs=(0x[0-9a-f]+), nsteps=(0x[0-9a-f]+), stepsize=(0x[0-9a-f]+), mute=(\d)', s)
        if not m: return 0
        return int(m.group(1), 16) | (int(m.group(2), 16) << 8) | (int(m.group(3), 16) << 16) | (int(m.group(4)) << 31)
    dai = re.search(r'Default Amp-In caps:(.*)', txt); dao = re.search(r'Default Amp-Out caps:(.*)', txt)
    out.append('V %s %s %s' % (vendor.group(1), rev.group(1) if rev else '0', sub.group(1) if sub else '0'))
    out.append('D %d %d' % (amp(dai.group(1) if dai else None), amp(dao.group(1) if dao else None)))
    nodes = re.split(r'\nNode ', '\nNode ' + txt.split('\nNode ', 1)[1]) if '\nNode ' in txt else []
    for nd in nodes[1:]:
        m = re.match(r'(0x[0-9a-f]+) \[.*?\] wcaps (0x[0-9a-f]+)', nd)
        if not m: continue
        nid = int(m.group(1), 16); wc = int(m.group(2), 16)
        pc = re.search(r'Pincap (0x[0-9a-f]+)', nd); cfg = re.search(r'Pin Default (0x[0-9a-f]+)', nd)
        ai = re.search(r'Amp-In caps:(.*)', nd); ao = re.search(r'Amp-Out caps:(.*)', nd)
        conn = []
        cm = re.search(r'Connection: (\d+)[ \t]*\n\s+((?:0x[0-9a-f]+\*?[ \t]*)+)', nd)
        if cm: conn = [int(x.rstrip('*'), 16) for x in cm.group(2).split()]
        out.append('N %d %d %d %d %d %d %d %s' % (nid, wc, int(pc.group(1), 16) if pc else 0,
                   int(cfg.group(1), 16) if cfg else 0, amp(ai.group(1) if ai else None),
                   amp(ao.group(1) if ao else None), len(conn), ' '.join(str(c) for c in conn)))
        if nid > 0x7f: break
    return '\n'.join(out) + '\n'
if __name__ == '__main__':
    # one table per codec section
    full = open(sys.argv[1], errors='replace').read()
    parts = re.split(r'\n(?=Codec: )', full)
    n = 0
    import os
    for part in parts:
        if 'Vendor Id' not in part or '\nNode ' not in part: continue
        tmp = '/tmp/_codec_part.txt'
        open(tmp, 'w').write(part)
        t = conv(tmp)
        if t:
            open('%s.%d' % (sys.argv[2], n), 'w').write(t); n += 1
