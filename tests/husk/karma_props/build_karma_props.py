"""Writes the scenes of tests/husk/check_karma_props.py (docs/design/M11-P1.md):
one USDA per case, holding what Houdini's own LOPs author over one of the
geo_*.usda scenes (a subLayer): a Karma Render Settings LOP
(karmarendersettings) with the case's parameters and, for per-object
properties, a Render Geometry Settings LOP with values on its Karma tab.
So the files carry exactly the names, types and menu labels those nodes
write. The generated files are committed; rebuild them after a Houdini
update with an isolated hython:

  powershell -ExecutionPolicy Bypass -File tests\\houdini\\run_hython.ps1 -Script tests\\husk\\karma_props\\build_karma_props.py

Cases: name -> (geometry, render settings parameters, [(prim, {Karma
property: value})]). Every case renders its camera at the given
resolution; LOOK cases add the noise-free sampling of PATH (Path Traced,
uniform oracle), so a property's effect is not hidden by noise.
"""

import os
import sys

import hou
from pxr import Sdf

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else HERE

PATH = {'convergence_mode': 'Path Traced', 'oracle': 'uniform', 'pathtracedsamples': 512}
NOCLAMP = {'colorlimit': 1e6, 'colorlimitindirect': 1e6}

GLASS = ('geo_glass.usda', (240, 160))
ROUGH = ('geo_rough.usda', (240, 160))
CLAMP = ('geo_clamp.usda', (300, 150))
BASIC = ('geo_basic.usda', (192, 115))

CASES = {
    # Fake caustics (Karma's default causticsenable off) and their knobs.
    'glass_base': (GLASS, dict(PATH, **NOCLAMP), []),
    'glass_caustics': (GLASS, dict(PATH, causticsenable=1, causticsroughnessclamp=0, **NOCLAMP), []),
    'glass_caustics_clamp': (GLASS, dict(PATH, causticsenable=1, **NOCLAMP), []),
    'glass_fake_color': (GLASS, dict(PATH, **NOCLAMP), [('/World/ball', {'fakecausticscolor': (0.5, 0.5, 0.5)})]),
    'glass_fake_opacity': (GLASS, dict(PATH, **NOCLAMP), [('/World/ball', {'fakecausticsopacity': (0.5, 0.5, 0.5)})]),
    'glass_fake_nobsdf': (GLASS, dict(PATH, **NOCLAMP),
                          [('/World/ball', {'fakecausticsbsdfenable': 0, 'fakecausticscolor': (0.5, 0.5, 0.5)})]),
    'glass_fake_darken': (GLASS, dict(PATH, **NOCLAMP), [('/World/ball', {'fakecausticsfresneldarken': 0.5})]),
    'glass_internal': (GLASS, dict(PATH, **NOCLAMP), [('/World/ball', {'internalreflectenable': 1})]),
    # Roughness constrained along paths (Karma's default on).
    'rough_base': (ROUGH, dict(PATH, **NOCLAMP), []),
    'rough_free': (ROUGH, dict(PATH, constrainmaxrough=0, **NOCLAMP), []),
    # Colour limits: which contributions Karma clamps.
    'clamp_off': (CLAMP, dict(PATH, **NOCLAMP), []),
    'clamp_default': (CLAMP, dict(PATH), []),
    'clamp_1': (CLAMP, dict(PATH, colorlimit=1, colorlimitindirect=1), []),
    'clamp_indirect': (CLAMP, dict(PATH, colorlimit=1e6, colorlimitindirect=0.5, colorlimitindlinked=0), []),
    'clamp_direct': (CLAMP, dict(PATH, colorlimit=0.5, colorlimitindirect=1e6, colorlimitindlinked=0), []),
    # Everyday properties.
    'basic_base': (BASIC, dict(PATH), []),
    'basic_difflimit': (BASIC, dict(PATH), [('/World/ground', {'diffuselimit': 0})]),
    'basic_ribbons': (BASIC, dict(PATH, curve_style='Flat Ribbons'), []),
    'basic_discs': (BASIC, dict(PATH, point_style='Discs'), []),
    'basic_nomblur': (BASIC, dict(PATH, enablemblur=0), []),
    'basic_objmblur': (BASIC, dict(PATH), [('/World/mover', {'mblur': 0})]),
    'basic_box': (BASIC, dict(PATH, pixelfilter='box', pixelfilterscale=2), []),
    # Sampling: the node's defaults (Variance), and Path Traced with uniform samples.
    'sampling_default': (BASIC, {}, []),
    'sampling_path': (BASIC, {'convergence_mode': 'Path Traced', 'oracle': 'uniform', 'pathtracedsamples': 24}, []),
    'sampling_variance': (BASIC, {'samplesperpixel': 4, 'varianceaa_maxsamples': 6, 'oracle_variance': 0.02}, []),
}


def karma_parm(node, name):
    enc = hou.text.encodeParm('primvars:karma:object:' + name)
    return node.parmTuple(enc), node.parm(hou.text.encodeParm('primvars:karma:object:' + name + '_control'))


def build(name, geo, res, settings, objects):
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
    krs.parm('picture').set('karma_props.exr')  # husk -o replaces it
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
    krs.cook(force=True)
    if krs.errors():
        raise RuntimeError('%s: %s' % (name, krs.errors()))
    # What the LOPs authored: the flattened /Render, and the Karma
    # properties of the edited prims (the geometry stays in its file).
    flat = krs.stage().Flatten()
    layer = Sdf.Layer.CreateAnonymous('.usda')
    Sdf.CopySpec(flat, '/Render', layer, '/Render')
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
    layer.comment = 'Generated by build_karma_props.py (case %s): what Houdini\'s LOPs author.' % name
    layer.subLayerPaths.append('./' + geo)
    for key in ('renderSettingsPrimPath', 'framesPerSecond', 'timeCodesPerSecond', 'metersPerUnit', 'upAxis'):
        if flat.pseudoRoot.HasInfo(key):
            layer.pseudoRoot.SetInfo(key, flat.pseudoRoot.GetInfo(key))
    path = os.path.join(OUT, name + '.usda')
    layer.Export(path)
    return path


for case, ((geo, res), settings, objects) in CASES.items():
    print('wrote', build(case, geo, res, settings, objects))
