"""Build Spool Up's game-native HUD: the blue Pre-Start lamps in the game's start lights, and the TURBO SPOOL gauge as
its own element of the game's HUD editor (move, scale, opacity, on/off in HUD customisation).

usage: python build.py <addon folder> [work folder]

Stock inputs come from the launcher's baseline store (%LOCALAPPDATA%\\Swofty\\baseline); the race screens' own archives,
which the baseline lacks, are read from the game folder and must hold the same stock bytes. The Swofty repo's Python
tools (pkg, idf, uinode, uiops, gfx_material) are read from env SWOFTY_TOOLS.

What it writes (erp/*.erp, one overlay op each in erp/spool_hud.json):
- spool_hud.erp, into flow_race.erp and screens/race*.erp: race_main.idf with a group 'spool_prestart' beside the game's
  start lights (five unlit lamps from the game's own lamp art, five blue lit lamps), and race.ui with the nodes that
  show them from the DLL's leaves. Quads clone start_light.idf's 'bg' quad and bind its slot 'bg' to their own material.
- spool_gauge.erp, into flow_race, flow_options and persistent (where the game keeps its HUD element scenes): the scene
  art/osd/components/spool_up/spool_gauge.idf (capsule, 14 slats, the full row, the editor's selection box) and its
  sprites. Quads clone indicator_wrong_way.idf's quad and bind its slot 'wrong_way'.
- spool_template.erp, into flow_race and flow_options: templates/spool_up_templates.ui with the template
  hud_spool_gauge, made from hud_indicator_wrong_way: the editor's on/off (osd_visibility_option) and the gauge's
  nodes on the DLL's leaves. A new file, so Project Halo's hud_dynamic_templates.ui stays its own.
- spool_layout.erp, into persistent: hud_layouts.uidl with the element spool_gauge in the 'default' layout, under
  the game's start lights. The editor names it from lng_hud_component_spool_gauge (assets/text.json).
"""
import copy
import hashlib
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'lib'))
sys.path.insert(0, HERE)
SWL = os.environ.get('SWOFTY_TOOLS')
if not SWL or not os.path.exists(os.path.join(SWL, 'idf.py')):
    raise SystemExit('set SWOFTY_TOOLS to the Swofty repo\'s tools folder (idf.py, pkg.py, uinode.py, uiops.py, gfx_material.py)')
sys.path.insert(0, SWL)

from compression import zstd  # noqa: E402
import bc7  # noqa: E402
import emb  # noqa: E402
import erpbuild as E  # noqa: E402
from erpmeta import index  # noqa: E402
import gfx_material as GM  # noqa: E402
import idf as I  # noqa: E402
import uinode as N  # noqa: E402
import uiops as U  # noqa: E402
from pkg import Arr, Obj, Pkg, Val  # noqa: E402
import sprites as SP  # noqa: E402

BUILD = '25251167-d873a1c8d204'
ERP = '2025_asset_groups/ui_package/flow_race.erp'
# Every archive that carries race_main.idf and race.ui: the screen archives hold the same stock bytes as flow_race
# (checked below while they are still stock), and the game may load the race screen from any of them.
ERPS = [ERP] + [f'2025_asset_groups/ui_package/screens/{n}.erp' for n in ('race', 'race_splitscreen', 'race_transition')]
GAME = r'F:\SteamLibrary\steamapps\common\F1 25'
P = 'eaid://ui_package/'
RACE_MAIN = P + 'art/osd/screens/race_main.idf'
RACE_UI = P + 'screens/race.ui'
LAMP_SCENE = P + 'art/osd/units/start_lights/start_light.idf'
LAMP_EMB = P + 'art/osd/units/start_lights/start_light.emb?context=default'
LAMP_MESH = LAMP_SCENE + '/bg?context=render'
LAMP_MODEL = LAMP_MESH + '?model'
BG_MAT = P + 'art/osd/materials/start_lights/start_light_bg.material'
BG_TEX = P + 'art/osd/textures/start_lights/start_light_bg.tif.image'
LIGHTS_TEX = P + 'art/texture_arrays/overrides.uidb/overrides.osd.start_lights.texarray.image'
TEX = P + 'art/osd/textures/spool_up/spool_{}.tif'
MAT = P + 'art/osd/materials/spool_up/spool_{}.material'
EMB = P + 'art/osd/components/spool_up/spool_{}.emb?context=default'
SLOT = 'bg'                 # start_light.idf's bg quad: its mesh's material slot
FMT_BC7_SRGB = 70
LIGHTS_Y = -3.39            # race_main.idf: anchor_top > start_lights
VM = '#view_model_data.hud_spool_'   # the core's hud.set takes only view_model_data.hud* leaves
UI = '2025_asset_groups/ui_package/'

# The gauge: its own HUD element
WW_SCENE = P + 'art/osd/components/indicators/indicator_wrong_way.idf'
WW_MESH = WW_SCENE + '/wrong_way?context=render'
WW_MODEL = WW_MESH + '?model'
DIAL = P + 'art/osd/components/dial/dial_complete.idf'
TEMPLATES = P + 'templates/hud_dynamic_templates.ui'
LAYOUTS = P + 'art/osd/screens/hud_layouts/hud_layouts.uidl'
GAUGE_SCENE = P + 'art/osd/components/spool_up/spool_gauge.idf'
GAUGE_UI = P + 'templates/spool_up_templates.ui'
GAUGE_KEY = 'spool_gauge'           # the layout's dynamicKey and nameId; the editor's name is lng_hud_component_<key>
GAUGE_TEMPLATE = 'hud_spool_gauge'
GAUGE_Y = round(LIGHTS_Y - SP.CAP_Y / 100.0, 4)   # anchor_top: under the start lights, where the overlay drew it
GAUGE_PREVIEW = 9                   # slats lit in the editor's picture
GAUGE_ERPS = [UI + 'flow_race.erp', UI + 'flow_options.erp', UI + 'persistent.erp']
TEMPLATE_ERPS = [UI + 'flow_race.erp', UI + 'flow_options.erp']
LAYOUT_ERPS = [UI + 'persistent.erp']
LAMP_SPRITES = ('lamp',)
SLOT_OF = {'lamp': SLOT, 'base': 'wrong_way', 'slat': 'wrong_way', 'full': 'wrong_way'}

# The leaves (bool) the DLL writes and what each one shows. Keep in step with grid_start.cpp (NL_*).
LAMP_LEAVES = ['on', 'lamps', 'lit']
GAUGE_LEAVES = ['gauge', 'full'] + [f's{i:02d}' for i in range(1, SP.SLATS + 1)]
LEAVES = LAMP_LEAVES + GAUGE_LEAVES


def stock_path(rel):
    base = os.path.join(os.environ['LOCALAPPDATA'], 'Swofty', 'baseline')
    d = json.load(open(os.path.join(base, BUILD, 'baseline.json')))
    sha = d['files'][rel]['sha256']
    p = os.path.join(base, 'objects', sha[:2], sha)
    assert hashlib.sha256(open(p, 'rb').read()).hexdigest() == sha, p
    return p


def read(archive, names):
    hdr, res = index(archive)
    doff = struct.unpack_from('<QQQ', hdr, 0x10)[2]
    want = {r[0]: r for r in res if r[0] in names}
    out = {}
    with open(archive, 'rb') as f:
        for n in names:
            if n not in want:
                raise KeyError(f'{archive}: no {n}')
            frs = []
            for fn, off, size, flags, comp, packed in want[n][3]:
                f.seek(doff + off)
                raw = f.read(packed)
                frs.append(zstd.decompress(raw) if comp == E.ZSTD else raw)
            out[n] = frs
    return out


# ---------------------------------------------------------------- resources
def surface(img):
    h, w = img.shape[:2]
    assert h % 4 == 0 and w % 4 == 0, (w, h)
    pix = bc7.encode(img)
    assert len(pix) == w * h
    back = bc7.decode6(pix, w, h)
    err = np.abs(back.astype(int) - img.astype(int)).mean()
    assert err < 4.0, ('bc7 error', err)
    return struct.pack('<10I', 2, 0x108, FMT_BC7_SRGB, w, h, 1, 1, 1, 0, 0), pix


def srv(tex):
    return struct.pack('<6I', 5, FMT_BC7_SRGB, 0, 1, 0, 0) + (tex + '.image').encode() + b'\0'


def material(stock_bytes, tex):
    m = GM.parse25(stock_bytes)
    seen = set()
    for p in m.params:
        if p.name in ('me_colour', 'me_finalColourTint'):
            p.value = struct.pack('<4f', 1, 1, 1, 1)
            seen.add(p.name)
        elif p.name == 'me_diffuseTexHeapIndex':
            p.link = tex
            seen.add(p.name)
        elif p.name == 'me_diffuseTex':
            p.value = tex
            seen.add(p.name)
    assert {'me_diffuseTex'} <= seen, seen
    out = GM.write25(m)
    back = GM.parse25(out)
    assert back.get('me_diffuseTex').value == tex
    return out


def sprite_resources(sprites, stock_material):
    res, embs = [], {}
    for name, s in sprites.items():
        tex, mat, eb = TEX.format(name), MAT.format(name), EMB.format(name)
        res.append((tex + '.image', 'GfxSurfaceRes', list(surface(s['img']))))
        res.append((tex, 'GfxSRVResource', [srv(tex)]))
        res.append((mat, 'GfxMaterialRes', [material(stock_material, tex)]))
        res.append((eb, 'GfxMatBindRes', [emb.write([(SLOT_OF[name], mat)])]))
        embs[name] = eb
    embs['stock_bg'] = LAMP_EMB
    return res, embs


# ---------------------------------------------------------------- scene
def _find(obj, name):
    if I._val(obj.get('name')) == name:
        return obj
    ch = obj.get('children')
    for c in (ch.items if isinstance(ch, Arr) else []):
        r = _find(c, name)
        if r is not None:
            return r
    return None


def _vec(v):
    return Val('vec3', [float(v[0]), float(v[1]), float(v[2]), 0.0])


def _param(o, name, value, vtype='stri'):
    for it in o['params'].items:
        if I._val(it.get('name')) == name:
            it.set('value', Val(it['value'].type, value))
            return
    I.set_param(o, name, value, vtype)


def _guid(name):
    import uuid
    return str(uuid.uuid5(uuid.NAMESPACE_URL, 'nick.grid-start-2026/' + name))


class Scene:
    def __init__(self, scene_bytes, path, quad_scene, quad_name, mesh, size):
        """scene_bytes: the scene to add to (its componentId must be path's); quads clone quad_name in quad_scene, whose
        mesh is a size x size quad in the x-z plane."""
        self.idf = I.Idf(scene_bytes)
        assert self.idf.scenes[0].name == I.component_id(path), path
        self.entry = self.idf.scenes[0].entry
        self.root = self.entry['scene']
        self.T_quad = _find(I.Idf(quad_scene).scenes[0].entry['scene'], quad_name)
        g = self.T_quad['geoms'].items[0]
        assert g['resourceid'].value == mesh, g['resourceid'].value
        lo, hi = I.get_param(self.T_quad, 'minExtents'), I.get_param(self.T_quad, 'maxExtents')
        self.mesh_w, self.mesh_h = hi[0] - lo[0], hi[2] - lo[2]
        assert abs(self.mesh_w - size) < 1e-4 and abs(self.mesh_h - size) < 1e-4, (lo, hi)
        self.names = set()

    def _base(self, name, pos, scale=(1, 1, 1)):
        assert name not in self.names, name
        self.names.add(name)
        o = I.clone_node(self.T_quad)
        o.set('name', Val('stri', name))
        _param(o, 'guid', _guid(name))
        o.set('position', _vec(pos))
        o.set('scale', _vec(scale))
        o.set('children', Arr([], o['children'].tag))
        o.set('anims', Arr([], o['anims'].tag))
        return o

    def null(self, name, pos, hidden=True):
        """A group: the quad with its geometry and extents taken away. Hidden in the scene: only the template's bound
        visible shows it, as the stock dial's speedo_number."""
        o = self._base(name, pos)
        o.set('rotation', Val('quat', [-0.0, -0.0, 0.0, -1.0]))
        o.set('geoms', Arr([], o['geoms'].tag))
        o['params'].items = [it for it in o['params'].items
                             if I._val(it.get('name')) not in ('minExtents', 'maxExtents', 'eLayer.layer', 'eLayer.layerIsOffset')]
        if hidden:
            _param(o, 'isHidden', True, 'bool')
        return o

    def quad(self, name, emb_path, pos, w, h, layer):
        """A w x h scene-unit quad centred on pos, showing the material emb_path binds to slot bg."""
        o = self._base(name, pos, (w / self.mesh_w, 1.0, h / self.mesh_h))
        o['geoms'].items[0].set('materialid', Val('stri', emb_path))
        _param(o, 'eLayer.layer', int(layer), 'si32')
        return o

    def to_bytes(self):
        return self.idf.to_bytes()


def _finish(B, stock_ids):
    ids = []
    for kid in B.root['children'].items:
        I.recompute_ids(kid, I._val(kid.get('name')), ids)
    I.relookup(B.entry)
    out = B.to_bytes()
    after = I.Idf(out)
    walked = {e.path: e.element_id for e, _ in after.scenes[0].root.walk()}
    changed = [k for k, v in stock_ids.items() if walked.get(k) != v]
    assert not changed, ('stock element ids changed', changed)
    assert len(set(ids)) == len(ids), 'elementId collision'
    names = [e.name for e, _ in after.scenes[0].root.walk()]
    assert len(names) == len(set(names)), 'element name used twice'
    return out


def build_scene(race_main, lamp_scene, sp, embs):
    """race_main.idf with the group spool_prestart (the lamps) under anchor_top -> (bytes, new elements)."""
    B = Scene(race_main, RACE_MAIN, lamp_scene, 'bg', LAMP_MESH, 0.72)
    anchor = _find(B.root, 'anchor_top')
    lights = _find(anchor, 'start_lights')
    assert anchor is not None and lights is not None
    assert abs(lights['position'].value[1] - LIGHTS_Y) < 1e-4, lights['position'].value
    assert _find(B.root, 'spool_prestart') is None, 'race_main.idf already has spool_prestart'
    stock_ids = {e.path: e.element_id for e, _ in B.idf.scenes[0].root.walk()}

    def su(px_x, px_y):   # px from the lamp row's centre, y down -> scene units, y up
        return (px_x / 100.0, -px_y / 100.0, 0.0)

    top = B.null('spool_prestart', (0.0, LIGHTS_Y, 0.0))
    lamps = B.null('spool_lamps', (0, 0, 0))
    lit = B.null('spool_lamps_lit', (0, 0, 0))
    for name, spr, cx, cy, w, h, layer in SP.layout(sp):
        if name.startswith('spool_lamp_bg'):
            lamps['children'].items.append(B.quad(name, embs[spr], su(cx, cy), w / 100.0, h / 100.0, layer))
        elif name.startswith('spool_lamp_on'):
            lit['children'].items.append(B.quad(name, embs[spr], su(cx, cy), w / 100.0, h / 100.0, layer))
    lamps['children'].items.append(lit)
    top['children'].items.append(lamps)
    anchor['children'].items.append(top)
    return _finish(B, stock_ids), len(B.names)


def build_gauge_scene(ww_scene, dial_scene, sp, embs):
    """The gauge's own scene, made from indicator_wrong_way.idf (a one-quad HUD element): Scene Root > the editor's
    selection box (tl_customisation, br_customisation, as the dial has) and spool_gauge > the capsule, spool_s01..14 and
    spool_full_on. Visible in the scene as the editor shows it (GAUGE_PREVIEW slats lit); in game every group is
    bound to a leaf by hud_spool_gauge."""
    idf = I.Idf(ww_scene)
    idf.scenes[0].entry.set('name', Val('ui64', I.component_id(GAUGE_SCENE)))
    B = Scene(idf.to_bytes(), GAUGE_SCENE, ww_scene, 'wrong_way', WW_MESH, 1.0)
    assert [I._val(c.get('name')) for c in B.root['children'].items] == ['wrong_way']
    B.root.set('children', Arr([], B.root['children'].tag))
    dial = I.Idf(dial_scene).scenes[0].entry['scene']
    for corner, x, y in (('tl_customisation', -1, 1), ('br_customisation', 1, -1)):
        o = I.clone_node(_find(dial, corner))
        _param(o, 'guid', _guid(corner))
        o.set('position', _vec((x * (SP.CAP_W / 2 + SP.PAD) / 100.0, y * (SP.CAP_H / 2 + SP.PAD) / 100.0, 0.0)))
        B.root['children'].items.append(o)

    def su(px_x, px_y):   # px from the capsule's centre, y down -> scene units, y up
        return (px_x / 100.0, -px_y / 100.0, 0.0)

    gauge = B.null('spool_gauge', (0, 0, 0), hidden=False)
    slats, full = [], None
    for name, spr, cx, cy, w, h, layer in SP.layout(sp):
        if name.startswith('spool_lamp'):
            continue
        q = B.quad(name, embs[spr], su(cx, cy - SP.CAP_Y), w / 100.0, h / 100.0, layer)
        if name.startswith('spool_slat'):
            # each slat in its own group so its bound visible shows it alone
            i = int(name[-2:])
            g = B.null(f'spool_s{i:02d}', (0, 0, 0), hidden=i > GAUGE_PREVIEW)
            g['children'].items.append(q)
            slats.append(g)
        elif name == 'spool_full':
            full = B.null('spool_full_on', (0, 0, 0))
            full['children'].items.append(q)
        else:
            gauge['children'].items.append(q)
    gauge['children'].items.extend(slats + [full])
    B.root['children'].items.append(gauge)
    return _finish(B, {}), len(B.names)


# ---------------------------------------------------------------- template
def _bool_prop(default, db):
    return U.obj(('value', Val('bool', bool(default))), ('dbentry', U.S(db)))


def _grp(name, leaf, element, kids=()):
    return U.node(name, 'UIGP', [('visible', _bool_prop(False, VM + leaf))], kids=list(kids), layout=[element],
                  behav=U.GROUP_BEHAVIOURS)


def _bound(root):
    """The leaves the nodes under root bind their visible to."""
    bound = []

    def walk(x):
        if N.is_node(x):
            v = x.items[N.PROPS].get('visible')
            if isinstance(v, Obj) and v.get('dbentry') is not None and v['dbentry'].value.startswith(VM):
                bound.append(v['dbentry'].value[len(VM):])
            for c in N.children(x):
                walk(c)
        elif isinstance(x, Arr):
            for c in x.items:
                walk(c)
        elif isinstance(x, Obj):
            for _k, v in x.items:
                walk(v)
    walk(root)
    return bound


def _strings(x):
    if isinstance(x, Val):
        return [x.value] if isinstance(x.value, str) else []
    if isinstance(x, Obj):
        return [s for _k, v in x.items for s in _strings(v)]
    if isinstance(x, Arr):
        return [s for v in x.items for s in _strings(v)]
    return []


def build_template(templates):
    """templates/spool_up_templates.ui: the template hud_spool_gauge, a copy of hud_indicator_wrong_way (a HUD element
    with the editor's on/off and nothing else) whose component is the gauge scene and whose osd_visibility_option holds
    the gauge's nodes instead of the wrong-way sign's."""
    p = Pkg(templates)
    items = p.root['Templates'].items
    names = [N.name(t) for t in items]
    assert GAUGE_TEMPLATE not in names
    t = copy.deepcopy(items[names.index('hud_indicator_wrong_way')])
    N.rename_refs(t, 'hud_indicator_wrong_way', GAUGE_TEMPLATE)
    t.items[N.PROPS]['name'].value = GAUGE_TEMPLATE
    comps = t.items[N.COMPONENTS].items
    assert len(comps) == 1 and comps[0]['componentId'].value == I.component_id(WW_SCENE), comps
    comps[0]['componentId'].value = I.component_id(GAUGE_SCENE)
    comps[0]['debugName'].value = GAUGE_SCENE[:-len('.idf')]
    comps[0]['name'].value = GAUGE_KEY
    osd = N.child(t, 'osd_visibility_option')
    assert [N.name(c) for c in N.children(osd)] == ['are_we_driving_forward', 'wrong_way']
    vis = osd.items[N.PROPS]['visible']
    assert vis['dbentry'].value == 'visible' and vis['value'].value is True
    for c in osd.items[N.COMMANDS].items:     # they fed the wrong-way sign's data node, which is gone
        c['signals'].items = []
    slats = [_grp(f's{i:02d}', f's{i:02d}', f'spool_s{i:02d}') for i in range(1, SP.SLATS + 1)]
    osd.items[N.CHILDREN].items = [_grp('gauge', 'gauge', 'spool_gauge', slats + [_grp('full', 'full', 'spool_full_on')])]
    N.fix_ids(t)
    p.root['Templates'].items = [t]
    out = p.to_bytes()
    back = Pkg(out)
    assert [N.name(x) for x in back.root['Templates'].items] == [GAUGE_TEMPLATE]
    assert sorted(_bound(back.root)) == sorted(GAUGE_LEAVES), sorted(_bound(back.root))
    left = [x for x in _strings(back.root) if 'wrong_way' in x or 'driving_forward' in x]
    assert not left, left
    return out


def build_layouts(uidl):
    """hud_layouts.uidl with the gauge in the 'default' layout: a copy of indicator_wrong_way's entry with no HUD
    filter (shown in every HUD mode, as the lap counter), under anchor_top at the overlay gauge's old place."""
    p = Pkg(uidl)
    lays = p.root['layouts'].items
    default = [x for x in lays if x['id'].value == 'default']
    assert len(default) == 1
    comps = default[0]['components'].items
    keys = [c['eDynamicLayout.dynamicKey'].value for x in lays for c in x['components'].items]
    assert GAUGE_KEY not in keys
    src = [c for c in comps if c['nameId'].value == 'indicator_wrong_way']
    assert len(src) == 1
    e = copy.deepcopy(src[0])
    assert e['xref'].value == I.component_id(WW_SCENE)
    e['target'].value = 'anchor_top'
    e['eDynamicLayout.dynamicTemplate'].value = GAUGE_TEMPLATE
    e['eDynamicLayout.dynamicKey'].value = GAUGE_KEY
    e['xref'].value = I.component_id(GAUGE_SCENE)
    e['nameId'].value = GAUGE_KEY
    e['position'].value = [0.0, GAUGE_Y, 0.0, 0.0]
    N.drop_key(e, 'eDynamicLayout.dynamicFilter')
    comps.append(e)
    out = p.to_bytes()
    back = Pkg(out)
    stock = Pkg(uidl)
    for a, b in zip(stock.root['layouts'].items, back.root['layouts'].items):
        n = len(a['components'].items)
        assert len(b['components'].items) == n + (a['id'].value == 'default'), a['id'].value
    return out


def build_ui(ui_bytes):
    """race.ui with the group spool_prestart on the race screen (main_start_lights' sibling)."""
    p = Pkg(ui_bytes)
    race = p.root['Screens'].items[0]
    assert N.name(race) == 'race'
    names = [N.name(c) for c in N.children(race)]
    assert 'main_start_lights' in names and 'spool_prestart' not in names, names
    msl = N.child(race, 'main_start_lights')
    lay = msl.items[N.LAYOUT].items[0].items
    assert [b['debugName'].value for b in lay] == ['in_race_screen', 'start_lights'], 'start lights moved'

    lamps = _grp('lamps', 'lamps', 'spool_lamps', [_grp('lit', 'lit', 'spool_lamps_lit')])
    top = U.node('spool_prestart', 'UIGP', [('visible', _bool_prop(False, VM + 'on'))], kids=[lamps],
                 behav=U.GROUP_BEHAVIOURS)
    top.items[N.LAYOUT] = U.layout_path('in_race_screen', 'spool_prestart')
    N.fix_ids(top, 'race')
    # after main_start_lights, so it draws in the same order relative to the rest of the screen
    kids = race.items[N.CHILDREN].items
    kids.insert(kids.index(msl) + 1, top)
    out = p.to_bytes()
    # read back: every leaf bound once, the stock nodes unchanged
    back = Pkg(out)
    assert sorted(_bound(back.root)) == sorted(LAMP_LEAVES), sorted(_bound(back.root))
    stock = Pkg(ui_bytes)
    s_race = stock.root['Screens'].items[0]
    b_race = back.root['Screens'].items[0]
    s_kids = [c for c in N.children(s_race)]
    b_kids = [c for c in N.children(b_race) if N.name(c) != 'spool_prestart']
    assert len(s_kids) == len(b_kids)
    for a, b in zip(s_kids, b_kids):
        assert Pkg.__name__ and N.name(a) == N.name(b)
    for scr_s, scr_b in zip(stock.root['Screens'].items[1:], back.root['Screens'].items[1:]):
        assert N.name(scr_s) == N.name(scr_b)
    return out


def _same_in(archives, names, ref):
    """Assert the archives hold the same bytes as ref for names: baseline archives from the store, the rest from the game
    folder while Swofty has not built them yet (the store only keeps the archives it has rewritten before)."""
    base = json.load(open(os.path.join(os.environ['LOCALAPPDATA'], 'Swofty', 'baseline', BUILD, 'baseline.json')))['files']
    game = os.environ.get('F1_25_DIR', GAME)
    for rel in archives:
        if rel in base:
            path = stock_path(rel)
        else:
            path = os.path.join(game, rel)
            if not os.path.exists(path):
                raise SystemExit(f'{path} is missing: set F1_25_DIR to the game folder')
            if any('/spool_up/' in r[0] for r in index(path)[1]):
                print(f'{rel}: already built with this addon, not compared')
                continue
        o = read(path, names)
        for n in names:
            assert o[n] == ref[n], f'{rel} carries a different {n}'


def write_erp(addon, name, ref_erp, resources):
    """resources: [(name, type, fragments, like)] -> erp/<name>.erp, read back."""
    a = E.Archive(ref_erp)
    for n, t, fr, like in resources:
        a.add(n, t, fr, like)
    path = os.path.join(addon, 'erp', name + '.erp')
    size = a.write(path)
    back = E.read_all(path)
    assert len(back) == len(resources)
    for n, t, fr, like in resources:
        assert back[n][1] == list(fr), n
    return size


def main():
    addon = sys.argv[1]
    work = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'work')
    os.makedirs(work, exist_ok=True)
    os.makedirs(os.path.join(addon, 'erp'), exist_ok=True)
    race = stock_path(ERP)
    st = read(race, [RACE_MAIN, RACE_UI, LAMP_SCENE, BG_MAT, BG_TEX, LIGHTS_TEX, LAMP_MODEL, LAMP_EMB,
                     WW_SCENE, WW_MODEL, DIAL, TEMPLATES])
    st.update(read(stock_path(UI + 'persistent.erp'), [LAYOUTS]))
    assert b'\0bg\0' in b''.join(st[LAMP_MODEL]), 'start_light bg mesh slot'
    assert b'\0wrong_way' in b''.join(st[WW_MODEL]), 'indicator_wrong_way mesh slot'
    head = struct.unpack('<10I', st[LIGHTS_TEX][0])
    assert head[2:5] == (FMT_BC7_SRGB, 128, 128) and head[7] == 2, head
    red = bc7.decode(st[LIGHTS_TEX][1][16384:32768], 128, 128)   # layer 1: the lit red lamp (set_red)
    assert red[..., 0].astype(int).sum() > 3 * red[..., 1].astype(int).sum(), 'layer 1 is not the red lamp'
    _same_in(ERPS[1:], [RACE_MAIN, RACE_UI, LAMP_SCENE, LAMP_MODEL, LAMP_EMB, BG_MAT], st)
    _same_in(GAUGE_ERPS[1:], [WW_SCENE, WW_MODEL, DIAL], st)

    sp = SP.sprites(red)
    res, embs = sprite_resources(sp, st[BG_MAT][0])
    lamp_res = [r for r in res if any(f'/spool_{k}.' in r[0] for k in LAMP_SPRITES)]
    gauge_res = [r for r in res if r not in lamp_res]
    assert len(lamp_res) == 4 * len(LAMP_SPRITES) and len(gauge_res) == 4 * (len(sp) - len(LAMP_SPRITES))
    scene, n_lamps = build_scene(st[RACE_MAIN][0], st[LAMP_SCENE][0], sp, embs)
    ui = build_ui(st[RACE_UI][0])
    gscene, n_gauge = build_gauge_scene(st[WW_SCENE][0], st[DIAL][0], sp, embs)
    tpl = build_template(st[TEMPLATES][0])
    lay = build_layouts(st[LAYOUTS][0])

    sizes = {
        'spool_hud': write_erp(addon, 'spool_hud', race, [(RACE_MAIN, 'UILayout', [scene], None),
                                                          (RACE_UI, 'UIDF', [ui], None)] +
                               [(n, t, fr, None) for n, t, fr in lamp_res]),
        'spool_gauge': write_erp(addon, 'spool_gauge', race, [(GAUGE_SCENE, 'UILayout', [gscene], WW_SCENE)] +
                                 [(n, t, fr, None) for n, t, fr in gauge_res]),
        'spool_template': write_erp(addon, 'spool_template', race, [(GAUGE_UI, 'UIDF', [tpl], TEMPLATES)]),
        'spool_layout': write_erp(addon, 'spool_layout', stock_path(UI + 'persistent.erp'),
                                  [(LAYOUTS, 'UIDL', [lay], None)]),
    }
    ops = {'schema_version': 1, 'ops': [
        {'op': 'overlay', 'erps': ERPS, 'source': 'erp/spool_hud.erp'},
        {'op': 'overlay', 'erps': GAUGE_ERPS, 'source': 'erp/spool_gauge.erp'},
        {'op': 'overlay', 'erps': TEMPLATE_ERPS, 'source': 'erp/spool_template.erp'},
        {'op': 'overlay', 'erps': LAYOUT_ERPS, 'source': 'erp/spool_layout.erp'},
    ]}
    with open(os.path.join(addon, 'erp', 'spool_hud.json'), 'w', newline='\n') as f:
        json.dump(ops, f, indent=2)
        f.write('\n')
    for nm, b in (('race_main.idf', scene), ('race.ui', ui), ('spool_gauge.idf', gscene),
                  ('spool_up_templates.ui', tpl), ('hud_layouts.uidl', lay)):
        open(os.path.join(work, nm), 'wb').write(b)
    bg = bc7.decode(st[BG_TEX][1], 128, 128)
    for nm, state in (('pre', dict(pre=True, lit=True, gauge=True, level=9, full=False)),
                      ('pre_off', dict(pre=True, lit=False, gauge=True, level=3, full=False)),
                      ('full', dict(pre=False, lit=False, gauge=True, level=14, full=True))):
        SP.preview(sp, red, bg, state, os.path.join(work, f'preview_{nm}.png'))
    print(f'race_main.idf {len(st[RACE_MAIN][0])} -> {len(scene)} bytes, {n_lamps} new elements; '
          f'race.ui {len(st[RACE_UI][0])} -> {len(ui)} bytes')
    print(f'spool_gauge.idf {len(gscene)} bytes, {n_gauge} elements; {GAUGE_TEMPLATE} {len(tpl)} bytes; '
          f'hud_layouts.uidl {len(st[LAYOUTS][0])} -> {len(lay)} bytes, {GAUGE_KEY} at anchor_top y {GAUGE_Y}')
    print(f'{len(LEAVES)} leaves bound; erp sizes {sizes}')


if __name__ == '__main__':
    main()
