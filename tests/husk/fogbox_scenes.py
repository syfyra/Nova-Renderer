# Karma Fog Box scenes (docs/design/M11-P5.md): built with the actual
# Karma Fog Box LOP (karmafogbox) in an isolated hython, the LOP's prims
# copied into plain USDA scenes with a camera, lights, a ground and one
# RenderSettings prim that Karma CPU and Nova both read. The scenes are
# checked in (tests/husk/fogbox/*.usda); check_karma_fogbox.py renders them
# with Nova through husk and compares with Karma's measured values and the
# analytic transmittance.
#
#   powershell -File tests/houdini/run_hython.ps1 -Script tests/husk/fogbox_scenes.py <outdir>
#       [--reference <refscene>] [--only name,...]
#
# With --reference (tests/reference/scenes/fogbox.usda) it also writes the
# reference scene (tests/reference/README.md rules: common.usda, the shared
# settings class; Karma's reference image via make_references.py).
#
# The Fog Box with Add Noise is no mesh: it authors a Volume prim whose
# density field is an in-memory SOP volume (an op: path, which only the
# Houdini session that made it can resolve; the USD Render ROP saves such
# volumes to files). The generator saves it next to the scene as
# <scene>_<box>.bgeo.sc, which is not checked in: check_karma_fogbox.py runs
# the generator for that scene at test time.
#
# Regenerate after a Houdini update to catch changes of the LOP (the Karma
# values in check_karma_fogbox.py were measured with Houdini 22.0.466).
import os
import sys

import hou
from pxr import Sdf

OUT = os.path.abspath(sys.argv[1])
REFERENCE = os.path.abspath(sys.argv[sys.argv.index('--reference') + 1]) if '--reference' in sys.argv else None
ONLY = sys.argv[sys.argv.index('--only') + 1].split(',') if '--only' in sys.argv else None
SPP = 256
os.makedirs(OUT, exist_ok=True)


def save_sop_volumes(layer, name):
    """Saves the in-memory SOP volumes the layer's fields read (op: paths) to
    <OUT>/<name>_<root prim>.bgeo.sc and points the fields at them."""
    def visit(path):
        spec = layer.GetObjectAtPath(path)
        if not isinstance(spec, Sdf.AttributeSpec) or spec.name != 'filePath':
            return
        value = spec.default
        asset = value.path if hasattr(value, 'path') else str(value)
        if not asset.startswith('op:'):
            return
        sop = asset[3:].split('.sop.volumes')[0].split(':SDF_FORMAT_ARGS')[0]
        root = str(path.GetPrimPath()).split('/')[1]
        file_name = '%s_%s.bgeo.sc' % (name, root)
        hou.node(sop).geometry().saveToFile(os.path.join(OUT, file_name))
        spec.default = Sdf.AssetPath('./' + file_name)
    layer.Traverse(Sdf.Path.absoluteRootPath, visit)


def fogboxes(boxes):
    """The stage of a chain of Fog Box LOPs, one per (prim path, parms)."""
    stage = hou.node('/stage')
    for child in stage.children():
        child.destroy()
    last = None
    for path, parms in boxes:
        node = stage.createNode('karmafogbox')
        node.parm('primpath').set(path)
        for name, value in parms.items():
            node.parm(name).set(value)
        if last is not None:
            node.setInput(0, last)
        last = node
    last.cook(force=True)
    return last.stage().Flatten()


def clean(layer):
    """Drops Houdini's bookkeeping: the layer info prim and customData."""
    if layer.GetPrimAtPath('/HoudiniLayerInfo'):
        del layer.rootPrims['HoudiniLayerInfo']

    def visit(path):
        spec = layer.GetObjectAtPath(path)
        if isinstance(spec, (Sdf.PrimSpec, Sdf.AttributeSpec, Sdf.RelationshipSpec)) and spec.HasInfo('customData'):
            spec.ClearInfo('customData')
    layer.Traverse(Sdf.Path.absoluteRootPath, visit)


CAMERAS = {
    # Orthographic, looking down -Z: 6 x 2 units.
    'front': '''
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 60
        float verticalAperture = 20
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }''',
    # Orthographic, looking down -Y: x in [-4, 4], z in [-3, 3] (image top = -z).
    'top': '''
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 80
        float verticalAperture = 60
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 10, 0)
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }''',
    # Perspective, inside the big fog box, looking along -Z and down a little.
    'inside': '''
    def Camera "cam"
    {
        float focalLength = 25
        float horizontalAperture = 36
        float verticalAperture = 24
        float2 clippingRange = (0.01, 1000)
        double3 xformOp:translate = (0, 1.5, 4)
        float3 xformOp:rotateXYZ = (-12, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }''',
}

RESOLUTION = {'front': (180, 60), 'top': (160, 120), 'inside': (150, 100)}

GROUND = '''
    def Mesh "ground" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-20, 0, 20), (20, 0, 20), (20, 0, -20), (-20, 0, -20)]
        normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (interpolation = "vertex")
        float3[] extent = [(-20, 0, -20), (20, 0, 20)]
        rel material:binding = </mtl/grey>
    }'''

GREY = '''
def Scope "mtl"
{
    def Material "grey"
    {
        token outputs:mtlx:surface.connect = </mtl/grey/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.5, 0.5, 0.5)
            float inputs:specular = 0
            token outputs:out
        }
    }%s
}'''

# A distant light from +x, 45 degrees above the horizon (a sharp shadow).
SUN45 = '''
def DistantLight "sun"
{
    float inputs:intensity = 2
    float inputs:angle = 0
    float3 xformOp:rotateXYZ = (0, 90, 0)
    float3 xformOp:rotateXYZ:tilt = (-45, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ", "xformOp:rotateXYZ:tilt"]
}'''

RENDER = '''
def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        int2 resolution = (%d, %d)
        rel products = [</Render/Products/p>]
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1e-06
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = %d
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
        token nova:pixelFilter = "box"
        float nova:pixelFilterScale = 0
        int nova:samples = %d
        float nova:clampIndirect = 0
    }
    def Scope "Products"
    {
        def RenderProduct "p"
        {
            token productName = "out.exr"
            rel orderedVars = [</Render/Vars/C>]
        }
    }
    def Scope "Vars"
    {
        def RenderVar "C"
        {
            uniform token dataType = "color4f"
            uniform string sourceName = "C.*[LO]"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "color4f"
            string driver:parameters:aov:name = "C"
        }
    }
}
'''


def scene(name, boxes, camera, ground=False, light='', materials='', world='', edits=None):
    """Writes OUT/<name>.usda: the fog boxes and the rest of the scene."""
    if ONLY and name not in ONLY:
        return
    fog = fogboxes(boxes) if boxes else None
    res = RESOLUTION[camera]
    text = '#usda 1.0\n(\n    defaultPrim = "World"\n    metersPerUnit = 1\n    upAxis = "Y"\n)\n'
    text += 'def Xform "World"\n{%s%s%s\n}\n' % (CAMERAS[camera], GROUND if ground else '', world)
    text += light + '\n' + (GREY % materials) + '\n' + RENDER % (res[0], res[1], SPP, SPP)
    layer = Sdf.Layer.CreateAnonymous('.usda')
    layer.ImportFromString(text)
    if fog:
        clean(fog)
        for path, _ in boxes:
            root = Sdf.Path(path)
            Sdf.CopySpec(fog, root, layer, root)
    for edit in edits or []:
        edit(layer)
    save_sop_volumes(layer, name)
    layer.Export(os.path.join(OUT, name + '.usda'))
    print('wrote', name)


def set_primvar(path, name, value, kind=Sdf.ValueTypeNames.FloatArray):
    def edit(layer):
        prim = layer.GetPrimAtPath(path)
        attr = prim.attributes.get('primvars:' + name)
        if attr is None:
            attr = Sdf.AttributeSpec(prim, 'primvars:' + name, kind)
            attr.SetInfo('interpolation', 'constant')
        attr.default = value
    return edit


def remove_primvar(path, name):
    def edit(layer):
        prim = layer.GetPrimAtPath(path)
        if 'primvars:' + name in prim.attributes:
            prim.RemoveProperty(prim.attributes['primvars:' + name])
    return edit


ABSORB = dict(colorr=0, colorg=0, colorb=0)

# 1. Pure absorbers seen from the front: alpha = 1 - exp(-density * depth).
#    A: unit box, density 1 (depth 1); B: box scaled 2, density 0.25 (depth
#    2); C: sphere of radius 0.5, density 0.5.
scene('absorb', [('/fogA', dict(ABSORB, density=1.0, tx=-2.25)),
                 ('/fogB', dict(ABSORB, density=0.25, scale=2.0, tx=0.0)),
                 ('/fogC', dict(ABSORB, density=0.5, shape=1, tx=2.25))], 'front')

# 2. What sets the density: the material's (clouddensity 0.5) against the
#    prim's volumeuniformdensity (0.25), with volumeuniform 1 (A) and 0 (B);
#    C: the primvar volumeuniform removed (a volume-only material alone).
scene('density_source', [('/fogA', dict(ABSORB, density=0.5, tx=-2.25)),
                         ('/fogB', dict(ABSORB, density=0.5, tx=0.0)),
                         ('/fogC', dict(ABSORB, density=0.5, tx=2.25))], 'front',
      edits=[set_primvar('/fogA/geo/render', 'karma:object:volumeuniformdensity', [0.25]),
             set_primvar('/fogB/geo/render', 'karma:object:volumeuniformdensity', [0.25]),
             set_primvar('/fogB/geo/render', 'karma:object:volumeuniform', [False], Sdf.ValueTypeNames.BoolArray),
             remove_primvar('/fogC/geo/render', 'karma:object:volumeuniform'),
             remove_primvar('/fogC/geo/render', 'karma:object:volumeuniformdensity')])

# 3. Overlapping and nested absorbers: densities add.
scene('overlap', [('/fogA', dict(ABSORB, density=0.5, sx=2.0, tx=-1.0)),
                  ('/fogB', dict(ABSORB, density=0.25, sx=2.0, tx=0.0)),
                  ('/fogC', dict(ABSORB, density=0.5, scale=0.5, tx=2.25)),
                  ('/fogD', dict(ABSORB, density=0.25, tx=2.25))], 'front')

# 4. Shadows of floating absorbing boxes (density 1) with the Shadow Density
#    multiplier 0, 1 and 3, lit at 45 degrees: seen from above, the ground in
#    a box's shadow is lit through exp(-shadow * density * path).
scene('shadow', [('/fogA', dict(ABSORB, density=1.0, shadow=0.0, ty=2.0, tz=-2.0)),
                 ('/fogB', dict(ABSORB, density=1.0, shadow=1.0, ty=2.0, tz=0.0)),
                 ('/fogC', dict(ABSORB, density=1.0, shadow=3.0, ty=2.0, tz=2.0))], 'top', ground=True, light=SUN45)

# 5. Scattering boxes (white, grey, forward phase) over the ground.
scene('scatter', [('/fogA', dict(density=0.5, ty=2.0, tz=-2.0)),
                  ('/fogB', dict(density=1.0, colorr=0.8, colorg=0.5, colorb=0.2, ty=2.0, tz=0.0)),
                  ('/fogC', dict(density=1.0, phase=0.6, ty=2.0, tz=2.0))], 'top', ground=True, light=SUN45)

# 6. The camera inside a large fog box over the lit ground.
scene('inside', [('/fog', dict(density=0.05, scale=40.0))], 'inside', ground=True, light=SUN45)

# 7. Noise (the LOP's Add Noise, its defaults) on a scattering box and an
#    absorbing one.
NOISE = dict(addnoise=1, noisescale=0.5)
scene('noise', [('/fogA', dict(NOISE, density=1.0, ty=2.0, tz=-1.5)),
                ('/fogB', dict(NOISE, **ABSORB, density=2.0, ty=2.0, tz=1.5))], 'top', ground=True, light=SUN45)

# 8. Emission: a kma_volume that absorbs (1) and emits (0.5) on a unit cube
#    with volumeuniform: (emission / absorption) (1 - exp(-1)); no lights.
EMIT_MTL = '''
    def Material "emit"
    {
        token outputs:kma:volume.connect = </mtl/emit/v.outputs:out>
        def Shader "v"
        {
            uniform token info:id = "kma_volume"
            color3f inputs:absorption = (1, 1, 1)
            color3f inputs:emission = (0.5, 0.25, 0.125)
            token outputs:out
        }
    }'''
EMIT_WORLD = '''
    def Cube "emitter" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        double size = 1
        rel material:binding = </mtl/emit>
        bool[] primvars:karma:object:volumeuniform = [1] (interpolation = "constant")
        double3 xformOp:translate = (-1.5, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Cube "emitter_plain" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        double size = 1
        rel material:binding = </mtl/emit>
        double3 xformOp:translate = (1.5, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }'''
scene('emission', [], 'front', materials=EMIT_MTL, world=EMIT_WORLD)

# 9. Instanced uniform volumes (no LOP: plain USD): a point instancer over
#    an absorbing unit cube (the second instance twice as deep) and two
#    native instances of the same cube (the second at half its size). Every
#    instance is a medium: alpha = 1 - exp(-depth).
INSTANCE_MTL = '''
    def Material "absorb"
    {
        token outputs:kma:volume.connect = </mtl/absorb/v.outputs:out>
        def Shader "v"
        {
            uniform token info:id = "kma_volume"
            color3f inputs:absorption = (1, 1, 1)
            token outputs:out
        }
    }'''
INSTANCE_WORLD = '''
    def PointInstancer "fogs"
    {
        point3f[] positions = [(-2.25, 0, 0), (-0.75, 0, 0)]
        float3[] scales = [(1, 1, 1), (1, 1, 2)]
        int[] protoIndices = [0, 0]
        rel prototypes = [</World/fogs/proto>]
        def Cube "proto" (prepend apiSchemas = ["MaterialBindingAPI"])
        {
            double size = 1
            rel material:binding = </mtl/absorb>
            bool[] primvars:karma:object:volumeuniform = [1] (interpolation = "constant")
        }
    }
    def Xform "copyA" (
        instanceable = true
        references = </_FogAsset>
    )
    {
        double3 xformOp:translate = (0.75, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Xform "copyB" (
        instanceable = true
        references = </_FogAsset>
    )
    {
        double3 xformOp:translate = (2.25, 0, 0)
        float3 xformOp:scale = (0.5, 0.5, 0.5)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
    }'''
INSTANCE_ASSET = '''
class Xform "_FogAsset"
{
    def Cube "cube" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        double size = 1
        rel material:binding = </mtl/absorb>
        bool[] primvars:karma:object:volumeuniform = [1] (interpolation = "constant")
    }
}'''
scene('instances', [], 'front', light=INSTANCE_ASSET, materials=INSTANCE_MTL, world=INSTANCE_WORLD)

# The reference scene: three Fog Boxes (a white box around a grey block, a
# coloured tilted box, a dark box casting no shadow) on a ground under a key
# light; single scattering plus the light past it (volume limit 0, both
# renderers' default), diffuse limit 1. No Sphere shape: Karma tessellates
# it (about 1 % less optical depth through the middle than the exact sphere
# Nova renders; check_karma_fogbox.py's absorb scene).
REF_TEXT = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    subLayers = [@./common.usda@]
)

def Xform "World"
{
    def Mesh "ground" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-6, 0, 6), (6, 0, 6), (6, 0, -6), (-6, 0, -6)]
        color3f[] primvars:displayColor = [(0.5, 0.5, 0.5)] (interpolation = "constant")
        float3[] extent = [(-6, 0, -6), (6, 0, 6)]
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        rel material:binding = </Looks/displayColorLambert>
    }
    def Mesh "block" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 3, 2, 1, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5, 4, 5, 6, 7]
        point3f[] points = [(-0.35, 0, -0.35), (0.35, 0, -0.35), (0.35, 0.7, -0.35), (-0.35, 0.7, -0.35), (-0.35, 0, 0.35), (0.35, 0, 0.35), (0.35, 0.7, 0.35), (-0.35, 0.7, 0.35)]
        color3f[] primvars:displayColor = [(0.7, 0.7, 0.7)] (interpolation = "constant")
        float3[] extent = [(-0.35, 0, -0.35), (0.35, 0.7, 0.35)]
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        rel material:binding = </Looks/displayColorLambert>
    }
    def DistantLight "key"
    {
        float inputs:intensity = 3
        bool inputs:normalize = 1
        float3 xformOp:rotateXYZ = (-40, 35, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
    def Camera "cam"
    {
        float focalLength = 30
        float horizontalAperture = 36
        float verticalAperture = 20.25
        float2 clippingRange = (0.01, 10000)
        double3 xformOp:translate = (0, 2.4, 7.5)
        float3 xformOp:rotateXYZ = (-14, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
}

def Scope "Render"
{
    def RenderSettings "ref" (
        inherits = </_class_RefSettings>
    )
    {
        rel camera = </World/cam>
        int2 resolution = (160, 90)
        float nova:object:diffuseLimit = 1
        float karma:object:diffuselimit = 1
    }
}
'''
REF_BOXES = [('/fogWhite', dict(density=0.35, phase=0.3, sx=3.0, sy=1.4, sz=2.4, ty=0.7)),
             ('/fogTilted', dict(density=1.0, colorr=0.9, colorg=0.5, colorb=0.3, sx=1.1, sy=0.9, sz=1.1,
                                 ry=35.0, rz=20.0, tx=2.4, ty=0.9, tz=-0.6)),
             ('/fogDark', dict(density=0.8, shadow=0.0, colorr=0.2, colorg=0.2, colorb=0.2, sx=1.0, sy=1.2, sz=1.0,
                               tx=-2.5, ty=0.6, tz=0.4))]
if REFERENCE:
    fog = fogboxes(REF_BOXES)
    clean(fog)
    layer = Sdf.Layer.CreateAnonymous('.usda')
    layer.ImportFromString(REF_TEXT)
    for path, _ in REF_BOXES:
        Sdf.CopySpec(fog, Sdf.Path(path), layer, Sdf.Path(path))
    text = layer.ExportToString()
    # Anonymous layers resolve the sublayer path absolutely: keep it relative.
    text = text.replace(layer.subLayerPaths[0], './common.usda') if layer.subLayerPaths else text
    with open(REFERENCE, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('wrote', REFERENCE)
