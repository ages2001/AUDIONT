#!/usr/bin/env python3
"""AUDIONT post-link tool.

* Verifies that every import of a kernel driver exists in the NT 3.1
  ntoskrnl.exe / hal.dll export tables (so one binary loads on NT 3.1-4.0).
* For user-mode images verifies imports against an allow-list of DLLs.
* Rejects TLS directories and unexpected DLL dependencies.
* Stamps OS/subsystem versions and recomputes the PE checksum.
"""
import sys, os, pefile

HERE = os.path.dirname(os.path.abspath(__file__))

def load_exports(name):
    with open(os.path.join(HERE, name)) as f:
        return set(x.strip() for x in f if x.strip())

def main():
    path, kind = sys.argv[1], sys.argv[2]          # kind: driver | user
    pe = pefile.PE(path)
    ok = True
    if kind == 'driver':
        allowed = {'ntoskrnl.exe': load_exports('NTOSKRNL.EXE.exports'),
                   'hal.dll': load_exports('HAL.DLL.exports')}
    else:
        allowed = {}
        for n in ['KERNEL32.DLL','USER32.DLL','ADVAPI32.DLL','GDI32.DLL','WINMM.DLL','COMDLG32.DLL','SHELL32.DLL']:
            allowed[n.lower()] = load_exports(n + '.exports')
        if len(sys.argv) > 3:   # extra DLLs without verification (e.g. our own)
            for n in sys.argv[3].lower().split(','):
                allowed[n] = None
    if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            dll = d.dll.decode().lower()
            if allowed is not None:
                if dll not in allowed:
                    print('ERROR: unexpected import DLL', dll); ok = False; continue
                for imp in d.imports:
                    n = imp.name.decode() if imp.name else None
                    if allowed[dll] is None: continue
                    if n is None or n not in allowed[dll]:
                        print('ERROR: %s!%s not exported by NT 3.1' % (dll, n)); ok = False
    tls = pe.OPTIONAL_HEADER.DATA_DIRECTORY[9]
    if tls.VirtualAddress:
        print('ERROR: image has a TLS directory'); ok = False
    oh = pe.OPTIONAL_HEADER
    if kind == 'driver':
        oh.MajorOperatingSystemVersion, oh.MinorOperatingSystemVersion = 1, 0
        oh.MajorSubsystemVersion, oh.MinorSubsystemVersion = 1, 0
        oh.Subsystem = 1
        oh.DllCharacteristics = 0
    else:
        oh.MajorOperatingSystemVersion, oh.MinorOperatingSystemVersion = 1, 0
        oh.MajorSubsystemVersion, oh.MinorSubsystemVersion = 3, 10
        oh.DllCharacteristics = 0
    oh.MajorImageVersion, oh.MinorImageVersion = 1, 0
    oh.CheckSum = 0
    data = bytearray(pe.write())
    pe.close()
    pe = pefile.PE(data=bytes(data))
    pe.OPTIONAL_HEADER.CheckSum = pe.generate_checksum()
    data = pe.write()
    pe.close()
    if not ok:
        sys.exit(1)
    with open(path, 'wb') as f:
        f.write(data)

if __name__ == '__main__':
    main()
