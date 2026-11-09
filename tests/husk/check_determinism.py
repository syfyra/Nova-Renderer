"""husk check that a scene renders the same image every time.

Hydra syncs rprims in parallel and iterates sprims through hash maps, so the
order in which Nova creates its meshes, lights and volumes differs from run
to run. Nothing that changes pixels may depend on that order: stochastic
opacity and the per-strand random number hash a stable key of the prim's
path (not its creation index), and lights, emissive triangles and volumes are
listed in the order of their keys (docs/bug-audit-2026-11.md, D-1).

Every scene is rendered several times with Nova (CPU) and Nova XPU (when it
renders on this machine) at husk's default thread count; every run must be
bit-identical to the first, in every AOV (beauty, primId, instanceId,
cryptomatte). Scenes:
- opacity: 24 partly transparent quads (one mesh prim each) and a point
  instancer of partly transparent quads in front of a wall;
- lights: eight coloured sphere lights over a ground;
- emitters: four emissive meshes treated as light sources;
- hair: twelve curve prims with Karma Hair's melanin randomisation;
- fog: the Karma Fog Box scatter scene (husk/fogbox), its three boxes moved
  to overlap.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_determinism.py --hfs ... --stage ... --out ...
"""

import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

HERE = os.path.dirname(os.path.abspath(__file__))

HEADER = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
'''

SETTINGS = '''
def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        int2 resolution = (%(res)s)
        int nova:samples = %(spp)d
        token nova:pixelFilter = "gauss"
        rel products = </Render/Products/p>
    }
    def Scope "Products"
    {
        def RenderProduct "p"
        {
            token productName = "out.exr"
            rel orderedVars = [</Render/Vars/C>, </Render/Vars/primId>, </Render/Vars/instanceId>,
                               </Render/Vars/CryptoObject>]
        }
    }
    def Scope "Vars"
    {
        def RenderVar "C" (prepend apiSchemas = ["HuskRenderVarAPI"])
        {
            uniform token dataType = "color4f"
            uniform string sourceName = "C.*[LO]"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:husk:format = "color4f"
            string driver:parameters:aov:husk:name = "C"
        }
        def RenderVar "primId" (prepend apiSchemas = ["HuskRenderVarAPI"])
        {
            string driver:parameters:aov:husk:name = "primId"
            uniform string sourceName = "primId"
            uniform token sourceType = "raw"
            uniform token dataType = "int"
        }
        def RenderVar "instanceId" (prepend apiSchemas = ["HuskRenderVarAPI"])
        {
            string driver:parameters:aov:husk:name = "instanceId"
            uniform string sourceName = "instanceId"
            uniform token sourceType = "raw"
            uniform token dataType = "int"
        }
        def RenderVar "CryptoObject" (prepend apiSchemas = ["KarmaRenderVarAPI", "HuskRenderVarAPI"])
        {
            uniform token dataType = "float3"
            token driver:parameters:aov:husk:format = "float3"
            bool driver:parameters:aov:karma:cryptomatte = 1
            int driver:parameters:aov:karma:cryptomatterank = 4
            string driver:parameters:aov:karma:cryptomattesidecar = ""
            string driver:parameters:aov:husk:name = "CryptoObject"
            uniform string sourceName = "__name"
        }
    }
}
'''

CAMERA = '''
    def Camera "cam"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 36
        float2 clippingRange = (0.01, 1000)
        double3 xformOp:translate = (0, 1.5, 7)
        float3 xformOp:rotateXYZ = (-8, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
'''


def surface(name, color, opacity=1.0, emission=0.0, specular=0.0):
    return '''
    def Material "%s"
    {
        token outputs:mtlx:surface.connect = </World/mtl/%s/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (%g, %g, %g)
            float inputs:specular = %g
            color3f inputs:opacity = (%g, %g, %g)
            float inputs:emission = %g
            color3f inputs:emission_color = (%g, %g, %g)
            token outputs:out
        }
    }
''' % (name, name, *color, specular, opacity, opacity, opacity, emission, *color)


def quad(name, center, size, material, axis='z', extra=''):
    x, y, z = center
    h = size / 2
    if axis == 'z':  # facing +z
        pts = [(x - h, y - h, z), (x + h, y - h, z), (x + h, y + h, z), (x - h, y + h, z)]
    else:  # facing +y
        pts = [(x - h, y, z + h), (x + h, y, z + h), (x + h, y, z - h), (x - h, y, z - h)]
    return '''
    def Mesh "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [%s]
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        rel material:binding = </World/mtl/%s>%s
    }
''' % (name, ', '.join('(%g, %g, %g)' % p for p in pts), material, extra)


def stage(body, materials, res='96, 96', spp=8, root=''):
    return (HEADER + 'def Xform "World"\n{\n' + CAMERA + body + '    def Scope "mtl"\n    {\n' + materials +
            '    }\n}\n' + root + SETTINGS % {'res': res, 'spp': spp})


def backdrop():
    return quad('ground', (0, 0, 0), 30, 'grey', axis='y') + quad('wall', (0, 4, -3), 12, 'grey')


SUN = '''
    def DistantLight "sun"
    {
        float inputs:intensity = 2
        float inputs:angle = 2
        float3 xformOp:rotateXYZ = (-50, 30, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
'''


def opacity_scene():
    body = backdrop() + SUN
    k = 0
    for i in range(6):
        for j in range(4):
            body += quad('veil%02d' % k, (-2.5 + i, 0.5 + j * 0.8, 1.0 + 0.15 * ((i + j) % 3)), 1.3, 'veil')
            k += 1
    body += '''
    def PointInstancer "panes" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        rel prototypes = [</World/panes/protos/pane>]
        int[] protoIndices = [0, 0, 0, 0, 0, 0, 0, 0]
        point3f[] positions = [(-2, 2.6, 2.2), (-1.4, 2.6, 2.3), (-0.8, 2.6, 2.4), (-0.2, 2.6, 2.5), (0.4, 2.6, 2.2),
                               (1.0, 2.6, 2.3), (1.6, 2.6, 2.4), (2.2, 2.6, 2.5)]
        def Scope "protos"
        {
''' + quad('pane', (0, 0, 0), 0.9, 'veil2').replace('\n    ', '\n            ') + '''
        }
    }
'''
    # Native instances of a partly transparent card.
    for k in range(4):
        body += '''
    def Xform "card%d" (instanceable = true
        prepend references = </CardProto>)
    {
        double3 xformOp:translate = (%g, 0.5, 2.6)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
''' % (k, -1.5 + 0.8 * k)
    proto = 'class Xform "CardProto"\n{\n' + quad('card', (0, 0, 0), 0.9, 'veil2') + '}\n'
    mats = surface('grey', (0.5, 0.5, 0.5)) + surface('veil', (0.9, 0.3, 0.2), opacity=0.5) + \
        surface('veil2', (0.2, 0.4, 0.9), opacity=0.35)
    return stage(body, mats, root=proto)


def lights_scene():
    body = backdrop()
    colors = [(1, 0.2, 0.2), (0.2, 1, 0.2), (0.2, 0.2, 1), (1, 1, 0.2), (1, 0.2, 1), (0.2, 1, 1), (1, 0.6, 0.2),
              (0.6, 0.2, 1)]
    for i, c in enumerate(colors):
        body += '''
    def SphereLight "lamp%d"
    {
        float inputs:intensity = 6
        float inputs:radius = 0.15
        color3f inputs:color = (%g, %g, %g)
        double3 xformOp:translate = (%g, %g, %g)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
''' % (i, *c, -3.5 + i, 1.0 + 0.4 * (i % 3), -1.5 + 0.5 * (i % 4))
    return stage(body, surface('grey', (0.5, 0.5, 0.5)))


def emitters_scene():
    body = backdrop()
    extra = '\n        int primvars:karma:object:treat_as_lightsource = 1'
    for i in range(4):
        body += quad('glow%d' % i, (-2.4 + 1.6 * i, 1.6 + 0.3 * (i % 2), 0.5 * (i % 3)), 0.6, 'glow%d' % i,
                     axis='y', extra=extra)
    mats = surface('grey', (0.5, 0.5, 0.5))
    for i, c in enumerate([(4, 1, 1), (1, 4, 1), (1, 1, 4), (3, 3, 1)]):
        mats += surface('glow%d' % i, c, emission=1.0)
    return stage(body, mats)


def hair_scene():
    body = backdrop() + SUN
    for i in range(12):
        x = -2.4 + 0.4 * i
        pts = ', '.join('(%g, %g, %g)' % (x + 0.15 * k, 0.2 + 0.35 * k, 1.5 + 0.02 * (i % 3)) for k in range(7))
        body += '''
    def BasisCurves "strand%02d" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        uniform token type = "linear"
        uniform token wrap = "nonperiodic"
        int[] curveVertexCounts = [7]
        point3f[] points = [%s]
        float[] widths = [0.08] (interpolation = "constant")
        rel material:binding = </World/mtl/hair>
    }
''' % (i, pts)
    mats = surface('grey', (0.5, 0.5, 0.5)) + '''
    def Material "hair"
    {
        token outputs:kma:surface.connect = </World/mtl/hair/hair.outputs:surface>
        def Shader "hair"
        {
            uniform token info:id = "kma_hair"
            float inputs:melanin = 0.5
            float inputs:melaninRedness = 0
            float inputs:melaninRandomize = 1
            float inputs:shift = 0
            token outputs:surface
        }
    }
'''
    return stage(body, mats)


# The Fog Box scatter scene (husk/fogbox) with its three boxes moved to
# overlap, so that rays cross several volumes at once.
FOG_OVERRIDE = '''#usda 1.0
(
    subLayers = [@%s@]
)

over "fogB"
{
    matrix4d xformOp:transform:karmafogbox2 = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0.4, 2.2, -1.5, 1) )
}

over "fogC"
{
    matrix4d xformOp:transform:karmafogbox3 = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (-0.3, 1.8, -2.4, 1) )
}

over "Render"
{
    over "s"
    {
        int nova:samples = 8
        int2 resolution = (80, 60)
    }
}
'''


def fog_scene():
    return FOG_OVERRIDE % os.path.join(HERE, 'fogbox', 'scatter.usda').replace('\\', '/')


SCENES = {
    'opacity': opacity_scene,
    'lights': lights_scene,
    'emitters': emitters_scene,
    'hair': hair_scene,
    'fog': fog_scene,
}


def read_parts(oiio, np, path):
    """Every subimage of an EXR as (name, float array), or None."""
    image = oiio.ImageInput.open(path)
    if not image:
        return None
    parts = []
    k = 0
    while image.seek_subimage(k, 0):
        spec = image.spec()
        px = np.asarray(image.read_image(k, 0, 0, spec.nchannels, oiio.FLOAT))
        parts.append((','.join(spec.channelnames), px))
        k += 1
    image.close()
    return parts


def digest(parts):
    h = hashlib.sha1()
    for name, px in parts:
        h.update(name.encode())
        h.update(px.tobytes())
    return h.hexdigest()[:16]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--only", nargs="*", help="scene names (default: all)")
    parser.add_argument("--renderers", nargs="*", default=["cpu", "xpu"])
    parser.add_argument("--keep", action="store_true", help="keep every run's EXR")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)
    plugins = {"cpu": "Nova", "xpu": "HdNovaXpuRendererPlugin"}
    try:
        for name, make in SCENES.items():
            if args.only and name not in args.only:
                continue
            usd = os.path.join(args.out, name + ".usda")
            with open(usd, "w", encoding="utf-8", newline="\n") as f:
                f.write(make())
            for tag in args.renderers:
                first = None
                hashes = []
                for r in range(args.runs):
                    exr = os.path.join(args.out, f"{name}_{tag}_{r}.exr")
                    if os.path.exists(exr):
                        os.remove(exr)
                    proc = run([tool(args, "husk.exe"), "--renderer", plugins[tag], "--headlight", "none", "-s",
                                "/Render/s", "-o", exr, usd], env, args.out)
                    if proc.returncode != 0 or not os.path.exists(exr):
                        check(False, f"husk {name} ({tag}) run {r} exits with 0 and writes its image")
                        break
                    log = proc.stderr + proc.stdout
                    if tag == "xpu" and r == 0 and ("rendering on the CPU" in log or "on the CPU\n" in log):
                        print(f"skip {name} (xpu): Nova XPU does not render on this machine")
                        break
                    parts = read_parts(oiio, np, exr)
                    hashes.append(digest(parts))
                    if first is None:
                        first = parts
                        beauty = parts[0][1]
                        check(float(beauty[..., :3].mean()) > 0.0, f"{name} ({tag}): the image is not black")
                    else:
                        for (pname, a), (_, b) in zip(first, parts):
                            same = np.array_equal(a.view(np.uint32), b.view(np.uint32))
                            if not same:
                                diff = np.abs(a.astype(np.float64) - b.astype(np.float64))
                                print(f"     {name} ({tag}) run {r} part {pname}: {int((diff > 0).any(axis=-1).sum())} "
                                      f"pixels differ, max {float(diff.max()):.4g}, mean of run 0 "
                                      f"{float(a.mean()):.6g} vs {float(b.mean()):.6g}")
                        if not args.keep:
                            os.remove(exr)
                if hashes:
                    check(len(set(hashes)) == 1,
                          f"{name} ({tag}): {len(hashes)} runs bit-identical ({', '.join(hashes)})")
    finally:
        import shutil
        shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
