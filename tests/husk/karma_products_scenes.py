# Karma Render Products, Standard Render Vars and Cryptomatte scenes
# (docs/design/M11-P6.md): the render prims are built in an isolated hython
# with the actual LOPs (Karma Standard Render Vars 2.0, Additional Render
# Vars, Karma Cryptomatte, Karma Render Products 2.0, Karma Render Settings
# with its deep camera map and checkpoint products) and copied into plain
# USDA scenes with fixed geometry and one RenderSettings prim that Karma CPU
# and Nova both read. The scenes are checked in (tests/husk/karma_products);
# check_karma_products.py renders them with Nova through husk and compares
# with Karma's values measured once (Houdini 22.0.466).
#
#   powershell -File tests/houdini/run_hython.ps1 -Script tests/husk/karma_products_scenes.py <outdir>
#
# Regenerate after a Houdini update to catch changes of the LOPs.
import os
import sys

import hou
from pxr import Sdf

OUT = os.path.abspath(sys.argv[1])
os.makedirs(OUT, exist_ok=True)


def clean(layer):
    """Drops Houdini's bookkeeping: the layer info prim and customData."""
    if layer.GetPrimAtPath('/HoudiniLayerInfo'):
        del layer.rootPrims['HoudiniLayerInfo']

    def visit(path):
        spec = layer.GetObjectAtPath(path)
        if isinstance(spec, (Sdf.PrimSpec, Sdf.AttributeSpec, Sdf.RelationshipSpec)) and spec.HasInfo('customData'):
            spec.ClearInfo('customData')
    layer.Traverse(Sdf.Path.absoluteRootPath, visit)


def lop_render_prims(nodes):
    """The flattened stage of a LOP chain: its /Render prims only."""
    stage = hou.node('/stage')
    for child in stage.children():
        child.destroy()
    last = None
    for type_name, parms, multiparms in nodes:
        node = stage.createNode(type_name)
        if last is not None:
            node.setInput(0, last)
        for name, count in multiparms:
            node.parm(name).set(count)
        for name, value in parms.items():
            node.parm(name).set(value)
        last = node
    last.cook(force=True)
    layer = last.stage().Flatten()
    clean(layer)
    return layer


def text_of(layer, path):
    """The USDA text of one prim (and its children) of a layer."""
    out = Sdf.Layer.CreateAnonymous('.usda')
    Sdf.CreatePrimInLayer(out, Sdf.Path(path).GetParentPath())
    Sdf.CopySpec(layer, path, out, path)
    text = out.ExportToString()
    return text[text.index('\n(') if False else text.index('def '):]


GEOMETRY_VARS = '''
def Xform "World"
{
    def Scope "m"
    {
        def Material "grey"
        {
            token outputs:mtlx:surface.connect = </World/m/grey/s.outputs:out>
            def Shader "s"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color = (0.5, 0.5, 0.5)
                float inputs:specular = 0
                token outputs:out
            }
        }
    }
    def Mesh "back" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4, 4]
        int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
        point3f[] points = [(-2, -1, 0), (-1.04, -1, 0), (1.03, -1, 0), (-2, 1, 0), (-1.04, 1, 0), (1.03, 1, 0)]
        texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)] (interpolation = "vertex")
        color3f[] primvars:pcol = [(0.5, 0.5, 0.5), (0, 0, 1)] (interpolation = "uniform")
        float[] primvars:ps = [-3, 1] (interpolation = "uniform")
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/m/grey>
    }
    def Mesh "front" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-0.5, -0.5, 1), (0.07, -0.5, 1), (0.07, 0.5, 1), (-0.5, 0.5, 1)]
        color3f primvars:pcol = (0, 0.6, 0) (interpolation = "constant")
        float primvars:ps = 2 (interpolation = "constant")
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/m/grey>
    }
    def Mesh "ramp" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(1.5, -1, 0), (1.95, -1, 0.9), (1.95, 1, 0.9), (1.5, 1, 0)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/m/grey>
        int primvars:karma:object:holdoutmode = 1 (interpolation = "constant")
    }
    def DistantLight "sun"
    {
        float inputs:intensity = 1
        float inputs:angle = 0
    }
    def Camera "cam"
    {
        token projection = "%(projection)s"
        float focalLength = 50
        float horizontalAperture = 40
        float verticalAperture = 20
        float2 clippingRange = (0.5, 50)
        double shutter:open = -0.25
        double shutter:close = 0.25
        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
'''

SETTINGS = '''
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        uniform int2 resolution = (%(w)d, %(h)d)
        rel products = [%(products)s]
        custom int karma:global:pathtracedsamples = %(spp)d
        custom int karma:global:samplesperpixel = %(spp)d
        custom string karma:global:pixeloracle = "uniform"
        custom string karma:global:pixelfilter = "box"
        custom token karma:global:convergence_mode = "Path Traced"
        custom int nova:samples = %(spp)d
        custom token nova:pixelFilter = "box"%(extra)s
    }
'''

HEADER = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    startTimeCode = 1
    endTimeCode = 1
    timeCodesPerSecond = 24
)
'''

# Extra vars of the Additional Render Vars LOP: name, dataType, sourceName,
# sourceType, filter, format. The depth and primary sample filters are
# Karma's own menu entries; the raw sources Karma's Extra Render Vars presets.
FILTERS = {
    'ubox': '["ubox",{}]', 'acc': '["accumulate",{}]', 'min': '["minmax",{"mode":"min"}]',
    'max': '["minmax",{"mode":"max"}]', 'zmin': '["minmax",{"mode":"zmin"}]', 'zmax': '["minmax",{"mode":"zmax"}]',
    'var': '["stats",{"mode":"var"}]', 'mse': '["stats",{"mode":"mse"}]', 'even': '["stats",{"mode":"even"}]',
    'odd': '["stats",{"mode":"odd"}]',
}
EXTRA = []
for key in ('ubox', 'acc', 'min', 'max', 'zmin', 'zmax', 'var', 'mse', 'even', 'odd'):
    EXTRA.append(('z_' + key, 'float', 'ray:hitPz', 'raw', FILTERS[key], 'float'))
for key in ('min', 'max', 'var'):
    EXTRA.append(('C_' + key, 'color3f', 'C.*[LO]', 'lpe', FILTERS[key], 'color3f'))
for key in ('ubox', 'min', 'max', 'zmin'):
    EXTRA.append(('pcol_' + key, 'color3f', 'pcol', 'primvar', FILTERS[key], 'color3f'))
    EXTRA.append(('ps_' + key, 'float', 'ps', 'primvar', FILTERS[key], 'float'))
for name, dt, src, fmt in [('mask', 'float', 'ray:mask', 'float'), ('time', 'float', 'ray:time', 'float'),
                           ('near', 'float', 'ray:near', 'float'), ('far', 'float', 'ray:far', 'float'),
                           ('rayorigin', 'point3f', 'ray:P', 'float3'), ('raydir', 'vector3f', 'ray:D', 'float3'),
                           ('contrib', 'color3f', 'ray:contrib', 'color3f'), ('dPdz', 'float', 'ray:dPdz', 'float'),
                           ('hituv', 'vector3f', 'ray:hituv', 'float3'),
                           ('N_facing', 'normal3f', 'ray:hitN_facing', 'float3'),
                           ('P_noholdouts', 'point3f', 'ray:noholdout;hitP', 'float3'),
                           ('P_holdouts', 'point3f', 'ray:holdout;hitP', 'float3'),
                           ('level', 'float', 'ray:level', 'float'), ('hitstack', 'float', 'ray:hitstack', 'float')]:
    EXTRA.append((name, dt, src, 'raw', FILTERS['ubox'], fmt))
EXTRA.append(('primarysamples', 'int', 'ray:pixelsamples', 'intrinsic', FILTERS['max'], 'float'))


def vars_scene(name, projection):
    parms = {'beauty': 1, 'combineddiffuse': 0, 'combinedglossyreflection': 0, 'combinedvolume': 0, 'hitP': 1,
             'hitPz': 1, 'primid': 1, 'element': 1, 'hitN': 1, 'hituv': 0, 'hitN_facingratio': 1}
    add = {}
    for i, (vname, dt, src, stype, filt, fmt) in enumerate(EXTRA, 1):
        add.update({'name%d' % i: vname, 'dataType%d' % i: dt, 'sourceName%d' % i: src, 'sourceType%d' % i: stype,
                    'filter%d' % i: filt, 'format%d' % i: fmt})
    product = {'products': 1, 'primname_0': 'p', 'productName_0': name + '.exr', 'camera': '/World/cam'}
    layer = lop_render_prims([
        ('karmastandardrendervars::2.0', parms, []),
        ('additionalrendervars', add, [('rendervars', len(EXTRA))]),
        ('karmarenderproducts::2.0', product, [('products', 1)]),
    ])
    render = text_of(layer, '/Render')
    settings = SETTINGS % {'w': 40, 'h': 20, 'products': '</Render/Products/p>', 'spp': 64, 'extra': ''}
    body = render.replace('def Scope "Render"\n{', 'def Scope "Render"\n{' + settings, 1)
    text = (HEADER + '\n# Built by tests/husk/karma_products_scenes.py (Karma LOPs, Houdini 22.0.466).\n' +
            GEOMETRY_VARS % {'projection': projection} + '\n' + body)
    with open(os.path.join(OUT, name + '.usda'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


GEOMETRY_DEEP = '''
def Xform "World"
{
    def Scope "m"
    {
        def Material "grey"
        {
            token outputs:mtlx:surface.connect = </World/m/grey/s.outputs:out>
            def Shader "s"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color = (0.5, 0.5, 0.5)
                float inputs:specular = 0
                token outputs:out
            }
        }
        def Material "half"
        {
            token outputs:mtlx:surface.connect = </World/m/half/s.outputs:out>
            def Shader "s"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color = (0.2, 0.8, 0.2)
                float inputs:specular = 0
                color3f inputs:opacity = (0.5, 0.5, 0.5)
                token outputs:out
            }
        }
    }
    def Mesh "back" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -1, 0), (1.03, -1, 0), (1.03, 1, 0), (-2, 1, 0)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/m/grey>
    }
    def Mesh "half" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, -1, 1), (0.53, -1, 1), (0.53, 1, 1), (-1, 1, 1)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/m/half>
    }
    def Mesh "ramp" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(1.5, -1, 0), (1.95, -1, 0.9), (1.95, 1, 0.9), (1.5, 1, 0)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/m/grey>
    }
    def DistantLight "sun"
    {
        float inputs:intensity = 1
        float inputs:angle = 0
    }
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 40
        float verticalAperture = 20
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
'''


def set_settings(layer, path, w, h, spp, products):
    """The settings prim the LOP authored, with this scene's camera, size and
    sampling (Karma's and Nova's names)."""
    prim = layer.GetPrimAtPath(path)
    rel = prim.relationships.get('camera') or Sdf.RelationshipSpec(prim, 'camera')
    rel.targetPathList.explicitItems = [Sdf.Path('/World/cam')]
    prel = prim.relationships.get('products')
    prel.targetPathList.explicitItems = [Sdf.Path(p) for p in products]

    def attr(name, type_name, value, custom=True):
        a = prim.attributes.get(name) or Sdf.AttributeSpec(prim, name, type_name, Sdf.VariabilityUniform
                                                           if name == 'resolution' else Sdf.VariabilityVarying,
                                                           declaresCustom=custom)
        a.default = value
    from pxr import Gf
    attr('resolution', Sdf.ValueTypeNames.Int2, Gf.Vec2i(w, h), custom=False)
    attr('karma:global:pathtracedsamples', Sdf.ValueTypeNames.Int, spp)
    attr('karma:global:samplesperpixel', Sdf.ValueTypeNames.Int, spp)
    attr('karma:global:pixeloracle', Sdf.ValueTypeNames.String, 'uniform')
    attr('karma:global:pixelfilter', Sdf.ValueTypeNames.String, 'box')
    attr('karma:global:convergence_mode', Sdf.ValueTypeNames.Token, 'Path Traced')
    attr('nova:samples', Sdf.ValueTypeNames.Int, spp)
    attr('nova:pixelFilter', Sdf.ValueTypeNames.Token, 'box')


def deep_scene(name, spp):
    """The Karma Render Settings LOP with its deep camera map (with the depth
    var) and checkpoint products."""
    parms = {'dcm': 1, 'dcmfilename': name + '_deep.exr', 'outputcheckpoint': 1, 'savefrequency': 0.5,
             'productName': name + '.checkpoint', 'picture': name + '.exr', 'hitPz': 1, 'resume': 1}
    stage = hou.node('/stage')
    for child in stage.children():
        child.destroy()
    node = stage.createNode('karmarendersettings', 'krs')
    for key, value in parms.items():
        if node.parm(key) is not None:
            node.parm(key).set(value)
        else:
            print('no parm', key)
    node.cook(force=True)
    layer = node.stage().Flatten()
    clean(layer)
    settings_path = None

    def find(path):
        nonlocal settings_path
        spec = layer.GetObjectAtPath(path)
        if isinstance(spec, Sdf.PrimSpec) and spec.typeName == 'RenderSettings':
            settings_path = str(path)
    layer.Traverse(Sdf.Path.absoluteRootPath, find)
    prods = [str(p) for p in layer.GetPrimAtPath(settings_path).relationships['products'].targetPathList.explicitItems]
    set_settings(layer, settings_path, 40, 20, spp, prods)
    for p in prods:  # file names relative to the render's directory
        spec = layer.GetPrimAtPath(p).attributes['productName']
        spec.ClearInfo('timeSamples')
        base = {'renderproduct': name + '.exr', 'dcm': name + '_deep.exr', 'checkpoint': name + '.checkpoint'}
        spec.default = base.get(p.split('/')[-1], spec.default)
    render = text_of(layer, '/Render')
    text = (HEADER + '\n# Built by tests/husk/karma_products_scenes.py (Karma LOPs, Houdini 22.0.466).\n' +
            GEOMETRY_DEEP + '\n' + render)
    with open(os.path.join(OUT, name + '.usda'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


CRYPTO_GEOMETRY = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'karma_products',
                                    'crypto_geometry.usda.in'), encoding='utf-8').read() \
    if os.path.exists(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'karma_products',
                                   'crypto_geometry.usda.in')) else None


def crypto_scene(name):
    """Karma Cryptomatte with prim and per-instance primvar layers."""
    parms = {'doprimcrypto': 1, 'domtlcrypto': 0, 'doprimvarcrypto': 1, 'primvarcrypto1': 'instname',
             'primvarcrypto2': 'piece'}
    layer = lop_render_prims([
        ('karmastandardrendervars::2.0', {'beauty': 1, 'combineddiffuse': 0, 'combinedglossyreflection': 0,
                                          'combinedvolume': 0, 'hitPz': 0, 'hituv': 0, 'hitN': 0}, []),
        ('karmacryptomatte', parms, [('primvars_folder', 2)]),
        ('karmarenderproducts::2.0', {'products': 1, 'primname_0': 'p', 'productName_0': name + '.exr',
                                      'camera': '/World/cam'}, [('products', 1)]),
    ])
    render = text_of(layer, '/Render')
    settings = SETTINGS % {'w': 160, 'h': 80, 'products': '</Render/Products/p>', 'spp': 16, 'extra': ''}
    body = render.replace('def Scope "Render"\n{', 'def Scope "Render"\n{' + settings, 1)
    text = (HEADER + '\n# Built by tests/husk/karma_products_scenes.py (Karma LOPs, Houdini 22.0.466).\n' +
            CRYPTO_GEOMETRY + '\n' + body)
    with open(os.path.join(OUT, name + '.usda'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


vars_scene('vars', 'orthographic')
vars_scene('vars_persp', 'perspective')
deep_scene('deep', 16)
if CRYPTO_GEOMETRY:
    crypto_scene('crypto_instances')
print('wrote', OUT)
