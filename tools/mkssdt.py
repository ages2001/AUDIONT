#!/usr/bin/env python3
"""Builds an SSDT with I2C codec / amplifier devices for VM tests
(qemu ... -acpitable file=ssdt.aml):

  mkssdt.py out.aml --ctrl '\\_SB.PCI0.S28_' HID@ADDR[:UID] ...   (ADDR 0: no I2C, GPIO only)

Each device gets _HID, _UID and a _CRS with I2cSerialBusV2 (400 kHz) on the
controller path; --gpio CTRL:PIN adds a GpioIo line to every device.

SoundWire (Intel): --sdw-hda ADR emits Device (HDAS) with that PCI _ADR
(dev << 16 | fn of the intel-hda device) holding the SoundWire controller
Device (SNDW) (_ADR 0x40000000, _DSD mipi-sdw-master-count and the
mipi-sdw-link-N-subproperties packages LNKn: intel-sdw-ip-clock, clock
frequencies, frame rate / rows) and one Device (SWDn) per --sdw peripheral
LINK:MFG:PART:CLASS:UID[:SOURCEPORTS:SINKPORTS] with the 64-bit _ADR and
the DisCo port lists:

  mkssdt.py sdw.aml --sdw-hda 0x000E0000 --sdw 0:25d:711:1:0:14:8 --sdw 1:25d:1316:1:1:4:2

--sdw-func N:TYPE adds an SDCA function (DisCo: Entity 0 Function_Topology
DC value TYPE, an entity list with Power Domain, Feature Unit, Clock Source
(sample rate range 44.1 kHz -> 8, 48 kHz -> 12; jack: also a Group Entity))
to the N-th --sdw peripheral, e.g. --sdw-func 2:1 (SmartAmp), 6 (UAJ), 3 (SmartMic).
"""
import argparse
import struct
import uuid

UUID_DEV = uuid.UUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301").bytes_le     # device properties
UUID_HIER = uuid.UUID("dbb8e3e6-5886-4ba6-8795-1319f52a966b").bytes_le    # hierarchical data extension


def pkglen(n):
    if n + 1 < 64:
        return bytes([n + 1])
    for k in (2, 3, 4):
        v = n + k
        if v < (1 << (4 + 8 * (k - 1))):
            out = [((k - 1) << 6) | (v & 0x0F)]
            v >>= 4
            for _ in range(k - 1):
                out.append(v & 0xFF)
                v >>= 8
            return bytes(out)
    raise ValueError


def i2c_desc(addr, ctrl):
    src = ctrl.encode() + b"\0"
    d = bytes([2, 0, 1, 0x02, 0, 0, 1]) + struct.pack("<HIH", 6, 400000, addr) + src
    return bytes([0x8E]) + struct.pack("<H", len(d)) + d


def gpio_desc(ctrl, pin):
    src = ctrl.encode() + b"\0"
    # GpioIo: rev 1, type 1 (I/O), flags, io flags (output), pull none, drive, debounce, pin table, source
    pt = 23
    rn = pt + 2
    vd = rn + len(src)
    d = bytes([1, 1]) + struct.pack("<H", 0) + struct.pack("<H", 2) + bytes([0]) + struct.pack("<HHH", 0, 0, pt) + \
        bytes([0]) + struct.pack("<HHH", rn, vd, 0) + struct.pack("<H", pin) + src
    return bytes([0x8C]) + struct.pack("<H", len(d)) + d


def name4(s):
    s = (s + "____")[:4]
    return s.encode()


def device(dname, hid, uid, descs):
    buf = b"".join(descs) + bytes([0x79, 0])
    bufop = bytes([0x0A, len(buf)]) if len(buf) < 256 else bytes([0x0B]) + struct.pack("<H", len(buf))
    crs = b"\x08_CRS" + b"\x11" + pkglen(len(bufop + buf)) + bufop + buf
    body = name4(dname) + b"\x08_HID\x0D" + hid.encode() + b"\0" + b"\x08_UID\x0A" + bytes([uid]) + crs
    return b"\x5B\x82" + pkglen(len(body)) + body


def fieldlen(v):
    return bytes([v]) if v < 64 else bytes([0x40 | (v & 0xF), v >> 4])


def lpe_device(hid, nvs):
    """an ACPI-mode audio DSP: _CRS fills Memory32Fixed bases from NVS fields (LPE0 / LPE1 at nvs, nvs + 4)"""
    m32 = lambda ln: bytes([0x86, 9, 0, 1]) + struct.pack("<II", 0, ln)
    buf = m32(0x200000) + m32(0x1000) + bytes([0x79, 0])
    rbuf = b"\x08RBUF\x11" + pkglen(2 + len(buf)) + bytes([0x0A, len(buf)]) + buf
    body = rbuf + b"\x8ARBUF\x0A\x04B0BA" + b"\x8ARBUF\x0A\x10B1BA" + b"\x70LPE0B0BA" + b"\x70LPE1B1BA" + b"\xA4RBUF"
    meth = b"_CRS\x08" + body
    crs = b"\x14" + pkglen(len(meth)) + meth
    dev = b"LPEA" + b"\x08_HID\x0D" + hid.encode() + b"\0" + b"\x08_UID\x01" + crs
    out = b"\x5B\x82" + pkglen(len(dev)) + dev
    region = b"\x5B\x80GNVS\x00\x0C" + struct.pack("<I", nvs - 0x10) + b"\x0B\x00\x01"
    flist = b"GNVS\x00" + b"\x00" + fieldlen(0x80) + b"LPE0" + fieldlen(32) + b"LPE1" + fieldlen(32)
    field = b"\x5B\x81" + pkglen(len(flist)) + flist
    return out + region + field


def aml_int(v):
    if v == 0:
        return b"\x00"
    if v == 1:
        return b"\x01"
    if v < 0x100:
        return bytes([0x0A, v])
    if v < 0x10000:
        return b"\x0B" + struct.pack("<H", v)
    if v < 0x100000000:
        return b"\x0C" + struct.pack("<I", v)
    return b"\x0E" + struct.pack("<Q", v)


def aml_str(t):
    return b"\x0D" + t.encode() + b"\0"


def aml_pkg(elems):
    body = bytes([len(elems)]) + b"".join(elems)
    return b"\x12" + pkglen(len(body)) + body


def aml_buf(data):
    body = aml_int(len(data)) + data
    return b"\x11" + pkglen(len(body)) + body


def aml_obj(v):
    if isinstance(v, int):
        return aml_int(v)
    if isinstance(v, str):
        return aml_str(v)
    if isinstance(v, (list, tuple)):
        return aml_pkg([aml_obj(x) for x in v])
    return v


def props(pairs):
    """Package (2) { ToUUID (device properties), Package () { Package (2) { name, value } .. } }"""
    return aml_pkg([aml_buf(UUID_DEV), aml_pkg([aml_pkg([aml_str(k), aml_obj(v)]) for k, v in pairs])])


def aml_name(n, obj):
    return b"\x08" + name4(n) + obj


def aml_device(n, body):
    body = name4(n) + body
    return b"\x5B\x82" + pkglen(len(body)) + body


def sdca_function(adr, ftype):
    """Device (AFnn): an SDCA function in DisCo form"""
    ents = [(0x01, 0x0B), (0x05, 0x07), (0x10, 0x11)]                  # CS, FU, PDE
    if ftype in (6, 7, 8):
        ents += [(0x06, 0x07), (0x11, 0x11), (0x12, 0x12)]              # jack: second FU / PDE, GE
    hier = [aml_pkg([aml_str("mipi-sdca-control-0x5-subproperties"), aml_str("C005")])]
    names = aml_name("C005", props([("mipi-sdca-control-access-mode", 5), ("mipi-sdca-control-dc-value", ftype)]))
    for eid, etype in ents:
        en = "E%03X" % eid
        hier.append(aml_pkg([aml_str("mipi-sdca-entity-id-0x%x-subproperties" % eid), aml_str(en)]))
        if etype == 0x0B:
            rng = struct.pack("<HH", 2, 2) + struct.pack("<IIII", 8, 44100, 12, 48000)    # not the usual 9: the driver must read it
            names += aml_name(en, aml_pkg([aml_buf(UUID_DEV), aml_pkg([aml_pkg([aml_str("mipi-sdca-entity-type"),
                                                                                 aml_int(etype)])]),
                                           aml_buf(UUID_HIER), aml_pkg([aml_pkg([aml_str("mipi-sdca-control-0x10-subproperties"),
                                                                                  aml_str("R%03X" % eid)])])]))
            names += aml_name("R%03X" % eid, aml_pkg([aml_buf(UUID_DEV), aml_pkg([aml_pkg([aml_str("mipi-sdca-control-range"),
                                                                                         aml_buf(rng)])])]))
        else:
            names += aml_name(en, props([("mipi-sdca-entity-type", etype)]))
    dsd = aml_pkg([aml_buf(UUID_DEV), aml_pkg([aml_pkg([aml_str("mipi-sdca-entity-id-list"),
                                                        aml_obj([e for e, _ in ents])])]),
                   aml_buf(UUID_HIER), aml_pkg(hier)])
    return aml_device("AF%02X" % adr, aml_name("_ADR", aml_int(adr)) + aml_name("_DSD", dsd) + names)


def sdw_devices(hda, links, ipclk, busclk, rows, periphs, funcs=()):
    sub = []
    lnk = b""
    for l in range(links):
        sub.append(aml_pkg([aml_str("mipi-sdw-link-%d-subproperties" % l), aml_str("LNK%d" % l)]))
        lnk += aml_name("LNK%d" % l, props([("intel-sdw-ip-clock", ipclk),
                                            ("mipi-sdw-clock-frequencies-supported", [busclk]),
                                            ("mipi-sdw-default-frame-rate", 48000),
                                            ("mipi-sdw-default-frame-row-size", rows),
                                            ("intel-quirk-mask", 0)]))
    dsd = aml_pkg([aml_buf(UUID_DEV), aml_pkg([aml_pkg([aml_str("mipi-sdw-master-count"), aml_int(links)])]),
                   aml_buf(UUID_HIER), aml_pkg(sub)])
    body = aml_name("_ADR", aml_int(0x40000000)) + aml_name("_DSD", dsd) + lnk
    for i, spec in enumerate(periphs):
        f = [int(x, 16) if k else int(x, 0) for k, x in enumerate(spec.split(":"))]
        link, mfg, part, cls, uid = f[:5]
        src = f[5] if len(f) > 5 else 0
        sink = f[6] if len(f) > 6 else 0
        adr = (link << 48) | (3 << 44) | (uid << 40) | (mfg << 24) | (part << 8) | cls
        pp = [("mipi-sdw-sw-interface-revision", 0x20000), ("mipi-sdw-paging-supported", 1)]
        if src:
            pp.append(("mipi-sdw-source-port-list", src))
        if sink:
            pp.append(("mipi-sdw-sink-port-list", sink))
        fn = b"".join(sdca_function(k + 1, t) for k, t in enumerate([t for n, t in funcs if n == i]))
        body += aml_device("SWD%d" % i, aml_name("_ADR", aml_int(adr)) + aml_name("_DSD", props(pp)) + fn)
    sndw = aml_device("SNDW", body)
    return aml_device("HDAS", aml_name("_ADR", aml_int(hda)) + sndw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--ctrl", default="\\_SB.PCI0.S28_")
    ap.add_argument("--gpio", default=None)
    ap.add_argument("--lpe", default=None, help="HID@NVS: ACPI-mode audio DSP with its base in NVS")
    ap.add_argument("--sdw-hda", default=None, help="PCI _ADR of the HD Audio device (SoundWire controller parent)")
    ap.add_argument("--sdw", action="append", default=[], help="LINK:MFG:PART:CLASS:UID[:SRC:SINK] (hex ids)")
    ap.add_argument("--sdw-func", action="append", default=[], help="N:TYPE SDCA function on the N-th --sdw peripheral")
    ap.add_argument("--sdw-links", type=int, default=4)
    ap.add_argument("--sdw-ipclk", type=int, default=38400000)
    ap.add_argument("--sdw-busclk", type=int, default=4800000)
    ap.add_argument("--sdw-rows", type=int, default=50)
    ap.add_argument("dev", nargs="*")
    a = ap.parse_args()
    body = b""
    if a.sdw_hda:
        funcs = [tuple(int(x, 0) for x in f.split(":")) for f in a.sdw_func]
        body += sdw_devices(int(a.sdw_hda, 0), a.sdw_links, a.sdw_ipclk, a.sdw_busclk, a.sdw_rows, a.sdw, funcs)
    for i, spec in enumerate(a.dev):
        hid, rest = spec.split("@")
        addr, _, uid = rest.partition(":")
        descs = []
        if int(addr, 0):
            descs.append(i2c_desc(int(addr, 0), a.ctrl))
        if a.gpio:
            c, p = a.gpio.rsplit(":", 1)
            descs.append(gpio_desc(c, int(p, 0)))
        body += device("CD%02d" % i, hid, int(uid, 0) if uid else 0, descs)
    if a.lpe:
        hid, nvs = a.lpe.split("@")
        body += lpe_device(hid, int(nvs, 0))
    length = 36 + len(body)
    hdr = b"SSDT" + struct.pack("<IB", length, 2) + b"\0" + b"AUDNT " + b"CODECS  " + struct.pack("<I", 1) + \
        b"ADNT" + struct.pack("<I", 1)
    t = bytearray(hdr + body)
    t[9] = (-sum(t)) & 0xFF
    open(a.out, "wb").write(bytes(t))
    print("%s: %u bytes, %u devices, %u SoundWire peripherals" % (a.out, len(t), len(a.dev), len(a.sdw)))


if __name__ == "__main__":
    main()
