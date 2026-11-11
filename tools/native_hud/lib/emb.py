"""GfxMatBindRes (.emb) reader/writer: mesh material slot name -> .material resource.

Layout (worked out from the stock files, checked by round trip):
  u32 n_slots, u32 n_unique_materials, u32 mat_block_off (relative to 0x0c)
  n_slots x { u32 slot_name_off (in the slot block), u32 material_off (in the material block) }
  slot block: NUL-terminated slot names
  material block: NUL-terminated material paths (deduplicated)

usage: python -I emb.py check <file.emb>...   |   python -I emb.py dump <file.emb>
"""
import struct
import sys


def cstr(b, off):
    return b[off:b.index(b"\0", off)].decode()


def parse(b):
    n, nm, moff = struct.unpack_from("<3I", b, 0)
    recs = [struct.unpack_from("<2I", b, 12 + 8 * i) for i in range(n)]
    sbase = 12 + 8 * n
    mbase = 12 + moff
    return [(cstr(b, sbase + s), cstr(b, mbase + m)) for s, m in recs]


def write(pairs):
    sblock = bytearray()
    soffs = {}
    for slot, _ in pairs:
        if slot not in soffs:
            soffs[slot] = len(sblock)
            sblock += slot.encode() + b"\0"
    mblock = bytearray()
    moffs = {}
    for _, mat in pairs:
        if mat not in moffs:
            moffs[mat] = len(mblock)
            mblock += mat.encode() + b"\0"
    n = len(pairs)
    out = bytearray(struct.pack("<3I", n, len(moffs), 8 * n + len(sblock)))
    for slot, mat in pairs:
        out += struct.pack("<2I", soffs[slot], moffs[mat])
    out += sblock + mblock
    return bytes(out)


def main():
    cmd = sys.argv[1]
    if cmd == "check":
        bad = 0
        for f in sys.argv[2:]:
            b = open(f, "rb").read()
            try:
                ok = write(parse(b)) == b
            except Exception as e:  # noqa: BLE001
                ok = False
                print("ERR", f, e)
            bad += not ok
            if not ok:
                print("DIFF", f)
        print(f"checked {len(sys.argv) - 2}, bad {bad}")
    elif cmd == "dump":
        for slot, mat in parse(open(sys.argv[2], "rb").read()):
            print(f"{slot:40s} {mat}")


if __name__ == "__main__":
    main()
