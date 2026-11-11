"""Spool Up's game-native Pre-Start HUD: the sprites, drawn in the look of Project Halo's onboard halo.

Units: px at 1920 x 1080 (100 px = 1 scene unit), y down. Each sprite is drawn SS times larger, then box-filtered
down to TEX texels per px, so a texture holds TEX x its size in px.

sprites(lamp_red) -> {name: {'img': HxWx4 uint8, 'w': px, 'h': px}}
LAYOUT            -> where each quad sits, centred on the lamp row's centre (px, y down)
"""
import colorsys
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, 'saira.ttf')
SS = 4    # supersampling
TEX = 2   # texels per 1080p px

# Project Halo's palette (tools/halo.js, theme authentic)
C = {
    'bandTop': (24, 62, 78, 0.50), 'band': (8, 26, 40, 0.50), 'bandBot': (6, 18, 28, 0.40),
    'bandEdge': (150, 212, 222, 0.42), 'bandGlow': (90, 200, 210, 0.10),
    'label': (228, 240, 243, 1.0),
    'stripePanel': (18, 30, 40, 0.80), 'stripeOff': (66, 78, 88, 0.92),
    'capOnTop': (58, 154, 238, 1.0), 'capOnBot': (23, 132, 206, 1.0), 'capOnLine': (68, 176, 228, 1.0),
    'throttle': (24, 214, 160, 1.0), 'throttleTop': (38, 220, 169, 1.0),
    'otOff': (92, 101, 108, 0.92), 'otOffLine': (130, 140, 148, 0.8), 'otOffText': (40, 48, 53, 1.0),
    'fullOn': (24, 214, 160, 1.0), 'fullOnLine': (140, 240, 208, 1.0), 'fullOnText': (4, 36, 27, 1.0),
    'fullGlow': (60, 220, 160, 0.32),
}

# Geometry, px at 1080p
LAMP_X = (-164, -82, 0, 82, 164)   # the game's five start lights (start_lights.idf: -1.64 .. 1.64)
LAMP = 72                           # their quad (start_light.idf bg / light: 0.72)
CAP_W, CAP_H = 384, 84              # the capsule
CAP_Y = 104                         # its centre, below the lamp row's centre
PAD = 8                             # room for the rim's glow around the capsule
LABEL_Y = 22                        # from the capsule's top: the label's centre
ROW_Y = 57                          # from the capsule's top: the slat row's centre
SLATS, SLAT_W, SLAT_H, SLAT_GAP, SLAT_LEAN = 14, 14, 26, 4, 5
PILL_W, PILL_H, ROW_GAP = 68, 26, 14
ROW_W = SLATS * SLAT_W + (SLATS - 1) * SLAT_GAP + ROW_GAP + PILL_W
ROW_X0 = -ROW_W / 2                 # the row's left edge, from the capsule's centre


def rgba(c, a=None):
    r, g, b, al = c
    return (r, g, b, int(round(255 * (al if a is None else a))))


def font(px, weight=700, width=125):
    f = ImageFont.truetype(FONT, int(round(px * SS)))
    f.set_variation_by_axes([weight, width])
    return f


class Canvas:
    """A supersampled RGBA canvas in px (1080p) with its origin at the sprite's top left."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.im = Image.new('RGBA', (int(w * SS * TEX), int(h * SS * TEX)), (0, 0, 0, 0))

    def s(self, v):
        return v * SS * TEX

    def layer(self):
        return Image.new('RGBA', self.im.size, (0, 0, 0, 0))

    def put(self, lay):
        self.im = Image.alpha_composite(self.im, lay)

    def rrect(self, x, y, w, h, r, fill=None, line=None, lw=1.0):
        lay = self.layer()
        d = ImageDraw.Draw(lay)
        box = [self.s(x), self.s(y), self.s(x + w) - 1, self.s(y + h) - 1]
        d.rounded_rectangle(box, radius=self.s(r), fill=fill, outline=line, width=max(1, int(round(self.s(lw)))))
        self.put(lay)

    def vgrad_rrect(self, x, y, w, h, r, stops):
        """A rounded rect filled with a vertical gradient: stops [(t, rgba tuple)]."""
        lay = self.layer()
        mask = Image.new('L', self.im.size, 0)
        ImageDraw.Draw(mask).rounded_rectangle([self.s(x), self.s(y), self.s(x + w) - 1, self.s(y + h) - 1],
                                               radius=self.s(r), fill=255)
        y0, y1 = int(self.s(y)), int(self.s(y + h))
        col = np.zeros((self.im.size[1], 4), np.float64)
        for row in range(y0, y1):
            t = (row - y0) / max(1, y1 - y0 - 1)
            for (ta, ca), (tb, cb) in zip(stops, stops[1:]):
                if ta <= t <= tb:
                    k = (t - ta) / max(1e-9, tb - ta)
                    col[row] = [ca[i] + (cb[i] - ca[i]) * k for i in range(4)]
                    break
        arr = np.repeat(col[:, None, :], self.im.size[0], axis=1)
        arr[..., 3] *= np.asarray(mask, np.float64) / 255.0
        lay = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), 'RGBA')
        self.put(lay)

    def poly(self, pts, fill=None, line=None, lw=1.0):
        lay = self.layer()
        d = ImageDraw.Draw(lay)
        p = [(self.s(x), self.s(y)) for x, y in pts]
        d.polygon(p, fill=fill)
        if line:
            d.line(p + [p[0]], fill=line, width=max(1, int(round(self.s(lw)))), joint='curve')
        self.put(lay)

    def text(self, cx, cy, s, px, fill, tracking=0.0, weight=700):
        """Text centred on (cx, cy), cap height centred (Saira's cap height is 0.688 em)."""
        f = font(px * TEX, weight)
        adv = [f.getlength(ch) for ch in s]
        tr = self.s(tracking)
        total = sum(adv) + tr * (len(s) - 1)
        x = self.s(cx) - total / 2
        base = self.s(cy) + 0.688 * px * SS * TEX / 2
        lay = self.layer()
        d = ImageDraw.Draw(lay)
        for ch, a in zip(s, adv):
            d.text((x, base), ch, font=f, fill=fill, anchor='ls')
            x += a + tr
        self.put(lay)

    def blur_glow(self, radius_px):
        from PIL import ImageFilter
        return self.im.filter(ImageFilter.GaussianBlur(self.s(radius_px)))

    def out(self):
        im = self.im.resize((int(self.w * TEX), int(self.h * TEX)), Image.BOX)
        a = np.array(im)
        assert a.shape[0] % 4 == 0 and a.shape[1] % 4 == 0, a.shape
        return a


def slat_pts(x, y):
    """One slat, leaning right like the halo's throttle stripes: (x, y) is its bottom left corner."""
    return [(x, y), (x + SLAT_LEAN, y - SLAT_H), (x + SLAT_LEAN + SLAT_W, y - SLAT_H), (x + SLAT_W, y)]


def slat_x(i):
    """Slat i's bottom left x, from the capsule's centre."""
    return ROW_X0 + i * (SLAT_W + SLAT_GAP) - SLAT_LEAN / 2


def pill_x():
    return ROW_X0 + SLATS * SLAT_W + (SLATS - 1) * SLAT_GAP + ROW_GAP


def draw_pill(c, x, y, lit):
    if lit:
        glow = Canvas(c.w, c.h)
        glow.rrect(x - 2, y - 2, PILL_W + 4, PILL_H + 4, PILL_H / 2 + 2, fill=rgba(C['fullGlow'], 0.9))
        c.put(glow.blur_glow(3))
        c.vgrad_rrect(x, y, PILL_W, PILL_H, PILL_H / 2, [(0, rgba(C['throttleTop'])), (1, rgba(C['fullOn']))])
        c.rrect(x, y, PILL_W, PILL_H, PILL_H / 2, line=rgba(C['fullOnLine']), lw=1.2)
        c.text(x + PILL_W / 2, y + PILL_H / 2, 'FULL', 13, rgba(C['fullOnText']), tracking=1.2, weight=800)
    else:
        c.rrect(x, y, PILL_W, PILL_H, PILL_H / 2, fill=rgba(C['otOff']), line=rgba(C['otOffLine']), lw=1.0)
        c.text(x + PILL_W / 2, y + PILL_H / 2, 'FULL', 13, rgba(C['otOffText']), tracking=1.2, weight=800)


def base():
    """The capsule: Halo glass, its rim, the label, the unlit slats and the unlit FULL pill."""
    W, H = CAP_W + 2 * PAD, CAP_H + 2 * PAD
    c = Canvas(W, H)
    x0, y0 = PAD, PAD
    r = 14
    glow = Canvas(W, H)
    glow.rrect(x0 - 1, y0 - 1, CAP_W + 2, CAP_H + 2, r + 1, line=rgba(C['bandGlow'], 0.55), lw=4)
    c.put(glow.blur_glow(3))
    c.vgrad_rrect(x0, y0, CAP_W, CAP_H, r, [(0, rgba(C['bandTop'], 0.78)), (0.45, rgba(C['band'], 0.80)),
                                             (1, rgba(C['bandBot'], 0.72))])
    c.rrect(x0, y0, CAP_W, CAP_H, r, line=rgba(C['bandEdge'], 0.55), lw=1.4)
    c.text(x0 + CAP_W / 2, y0 + LABEL_Y, 'TURBO SPOOL', 15, rgba(C['label']), tracking=2.2)
    cx, row = x0 + CAP_W / 2, y0 + ROW_Y
    # the stripe panel behind the slats, as behind the halo's brake and throttle stripes
    c.rrect(cx + ROW_X0 - 6, row - SLAT_H / 2 - 5, SLATS * (SLAT_W + SLAT_GAP) - SLAT_GAP + SLAT_LEAN + 12, SLAT_H + 10, 5,
            fill=rgba(C['stripePanel']))
    for i in range(SLATS):
        c.poly(slat_pts(cx + slat_x(i), row + SLAT_H / 2), fill=rgba(C['stripeOff']))
    draw_pill(c, cx + pill_x(), row - PILL_H / 2, False)
    return c.out()


def slat_on():
    """One lit slat, the halo's capsule blue: drawn in its own box with 2 px around it."""
    W, H = SLAT_W + SLAT_LEAN + 4, SLAT_H + 4
    W, H = (W + 1) // 2 * 2, (H + 1) // 2 * 2
    c = Canvas(W, H)
    pts = slat_pts((W - SLAT_W - SLAT_LEAN) / 2, (H + SLAT_H) / 2)
    lay = Canvas(W, H)
    lay.poly(pts, fill=(0, 0, 0, 255))
    mask = np.array(lay.im)[..., 3].astype(np.float64) / 255.0
    hgt = mask.shape[0]
    t = np.linspace(0, 1, hgt)[:, None]
    top, bot = np.array(C['capOnTop'][:3], float), np.array(C['capOnBot'][:3], float)
    rgb = top[None, None, :] * (1 - t[..., None]) + bot[None, None, :] * t[..., None]
    arr = np.zeros(mask.shape + (4,))
    arr[..., :3] = rgb
    arr[..., 3] = 255 * mask
    c.put(Image.fromarray(arr.astype(np.uint8), 'RGBA'))
    c.poly(pts, line=rgba(C['capOnLine']), lw=0.9)
    return c.out(), W, H


def slats_full():
    """All 14 slats green (the halo's throttle green), with the lit FULL pill: one quad over the whole row."""
    W = int(ROW_W + SLAT_LEAN + 16)
    W += W % 2
    H = SLAT_H + 16
    c = Canvas(W, H)
    x0 = (W - ROW_W - SLAT_LEAN) / 2   # the row's left edge in this sprite
    for i in range(SLATS):
        x = x0 + i * (SLAT_W + SLAT_GAP)
        c.poly(slat_pts(x, H / 2 + SLAT_H / 2), fill=rgba(C['throttle']), line=rgba(C['throttleTop']), lw=0.9)
    draw_pill(c, x0 + SLAT_LEAN / 2 + (pill_x() - ROW_X0), H / 2 - PILL_H / 2, True)
    return c.out(), W, H


def lamp_blue(red_rgba):
    """The game's own lit start light (overrides.osd.start_lights layer 1, red), turned blue: hue moved, saturation
    and value kept, then lifted a little because blue reads darker than red at the same value."""
    a = red_rgba.astype(np.float64) / 255.0
    out = a.copy()
    flat = a[..., :3].reshape(-1, 3)
    res = np.empty_like(flat)
    for i, (r, g, b) in enumerate(flat):
        h, s, v = colorsys.rgb_to_hsv(r, g, b)
        h = 222.0 / 360.0
        s = s * 0.92
        v = min(1.0, v * 1.12)
        res[i] = colorsys.hsv_to_rgb(h, s, v)
    out[..., :3] = res.reshape(a.shape[:2] + (3,))
    return (np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)


def sprites(lamp_red):
    b = base()
    s, sw, sh = slat_on()
    f, fw, fh = slats_full()
    return {
        'base': {'img': b, 'w': CAP_W + 2 * PAD, 'h': CAP_H + 2 * PAD},
        'slat': {'img': s, 'w': sw, 'h': sh},
        'full': {'img': f, 'w': fw, 'h': fh},
        'lamp': {'img': lamp_blue(lamp_red), 'w': LAMP, 'h': LAMP},
    }


def layout(sp):
    """[(element name, sprite, cx, cy, w, h, layer)] in px from the lamp row's centre, y down. Sprite 'stock_bg' is the
    game's own unlit lamp (start_light.emb's bg slot)."""
    q = []
    for i, x in enumerate(LAMP_X, 1):
        q.append((f'spool_lamp_bg{i}', 'stock_bg', x, 0, LAMP, LAMP, 35))
        q.append((f'spool_lamp_on{i}', 'lamp', x, 0, LAMP, LAMP, 40))
    q.append(('spool_cap_base', 'base', 0, CAP_Y, sp['base']['w'], sp['base']['h'], 35))
    row = CAP_Y - CAP_H / 2 + ROW_Y
    for i in range(SLATS):
        # the slat sprite's centre: its bottom left corner plus half its lean and width, half its height up
        cx = slat_x(i) + (SLAT_W + SLAT_LEAN) / 2
        q.append((f'spool_slat{i + 1:02d}', 'slat', cx, row, sp['slat']['w'], sp['slat']['h'], 40))
    q.append(('spool_full', 'full', ROW_X0 + ROW_W / 2, row, sp['full']['w'], sp['full']['h'], 41))
    return q


def preview(sp, lamp_red, bg, state, path):
    """A 1080p-px picture of the HUD in a state {'pre': bool, 'lit': bool, 'gauge': bool, 'level': 0..14, 'full': bool}
    on a dark backdrop, at TEX texels per px: what the game should draw."""
    W, H = 520, 260
    ox, oy = W / 2, 50
    img = Image.new('RGBA', (W * TEX, H * TEX), (44, 52, 60, 255))
    def paste(sprite_img, cx, cy):
        im = Image.fromarray(sprite_img, 'RGBA')
        img.alpha_composite(im, (int(round((ox + cx) * TEX - im.size[0] / 2)), int(round((oy + cy) * TEX - im.size[1] / 2))))
    q = layout(sp)
    for name, spr, cx, cy, w, h, layer in sorted(q, key=lambda e: e[6]):
        if name.startswith('spool_lamp_bg') and not state['pre']:
            continue
        if name.startswith('spool_lamp_on') and not (state['pre'] and state['lit']):
            continue
        if name.startswith('spool_cap') and not state['gauge']:
            continue
        if name.startswith('spool_slat'):
            if not state['gauge'] or state['full'] or int(name[-2:]) > state['level']:
                continue
        if name == 'spool_full' and not (state['gauge'] and state['full']):
            continue
        im = bg if spr == 'stock_bg' else sp[spr]['img']
        if spr in ('stock_bg', 'lamp'):
            im = np.array(Image.fromarray(im, 'RGBA').resize((LAMP * TEX, LAMP * TEX), Image.LANCZOS))
        paste(im, cx, cy)
    img.save(path)
