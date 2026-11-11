"""BC7 for the game's UI textures (GfxSurfaceRes format 70).

encode(rgba)        -> bytes   mode 6 (one subset, 7.7.7.7 + p-bit endpoints, 4-bit indices), PCA start,
                               weighted least-squares refinement; RGB error is weighted by alpha so the
                               visible (blended) colour is what gets fitted
decode6(buf, w, h)  -> array   fast vectorised decoder for mode-6-only data (checks our own output)
decode(buf, w, h)   -> array   every mode, one block at a time (reads stock textures; slow)

Blocks are row-major 4x4 tiles, 16 bytes each, bits LSB first, as D3D lays them out.
"""
import numpy as np

from bc7_tables import P2, P3

W4 = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int32)


def pad4(img):
    h, w = img.shape[:2]
    H, W = (h + 3) & ~3, (w + 3) & ~3
    if (H, W) == (h, w):
        return img
    out = np.zeros((H, W, 4), np.uint8)
    out[:h, :w] = img
    out[h:, :w] = img[h - 1:h, :, :] * 0
    return out


def to_blocks(img):
    H, W = img.shape[:2]
    return img.reshape(H // 4, 4, W // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)


def from_blocks(b, H, W):
    return b.reshape(H // 4, W // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(H, W, 4)


def _weights(x):
    # x: (N,16,4) float. RGB matters in proportion to alpha (straight alpha, blended); alpha always matters.
    a = x[..., 3:4] / 255.0
    w = np.empty_like(x)
    w[..., :3] = 0.03 + a
    w[..., 3] = 1.0
    return w


def _quantize(e):
    """e: (N,4) float endpoint -> c7 (N,4) int, p (N,) int, rec (N,4) int."""
    best_err = None
    for p in (0, 1):
        c7 = np.clip(np.rint((e - p) / 2.0), 0, 127).astype(np.int32)
        rec = (c7 << 1) | p
        err = ((rec - e) ** 2).sum(1)
        if best_err is None:
            best_err, bc7, bp, brec = err, c7, np.full(len(e), p, np.int32), rec
        else:
            m = err < best_err
            best_err = np.where(m, err, best_err)
            bc7 = np.where(m[:, None], c7, bc7)
            bp = np.where(m, p, bp)
            brec = np.where(m[:, None], rec, brec)
    return bc7, bp, brec


def _palette(r0, r1):
    # r0, r1: (N,4) int reconstructed 8-bit endpoints -> (N,16,4) int
    w = W4[None, :, None]
    return ((64 - w) * r0[:, None, :] + w * r1[:, None, :] + 32) >> 6


def _assign(x, wt, pal):
    # x, wt: (N,16,4); pal: (N,16,4) -> idx (N,16), err (N,)
    d = ((x[:, :, None, :] - pal[:, None, :, :]) ** 2 * wt[:, :, None, :]).sum(-1)  # (N,16px,16pal)
    idx = d.argmin(-1)
    err = np.take_along_axis(d, idx[..., None], -1)[..., 0].sum(-1)
    return idx, err


def _lsq(x, wt, idx, e0, e1):
    # weighted least squares endpoints for fixed indices, per channel
    t = (W4[idx] / 64.0)[..., None]  # (N,16,1)
    u = 1.0 - t
    a11 = (wt * u * u).sum(1)
    a12 = (wt * u * t).sum(1)
    a22 = (wt * t * t).sum(1)
    b1 = (wt * u * x).sum(1)
    b2 = (wt * t * x).sum(1)
    det = a11 * a22 - a12 * a12
    ok = np.abs(det) > 1e-6
    sd = np.where(ok, det, 1.0)
    n0 = (b1 * a22 - b2 * a12) / sd
    n1 = (a11 * b2 - a12 * b1) / sd
    n0 = np.where(ok, n0, e0)
    n1 = np.where(ok, n1, e1)
    return np.clip(n0, 0, 255), np.clip(n1, 0, 255)


def _encode_chunk(xb):
    x = xb.astype(np.float64)
    wt = _weights(x)
    n = len(x)
    # start: principal axis of the weighted pixels
    sw = wt.sum(1)
    mean = (x * wt).sum(1) / sw
    d = (x - mean[:, None, :]) * np.sqrt(wt)
    cov = np.einsum('npi,npj->nij', d, d)
    v = np.ones((n, 4)) / 2.0
    for _ in range(8):
        v = np.einsum('nij,nj->ni', cov, v)
        v /= np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-12)
    proj = np.einsum('npc,nc->np', x - mean[:, None, :], v)
    e0 = np.clip(mean + v * proj.min(1, keepdims=True), 0, 255)
    e1 = np.clip(mean + v * proj.max(1, keepdims=True), 0, 255)

    best = None
    for it in range(4):
        c0, p0, r0 = _quantize(e0)
        c1, p1, r1 = _quantize(e1)
        idx, err = _assign(x, wt, _palette(r0, r1))
        cand = (err, c0, p0, c1, p1, idx)
        if best is None:
            best = cand
        else:
            m = err < best[0]
            best = tuple(np.where(m.reshape((-1,) + (1,) * (a.ndim - 1)), a, b) for a, b in zip(cand, best))
        if it < 3:
            e0, e1 = _lsq(x, wt, idx, e0, e1)
    err, c0, p0, c1, p1, idx = best

    # anchor: pixel 0's index must have its top bit clear
    flip = idx[:, 0] >= 8
    c0, c1 = np.where(flip[:, None], c1, c0), np.where(flip[:, None], c0, c1)
    p0, p1 = np.where(flip, p1, p0), np.where(flip, p0, p1)
    idx = np.where(flip[:, None], 15 - idx, idx)

    lo = np.full(n, 1 << 6, np.uint64)
    off = 7
    for ch in range(4):
        for c in (c0, c1):
            lo |= c[:, ch].astype(np.uint64) << np.uint64(off)
            off += 7
    lo |= p0.astype(np.uint64) << np.uint64(63)
    hi = p1.astype(np.uint64)
    hi |= idx[:, 0].astype(np.uint64) << np.uint64(1)
    for i in range(1, 16):
        hi |= idx[:, i].astype(np.uint64) << np.uint64(4 + 4 * (i - 1))
    return np.stack([lo, hi], 1).astype('<u8').tobytes(), err


def encode(rgba, chunk=4096):
    """rgba: HxWx4 uint8, H and W multiples of 4 (see pad4). Returns the BC7 bytes."""
    H, W = rgba.shape[:2]
    assert H % 4 == 0 and W % 4 == 0, (H, W)
    b = to_blocks(rgba)
    out = []
    for i in range(0, len(b), chunk):
        data, _ = _encode_chunk(b[i:i + chunk])
        out.append(data)
    return b''.join(out)


def decode6(buf, w, h):
    q = np.frombuffer(buf, '<u8').reshape(-1, 2)
    lo, hi = q[:, 0], q[:, 1]
    assert np.all((lo & np.uint64(0x7F)) == np.uint64(64)), 'not all mode 6'

    def f(v, o, n):
        return ((v >> np.uint64(o)) & np.uint64((1 << n) - 1)).astype(np.int32)

    c = [f(lo, 7 + 7 * k, 7) for k in range(8)]
    p0 = f(lo, 63, 1)
    p1 = f(hi, 0, 1)
    r0 = np.stack([(c[2 * ch] << 1) | p0 for ch in range(4)], 1)
    r1 = np.stack([(c[2 * ch + 1] << 1) | p1 for ch in range(4)], 1)
    idx = np.stack([f(hi, 1, 3)] + [f(hi, 4 + 4 * (i - 1), 4) for i in range(1, 16)], 1)
    pal = _palette(r0, r1)
    px = np.take_along_axis(pal, idx[..., None], 1).astype(np.uint8)
    H, W = (h + 3) & ~3, (w + 3) & ~3
    return from_blocks(px, H, W)[:h, :w]


BITS = [[4, 6, 5, 7, 5, 7, 7, 5], [0, 0, 0, 0, 6, 8, 7, 5]]
W2 = [0, 21, 43, 64]
W3 = [0, 9, 18, 27, 37, 46, 55, 64]
W4L = [int(v) for v in W4]
PBITS = 0b11001011


def _block(src, so):
    pos = 0

    def bits(n):
        nonlocal pos
        v = 0
        for k in range(n):
            v |= ((src[so + (pos >> 3)] >> (pos & 7)) & 1) << k
            pos += 1
        return v

    mode = 0
    while mode < 8 and bits(1) == 0:
        mode += 1
    if mode >= 8:
        return [[0, 0, 0, 0]] * 16
    partition, np_, rotation, isb = 0, 1, 0, 0
    if mode in (0, 1, 2, 3, 7):
        np_ = 3 if mode in (0, 2) else 2
        partition = bits(4 if mode == 0 else 6)
    ne = np_ * 2
    if mode in (4, 5):
        rotation = bits(2)
        if mode == 4:
            isb = bits(1)
    ep = [[0, 0, 0, 0] for _ in range(6)]
    for c in range(3):
        for e in range(ne):
            ep[e][c] = bits(BITS[0][mode])
    if BITS[1][mode] > 0:
        for e in range(ne):
            ep[e][3] = bits(BITS[1][mode])
    if mode in (0, 1, 3, 6, 7):
        for e in range(ne):
            for c in range(4):
                ep[e][c] <<= 1
        if mode == 1:
            i, j = bits(1), bits(1)
            for c in range(3):
                ep[0][c] |= i
                ep[1][c] |= i
                ep[2][c] |= j
                ep[3][c] |= j
        else:
            for e in range(ne):
                j = bits(1)
                for c in range(4):
                    ep[e][c] |= j
    pb = (PBITS >> mode) & 1
    for e in range(ne):
        j = BITS[0][mode] + pb
        for c in range(3):
            ep[e][c] = (ep[e][c] << (8 - j)) & 0xFF
            ep[e][c] |= ep[e][c] >> j
        ja = BITS[1][mode] + pb
        ep[e][3] = (ep[e][3] << (8 - ja)) & 0xFF
        ep[e][3] |= ep[e][3] >> ja
    if BITS[1][mode] == 0:
        for e in range(ne):
            ep[e][3] = 0xFF
    ib = 3 if mode in (0, 1) else 4 if mode == 6 else 2
    ib2 = 3 if mode == 4 else 2 if mode == 5 else 0
    w1 = W2 if ib == 2 else W3 if ib == 3 else W4L
    w2 = W2 if ib2 == 2 else W3
    table = P2 if np_ == 2 else P3

    def pset(i, j):
        return (0 if (i | j) else 128) if np_ == 1 else table[partition * 16 + i * 4 + j]

    idx = []
    for i in range(4):
        for j in range(4):
            n = ib - (1 if pset(i, j) & 0x80 else 0)
            idx.append(bits(n))

    def lerp(a, b, w):
        return (a * (64 - w) + b * w + 32) >> 6

    out = []
    for i in range(4):
        for j in range(4):
            s = pset(i, j) & 3
            e0, e1 = ep[s * 2], ep[s * 2 + 1]
            wt = w1[idx[i * 4 + j]]
            if ib2 == 0:
                px = [lerp(e0[k], e1[k], wt) for k in range(4)]
            else:
                wt2 = w2[bits(ib2 if (i | j) else ib2 - 1)]
                if isb == 0:
                    px = [lerp(e0[k], e1[k], wt) for k in range(3)] + [lerp(e0[3], e1[3], wt2)]
                else:
                    px = [lerp(e0[k], e1[k], wt2) for k in range(3)] + [lerp(e0[3], e1[3], wt)]
            if rotation:
                px[3], px[rotation - 1] = px[rotation - 1], px[3]
            out.append(px)
    return out


def decode(buf, w, h):
    bw, bh = (w + 3) >> 2, (h + 3) >> 2
    out = np.zeros((bh * 4, bw * 4, 4), np.uint8)
    src = memoryview(buf)
    for by in range(bh):
        for bx in range(bw):
            px = _block(src, (by * bw + bx) * 16)
            out[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = np.array(px, np.uint8).reshape(4, 4, 4)
    return out[:h, :w]


def psnr_premul(a, b):
    """PSNR of alpha-premultiplied RGB and of alpha, the values that reach the screen."""
    fa, fb = a.astype(np.float64), b.astype(np.float64)
    pa = fa[..., :3] * fa[..., 3:4] / 255
    pb = fb[..., :3] * fb[..., 3:4] / 255
    mse_c = ((pa - pb) ** 2).mean()
    mse_a = ((fa[..., 3] - fb[..., 3]) ** 2).mean()
    f = lambda m: 99.0 if m == 0 else 10 * np.log10(255 ** 2 / m)
    return f(mse_c), f(mse_a)
