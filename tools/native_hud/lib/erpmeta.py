"""Print the index metadata (type, argument, fragment names/flags/compression/sizes) of resources in an ERP.

usage: python -I erpmeta.py <archive.erp> <substring> [max]
"""
import struct
import sys
from collections import Counter


def index(path):
    with open(path, 'rb') as f:
        hdr = f.read(0x30)
        ver = struct.unpack_from('<I', hdr, 4)[0]
        info_off, info_size, data_off = struct.unpack_from('<QQQ', hdr, 0x10)
        nres, ntemp = struct.unpack('<II', f.read(8))
        out = []
        for _ in range(nres):
            struct.unpack('<I', f.read(4))
            nlen = struct.unpack('<H', f.read(2))[0]
            name = f.read(nlen).rstrip(b'\0').decode()
            typ = f.read(16)
            unk = f.read(4) + (f.read(2) if ver >= 4 else b'')
            frags = []
            for _ in range(f.read(1)[0]):
                fn = f.read(4)
                off, size, flags = struct.unpack('<QQI', f.read(20))
                comp = f.read(1)[0]
                packed = struct.unpack('<Q', f.read(8))[0]
                frags.append((fn, off, size, flags, comp, packed))
            h = f.read(16)
            out.append((name, typ, unk, frags, h))
        return hdr, out


def main():
    hdr, res = index(sys.argv[1])
    sub = sys.argv[2]
    mx = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    print('hdr', hdr.hex())
    comps = Counter()
    for name, typ, unk, frags, h in res:
        for fr in frags:
            comps[(typ.rstrip(b'\0').decode(errors='replace'), fr[4])] += 1
    n = 0
    for name, typ, unk, frags, h in res:
        if sub in name and n < mx:
            n += 1
            print(name, typ.rstrip(b'\0').decode(errors='replace'), unk.hex(), [(fr[0], fr[2], hex(fr[3]), hex(fr[4]), fr[5]) for fr in frags])
    print('comp by type:', sorted(comps.items()))


if __name__ == '__main__':
    main()
