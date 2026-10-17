#!/usr/bin/env python3
"""Builds an NHLT ACPI table for tests in a virtual machine
(qemu ... -acpi-table file=nhlt.bin).

  mknhlt.py nhlt.bin [--mics 2|4] [--ssp PORT ...] [--no-dmic]

The DMIC endpoint carries 16-bit and 32-bit 48 kHz formats, an SSP port a
render and a capture endpoint (analog codec) with 24-in-32-bit 48 kHz stereo.
The configuration blobs are placeholders: only a DSP model accepts them.
"""
import argparse
import struct

KSDATAFORMAT_SUBTYPE_PCM = bytes.fromhex('0100000000001000800000aa00389b71')


def wavext(ch, rate, bits, valid, mask):
    block = ch * bits // 8
    return struct.pack('<HHIIHHHHI', 0xFFFE, ch, rate, rate * block, block, bits, 22, valid, mask) + \
        KSDATAFORMAT_SUBTYPE_PCM


def fmtcfg(ch, rate, bits, valid, mask, blob):
    return wavext(ch, rate, bits, valid, mask) + struct.pack('<I', len(blob)) + blob


def endpoint(link, dev_type, direction, bus, caps, fmts, vid=0x8086, did=0xAE20):
    body = struct.pack('<BBHHHIBBB', link, 0, vid, did, 1, 0, dev_type, direction, bus)
    body += struct.pack('<I', len(caps)) + caps
    body += struct.pack('<B', len(fmts)) + b''.join(fmts)
    return struct.pack('<I', 4 + len(body)) + body


def ssp_blob(port):
    """SSP blob v1.0 (84 bytes): I2S, 2 x 32 bit slots, 24 bit data, MCLK 0 on"""
    w = [0] * 21
    w[9] = (1 << 20) | (8 - 1) | (3 << 4) | (3 << 8)          # SSCR0: EDSS + DSS -> 24 bit, PSP, SCR
    w[12] = (32 << 16)                                        # SSPSP: SFRMWDTH 32 -> 64 bit frames
    w[19] = 1                                                 # MDIVC: MCLK 0
    return struct.pack('<21I', *w)


def blob(n, seed):
    return bytes((seed + i * 7) & 0xFF for i in range(n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--mics', type=int, default=2)
    ap.add_argument('--ssp', type=int, action='append', default=[])
    ap.add_argument('--amp', type=int, action='append', default=[], help='render-only SSP port (amplifiers)')
    ap.add_argument('--no-dmic', action='store_true')
    a = ap.parse_args()
    eps = []
    if not a.no_dmic:
        arr = 0x0A if a.mics == 2 else 0x0D
        caps = struct.pack('<BBB', 0, 1, arr)
        m = a.mics
        mask = 0x3 if m == 2 else 0x33
        fmts = [fmtcfg(m, 48000, 16, 16, mask, blob(0x9C, 1)),
                fmtcfg(m, 48000, 32, 32, mask, blob(0xB8, 2))]
        if m == 4:
            fmts.append(fmtcfg(2, 48000, 32, 32, 0x3, blob(0xB8, 3)))
        eps.append(endpoint(2, 0, 1, 0, caps, fmts))
    for port in a.ssp:
        for d in (0, 1):
            caps = struct.pack('<BBB', 0, 0, 0)
            fmts = [fmtcfg(2, 48000, 32, 24, 0x3, ssp_blob(port))]
            eps.append(endpoint(3, 4, d, port, caps, fmts))
    for port in a.amp:
        caps = struct.pack('<BBB', 0, 0, 0)
        eps.append(endpoint(3, 4, 0, port, caps, [fmtcfg(2, 48000, 32, 24, 0x3, ssp_blob(port))]))
    body = struct.pack('<B', len(eps)) + b''.join(eps)
    length = 36 + len(body)
    hdr = bytearray(b'NHLT' + struct.pack('<IBB', length, 0, 0) + b'AUDNT ' + b'AUDIONT ' +
                    struct.pack('<I', 1) + b'ADNT' + struct.pack('<I', 1))
    tbl = hdr + body
    tbl[9] = (-sum(tbl)) & 0xFF
    open(a.out, 'wb').write(bytes(tbl))
    print('%s: %u bytes, %u endpoints' % (a.out, len(tbl), len(eps)))


if __name__ == '__main__':
    main()
