"""Write a ready ERP archive (for an addon's erp.overlay op) from raw fragments.

Each resource copies its type argument, fragment names, flags and compression from the first stock resource of the
same type and fragment count in a reference archive, the way the launcher's add-resource does. Compressed fragments
are single zstd frames with the content size in the header and a window of at most 1 MiB (what erpw.frame_fits wants).
"""
import hashlib
import os
import struct
import sys

from compression import zstd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from erpmeta import index  # noqa: E402

ZSTD, STORED = 0x11, 0x91
_OPTS = {zstd.CompressionParameter.compression_level: 19, zstd.CompressionParameter.window_log: 20}


def compress(data):
    return zstd.compress(data, options=_OPTS)


class Archive:
    def __init__(self, reference_erp):
        self.hdr, res = index(reference_erp)
        self.layouts = {}
        self.stock = {}
        for name, typ, unk, frags, h in res:
            lay = (unk, [(fr[0], fr[3], fr[4]) for fr in frags])
            self.layouts.setdefault((typ, len(frags)), lay)
            self.stock[name] = (typ, lay)
        self.res = []
        self.names = set()

    def layout(self, name, t, nfrags, like=None):
        """A stock resource keeps its own layout (fragment names differ within a type: hud_dynamic_templates.ui
        is the one UIDF stored as 'syst'); a new one copies stock resource `like`, else the first stock resource of
        its type and size."""
        for n in (name, like):
            if n in self.stock and self.stock[n][0] == t and len(self.stock[n][1][1]) == nfrags:
                return self.stock[n][1]
        if like is not None:
            raise KeyError(f'{like}: no stock {t.rstrip(bytes(1)).decode()} with {nfrags} fragment(s) by that name')
        if (t, nfrags) not in self.layouts:
            raise KeyError(f'no stock {t.rstrip(bytes(1)).decode()} with {nfrags} fragment(s) to copy a layout from')
        return self.layouts[(t, nfrags)]

    def add(self, name, rtype, frags, like=None):
        t = rtype.encode().ljust(16, b'\0')
        if name in self.names:
            raise KeyError(f'duplicate resource {name}')
        self.names.add(name)
        unk, lay = self.layout(name, t, len(frags), like)
        out = []
        for (fname, flags, comp), data in zip(lay, frags):
            raw = compress(data) if comp == ZSTD else data
            out.append((fname, flags, comp, len(data), raw))
        self.res.append((name, t, unk, out, hashlib.md5(b''.join(frags)).digest()))

    def write(self, path):
        idx = bytearray()
        off = nfr = 0
        for name, t, unk, frags, h in self.res:
            nm = name.encode() + b'\0'
            e = bytearray(struct.pack('<H', len(nm)) + nm + t + unk + bytes([len(frags)]))
            for fname, flags, comp, size, raw in frags:
                e += fname + struct.pack('<QQI', off, size, flags) + bytes([comp]) + struct.pack('<Q', len(raw))
                off += len(raw)
                nfr += 1
            e += h
            idx += struct.pack('<I', len(e)) + e
        info = struct.pack('<II', len(self.res), nfr) + idx
        hdr = bytearray(self.hdr)
        struct.pack_into('<QQQ', hdr, 0x10, 0x30, len(info), 0x30 + len(info))
        with open(path, 'wb') as f:
            f.write(hdr)
            f.write(info)
            for name, t, unk, frags, h in self.res:
                for fr in frags:
                    f.write(fr[4])
        return os.path.getsize(path)


def read_all(path):
    """{name: (type, [fragment bytes])} of an archive, decompressed (for checks)."""
    hdr, res = index(path)
    data_off = struct.unpack_from('<QQQ', hdr, 0x10)[2]
    out = {}
    with open(path, 'rb') as f:
        for name, typ, unk, frags, h in res:
            datas = []
            for fn, off, size, flags, comp, packed in frags:
                f.seek(data_off + off)
                raw = f.read(packed)
                d = zstd.decompress(raw) if comp == ZSTD else raw
                assert len(d) == size, (name, len(d), size)
                datas.append(d)
            assert hashlib.md5(b''.join(datas)).digest() == h, name
            out[name] = (typ.rstrip(b'\0').decode(), datas)
    return out
