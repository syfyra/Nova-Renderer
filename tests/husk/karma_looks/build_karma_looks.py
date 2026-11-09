"""Writes the scenes of tests/husk/check_karma_looks.py (docs/design/M11-P9a.md):
one USDA per case, holding what Houdini's own LOPs author over one of the
geo_*.usda scenes (a subLayer): a Karma Render Settings LOP
(karmarendersettings) with the case's outline parameters or Simplified
Shading, the Karma properties the Karma LOP does not show
(override_lighting, head_*, autoheadlight) with the types the Render
Settings LOP authors them, and a Render Geometry Settings LOP with the
objects' outline properties on its Karma tab. The generated files are committed;
rebuild them after a Houdini update with an isolated hython:

  powershell -ExecutionPolicy Bypass -File tests\\houdini\\run_hython.ps1 -Script tests\\husk\\karma_looks\\build_karma_looks.py

Cases: name -> (geometry, Karma Render Settings parameters, Render Settings
parameters (Karma property: value), [(prim, {Karma object property: value})]).
Every case renders 320 x 240, Path Traced, uniform oracle, 64 samples,
colour limits off.
"""

import os
import sys

import hou
from pxr import Sdf

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else HERE

PATH = {'convergence_mode': 'Path Traced', 'oracle': 'uniform', 'pathtracedsamples': 64,
        'colorlimit': 1e6, 'colorlimitindirect': 1e6}
LINES = dict(PATH, outline_enable=1, outline_linesaovs=1, outline_debug=1)
WIDE = [('/World', {'outline_radius': 0.02})]  # 8.9 pixels at the cards

CASES = {
    # Contour lines: the default stencil, its controls.
    'edge_base': ('geo_edge.usda', dict(LINES), {}, WIDE),
    'edge_fit': ('geo_edge.usda', dict(LINES, outline_fitmin=0.2, outline_fitmax=0.6), {}, WIDE),
    'edge_intensity': ('geo_edge.usda', dict(LINES, outline_intensity=0.5), {}, WIDE),
    'edge_color': ('geo_edge.usda', dict(LINES, outline_colorr=0, outline_colorg=1, outline_colorb=0), {}, WIDE),
    'edge_stencils': ('geo_edge.usda', dict(LINES, outline_primary_stencils=3), {}, WIDE),
    'edge_random': ('geo_edge.usda', dict(LINES, outline_mode=1), {}, WIDE),
    'edge_blend': ('geo_edge.usda', dict(LINES, outline_depthblend=1), {}, [('/World', {'outline_radius': 0.2})]),
    'edge_noapply': ('geo_edge.usda', dict(LINES, outline_apply=0), {}, WIDE),
    'edge_default': ('geo_edge.usda', dict(LINES), {}, []),  # the default radius: 0.9 pixels
    # Depth and crease lines: a step of one object, a convex fold.
    'step_lines': ('geo_step.usda', dict(LINES), {}, WIDE),
    # Look-dev overrides.
    'look_base': ('geo_look.usda', dict(PATH), {}, []),
    'look_emissive': ('geo_look.usda', dict(PATH), {'override_lighting': 'Emissive Objects'}, []),
    'look_headlight': ('geo_look.usda', dict(PATH), {'override_lighting': 'Headlight'}, []),
    'look_dome': ('geo_look.usda', dict(PATH), {'override_lighting': 'Dome Light'}, []),
    'look_auto': ('geo_look_dark.usda', dict(PATH), {'autoheadlight': 1}, []),
    'look_simple': ('geo_look.usda', dict(PATH, force_headlight=1), {}, []),
    'look_simple_noao': ('geo_look.usda', dict(PATH, force_headlight=1), {'head_ao_samples': 0}, []),
    'look_simple_aodist': ('geo_look.usda', dict(PATH, force_headlight=1), {'head_ao_distance': 5.0}, []),
    'look_simple_fog': ('geo_look.usda', dict(PATH, force_headlight=1),
                        {'head_depthcue_z': (5.0, 15.0), 'head_depthcue_color': (1.0, 0.0, 0.0),
                         'head_depthcue_alpha': 0.5}, []),
}


def karma_parm(node, name):
    enc = hou.text.encodeParm('primvars:karma:object:' + name)
    return node.parmTuple(enc), node.parm(hou.text.encodeParm('primvars:karma:object:' + name + '_control'))


def build(name, geo, res, settings, globals_, objects):
    stage = hou.node('/stage')
    for child in stage.children():
        child.destroy()
    last = stage.createNode('sublayer', 'geometry')
    last.parm('filepath1').set(os.path.join(HERE, geo).replace('\\', '/'))
    for i, (prim, props) in enumerate(objects):
        rgs = stage.createNode('rendergeometrysettings', 'geo%d' % i)
        rgs.setInput(0, last)
        rgs.parm('primpattern').set(prim)
        for prop, value in props.items():
            tup, control = karma_parm(rgs, prop)
            if tup is None or control is None:
                raise RuntimeError('Render Geometry Settings has no Karma parameter %s' % prop)
            control.set('set')
            tup.set(value if isinstance(value, (tuple, list)) else (value,))
        last = rgs
    krs = stage.createNode('karmarendersettings', 'karma')
    krs.setInput(0, last)
    krs.parm('primpath').set('/Render/rendersettings')
    krs.parm('renderproductsparentprimpath').set('/Render/Products')
    krs.parm('rendervarsparentprimpath').set('/Render/Vars')
    krs.parm('picture').set('karma_looks.exr')  # husk -o replaces it
    krs.parm('camera').set('/World/cam')
    for parm, value in (('resolutionx', res[0]), ('resolutiony', res[1])):
        p = krs.parm(parm)
        p.lock(False)
        p.deleteAllKeyframes()  # the auto-height expression
        p.set(value)
    for parm, value in settings.items():
        p = krs.parm(parm)
        if p is None:
            raise RuntimeError('Karma Render Settings has no parameter %s' % parm)
        p.set(value)
    last = krs
    last.cook(force=True)
    if last.errors():
        raise RuntimeError('%s: %s' % (name, last.errors()))
    flat = last.stage().Flatten()
    layer = Sdf.Layer.CreateAnonymous('.usda')
    Sdf.CopySpec(flat, '/Render', layer, '/Render')
    # The Karma properties the Karma LOP does not show, with the types the
    # Render Settings LOP authors them (measured: the menu as its label
    # token, ints, floats, float2/float3, bools). Its parameters do not take
    # values in an isolated hython (it authors the defaults), so they are
    # written here.
    settings_prim = layer.GetPrimAtPath('/Render/rendersettings')
    for prop, value in globals_.items():
        if isinstance(value, str):
            t = Sdf.ValueTypeNames.Token
        elif isinstance(value, tuple):
            t = Sdf.ValueTypeNames.Float2 if len(value) == 2 else Sdf.ValueTypeNames.Float3
        elif prop == 'autoheadlight':
            t, value = Sdf.ValueTypeNames.Bool, bool(value)
        elif isinstance(value, int):
            t = Sdf.ValueTypeNames.Int
        else:
            t = Sdf.ValueTypeNames.Float
        attr = Sdf.AttributeSpec(settings_prim, 'karma:global:' + prop, t)
        attr.default = value
    for prim, props in objects:
        Sdf.CreatePrimInLayer(layer, prim)
        for prop in props:
            attr = Sdf.Path(prim).AppendProperty('primvars:karma:object:' + prop)
            if not flat.GetAttributeAtPath(attr):
                raise RuntimeError('%s: the LOP did not author %s' % (name, attr))
            Sdf.CopySpec(flat, attr, layer, attr)

    def strip(path):  # Houdini's editor bookkeeping
        spec = layer.GetObjectAtPath(path)
        if spec is not None and 'customData' in spec.ListInfoKeys():
            spec.ClearInfo('customData')
    layer.Traverse(Sdf.Path.absoluteRootPath, strip)
    layer.comment = 'Generated by build_karma_looks.py (case %s): what Houdini\'s LOPs author.' % name
    layer.subLayerPaths.append('./' + geo)
    for key in ('renderSettingsPrimPath', 'framesPerSecond', 'timeCodesPerSecond', 'metersPerUnit', 'upAxis'):
        if flat.pseudoRoot.HasInfo(key):
            layer.pseudoRoot.SetInfo(key, flat.pseudoRoot.GetInfo(key))
    path = os.path.join(OUT, name + '.usda')
    layer.Export(path)
    return path


for case, (geo, settings, globals_, objects) in CASES.items():
    print('wrote', build(case, geo, (320, 240), settings, globals_, objects))
