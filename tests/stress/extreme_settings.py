"""Extreme render settings and cut-off files through nova_cli: every render setting Nova reads
(its own from houdini/tools/nova_settings.json, Karma's from the names in hydra/) and every
per-object property as NaN, infinities, the limits of int and values of the wrong type, as a
render setting and as a primvar on every kind of prim; and .vdb and texture files cut off at
several lengths. About 1700 stages, checked like broken_stages.py's.

  $HFS/python313/python.exe extreme_settings.py --repo <repo> --cli <nova_cli.exe> --hfs <HFS> --out <dir>
        [--only pattern,...] [--devices cpu,xpu] [--jobs N] [--limit seconds]
"""
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import broken_stages as stress  # noqa: E402
from broken_stages import BALL, BEAUTY, MTLX, SS, case, curves, material, mesh, points, settings  # noqa: E402

stress.CASES.clear()

repo = sys.argv[sys.argv.index("--repo") + 1]
del sys.argv[sys.argv.index("--repo"):sys.argv.index("--repo") + 2]

# A scene that uses most of the renderer: glass and metal, subdivision and
# displacement, curves, points, an emissive mesh, a fog sphere, an instancer.
RICH = (
    BALL % "        rel material:binding = </World/mat>"
    + material(SS + ["float inputs:transmission = 0.6", "float inputs:specular_roughness = 0.2", "float inputs:coat = 0.5",
                     "float inputs:subsurface = 0.2"],
               '        def Shader "d"\n        {\n            uniform token info:id = "ND_displacement_float"\n            float inputs:displacement.connect = </World/mat/n.outputs:out>\n            float inputs:scale = 0.1\n            token outputs:out\n        }\n'
               '        def Shader "n"\n        {\n            uniform token info:id = "ND_noise3d_float"\n            float outputs:out\n        }\n',
               outputs=MTLX + '\n        token outputs:mtlx:displacement.connect = </World/mat/d.outputs:out>')
    + mesh("q", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                 "point3f[] points = [(-3, 0.2, -1), (-1.5, 0.2, -1), (-1.5, 2.2, -1), (-3, 2.2, -1)]",
                 'uniform token subdivisionScheme = "catmullClark"', "rel material:binding = </World/mat>"])
    + curves(['uniform token type = "cubic"', 'uniform token basis = "bspline"', "int[] curveVertexCounts = [4, 4]",
              "point3f[] points = [(2,0.2,0),(2.2,0.8,0),(2,1.4,0),(2.1,2,0),(2.5,0.2,0),(2.7,0.8,0),(2.5,1.4,0),(2.6,2,0)]",
              "float[] widths = [0.1]"])
    + points(["point3f[] points = [(-2,0.4,1.5),(-1.5,0.4,1.5),(-1,0.4,1.5)]", "float[] widths = [0.4]"])
    + mesh("emit", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                    "point3f[] points = [(1.5, 3, -1), (2.5, 3, -1), (2.5, 3, 0), (1.5, 3, 0)]",
                    'uniform token subdivisionScheme = "none"', "rel material:binding = </World/emat>"])
    + '\n    def Material "emat"\n    {\n        token outputs:surface.connect = </World/emat/s.outputs:surface>\n        def Shader "s"\n        {\n            uniform token info:id = "UsdPreviewSurface"\n            color3f inputs:emissiveColor = (8, 6, 4)\n            token outputs:surface\n        }\n    }\n'
    + '\n    def Sphere "fog"\n    {\n        double radius = 0.7\n        double3 xformOp:translate = (1.2, 0.8, 1.5)\n        uniform token[] xformOpOrder = ["xformOp:translate"]\n        rel material:binding = </World/vmat>\n    }\n'
    + '\n    def Material "vmat"\n    {\n        token outputs:mtlx:volume.connect = </World/vmat/s.outputs:out>\n        def Shader "s"\n        {\n            uniform token info:id = "ND_anisotropic_vdf"\n            color3f inputs:absorption = (0.5, 0.5, 0.5)\n            color3f inputs:scattering = (1.5, 1.5, 1.5)\n            token outputs:out\n        }\n    }\n'
    + '\n    def PointInstancer "pi"\n    {\n        rel prototypes = </World/pi/proto>\n        point3f[] positions = [(-0.5,0.2,2.5),(0.5,0.2,2.5)]\n        int[] protoIndices = [0, 0]\n        def Sphere "proto"\n        {\n            double radius = 0.2\n        }\n    }\n'
)

VARS = [BEAUTY, ("N", "normal3f", "N", "raw"), ("diff", "color3f", "C<RD>.*L", "lpe"), ("albedo", "color3f", "albedo", "raw"),
        ("depth", "float", "depth", "raw")]


def slug(text):
    return re.sub(r"[^a-zA-Z0-9]+", "_", text).strip("_")[:48]


INT_VALUES = ["-2147483648", "2147483647", "-1", "0"]
FLOAT_VALUES = ["nan", "inf", "-inf", "-1e30", "1e30", "0", "1e-30"]


def sweep(prefix, key, usd_type, value, extra=()):
    name = "%s_%s_%s" % (prefix, slug(key), slug(value))
    line = "%s %s = %s" % (usd_type, key, value)
    case(name, RICH, settings=settings([line], VARS), extra=["--settings", "/Render/rs", "--timeout", "25"] + list(extra))


with open(os.path.join(repo, "houdini", "tools", "nova_settings.json"), encoding="utf8") as f:
    nova_settings = json.load(f)["settings"]
for s in nova_settings:
    key, typ = s["key"], s["type"]
    if typ == "int":
        for v in INT_VALUES:
            sweep("set", key, "int", v)
        sweep("set", key, "float", "nan")
    elif typ == "float":
        for v in FLOAT_VALUES:
            sweep("set", key, "float", v)
    elif typ == "bool":
        sweep("set", key, "int", "7")
        sweep("set", key, "string", '"yes"')
    else:
        sweep("set", key, "string", '"bogus"')
        sweep("set", key, "string", '""')
        sweep("set", key, "float", "nan")
        sweep("set", key, "token", '"' + "x" * 3000 + '"')

# Karma's names, with every type they might be authored in.
karma = set()
for root, _, files in os.walk(os.path.join(repo, "hydra")):
    for fn in files:
        if fn.endswith((".cpp", ".h")):
            with open(os.path.join(root, fn), encoding="utf8", errors="replace") as f:
                karma.update(re.findall(r'"(karma:(?:global|object)[a-zA-Z0-9:_]*[a-zA-Z0-9_])"', f.read()))
for key in sorted(karma):
    for v in ("nan", "inf", "-1e30", "1e30"):
        sweep("kset", key, "float", v)
    for v in ("-2147483648", "2147483647"):
        sweep("kset", key, "int", v)
    sweep("kset", key, "string", '"bogus"')
    sweep("kset", key, "color3f", "(nan, -1, inf)")

# The object properties as primvars on every prim kind.
OBJECT_KEYS = sorted(k for k in karma if k.startswith("karma:object:")) + [
    s["key"] for s in nova_settings if s["key"].startswith("nova:object:")]


def with_primvar(line):
    """RICH with `line` added to every geometry prim."""
    out = RICH
    for marker in ('def Sphere "ball"\n    {\n', 'def Mesh "q"\n    {\n', 'def BasisCurves "c"\n    {\n', 'def Points "p"\n    {\n',
                   'def Mesh "emit"\n    {\n', 'def Sphere "fog"\n    {\n', 'def PointInstancer "pi"\n    {\n'):
        out = out.replace(marker, marker + "        " + line + "\n")
    return out


for key in OBJECT_KEYS:
    for typ, v in (("float", "nan"), ("float", "inf"), ("float", "-1e30"), ("float", "1e30"), ("int", "-2147483648"),
                   ("int", "2147483647"), ("string", '"bogus"')):
        name = "pv_%s_%s" % (slug(key), slug(v))
        case(name, with_primvar("%s primvars:%s = %s" % (typ, key, v)), settings=settings([], VARS),
             extra=["--settings", "/Render/rs", "--timeout", "25"])

# Truncated and broken asset files (an interrupted cache or texture write).
VOL = ('\n    def Volume "v"\n    {\n        rel field:density = </World/v/density>\n        def OpenVDBAsset "density"\n        {\n'
       '            asset filePath = @%s@\n            token fieldName = "%s"\n%s        }\n    }\n')
for frac in ("8b", "64b", "1k", "10pct", "50pct", "90pct", "99pct", "minus1"):
    case("vdb_trunc_" + frac, VOL % ("pyro_%s.vdb" % frac, "density", ""))
case("vdb_empty_file", VOL % ("empty.vdb", "density", ""))
case("vdb_is_directory", VOL % ("img", "density", ""))
case("vdb_field_missing", VOL % ("pyro_full.vdb", "no_such_grid", ""))
case("vdb_field_empty_name", VOL % ("pyro_full.vdb", "", ""))
case("vdb_field_index_huge", VOL % ("pyro_full.vdb", "density", "            int fieldIndex = 2147483647\n"))
case("vdb_field_index_negative", VOL % ("pyro_full.vdb", "density", "            int fieldIndex = -5\n"))
case("vdb_full", VOL % ("pyro_full.vdb", "density", ""))
case("vdb_flipped_bytes", VOL % ("pyro_flipped.vdb", "density", ""))

TEX = (BALL % "        rel material:binding = </World/mat>" + material(
    stress.PREVIEW + ["color3f inputs:diffuseColor.connect = </World/mat/tex.outputs:rgb>"],
    '        def Shader "tex"\n        {\n            uniform token info:id = "UsdUVTexture"\n            asset inputs:file = @%s@\n            float3 outputs:rgb\n        }\n',
    outputs=stress.PV))
DOME = '\n    def DomeLight "sky"\n    {\n        asset inputs:texture:file = @%s@\n        bool inputs:karma:light:renderlightgeo = 1\n    }\n'
for ext in ("exr", "png", "jpg", "tif", "hdr", "tx"):
    for frac in ("16b", "50pct", "99pct"):
        fn = "img_%s.%s" % (frac, ext)
        case("tex_trunc_%s_%s" % (ext, frac), TEX % fn)
        if ext in ("exr", "hdr"):
            case("dome_trunc_%s_%s" % (ext, frac), BALL % "", lights=DOME % fn)
    case("tex_empty_" + ext, TEX % ("empty." + ext))
case("tex_is_directory", TEX % "img")
case("tex_long_path", TEX % ("a" * 400 + ".exr"))
case("tex_weird_chars", TEX % "we ird<>|?*name.exr")
case("tex_huge_dims_header", TEX % "hugeheader.exr")
case("dome_is_directory", BALL % "", lights=DOME % "img")
case("dome_rat_missing", BALL % "", lights=DOME % "nothing.rat")
case("dome_rat_corrupt", BALL % "", lights=DOME % "corrupt.rat")
case("ies_corrupt", BALL % "" + stress.light("SphereLight", ["float inputs:radius = 0.2", "float inputs:intensity = 50",
                                                             "asset inputs:shaping:ies:file = @corrupt.ies@"] + stress.POSE))


def write_assets2(out, hfs):
    import numpy as np
    import OpenImageIO as oiio
    src = os.path.join(repo, "tests", "data", "volumes", "pyro.vdb")
    data = open(src, "rb").read()
    n = len(data)
    cuts = {"8b": 8, "64b": 64, "1k": 1024, "10pct": n // 10, "50pct": n // 2, "90pct": n * 9 // 10, "99pct": n * 99 // 100,
            "minus1": n - 1}
    for name, size in cuts.items():
        with open(os.path.join(out, "pyro_%s.vdb" % name), "wb") as f:
            f.write(data[:size])
    shutil.copyfile(src, os.path.join(out, "pyro_full.vdb"))
    flipped = bytearray(data)
    for i in range(200, n, 997):  # sprinkle corrupted bytes through the file
        flipped[i] ^= 0xFF
    with open(os.path.join(out, "pyro_flipped.vdb"), "wb") as f:
        f.write(flipped)
    open(os.path.join(out, "empty.vdb"), "wb").close()
    with open(os.path.join(out, "corrupt.rat"), "wb") as f:
        f.write(b"\x00\x01garbage" * 100)
    with open(os.path.join(out, "corrupt.ies"), "wb") as f:
        f.write(b"IESNA:LM-63-2002\nTILT=NONE\n1 -1 nan 99999999 99999999 1 1 0 0 0\n")
    rng = np.random.default_rng(1)
    img = rng.random((64, 64, 3), dtype=np.float32)
    for ext in ("exr", "png", "jpg", "tif", "hdr", "tx"):
        full = os.path.join(out, "img_full." + ext)
        spec = oiio.ImageSpec(64, 64, 3, oiio.FLOAT if ext in ("exr", "hdr", "tx") else oiio.UINT8)
        target = full if ext != "tx" else os.path.join(out, "img_full_tx.exr")
        o = oiio.ImageOutput.create(target)
        if o is None:
            continue
        o.open(target, spec)
        o.write_image(img if ext in ("exr", "hdr", "tx") else (img * 255).astype(np.uint8))
        o.close()
        if ext == "tx":
            shutil.copyfile(target, full)
        blob = open(full, "rb").read()
        for name, size in (("16b", 16), ("50pct", len(blob) // 2), ("99pct", len(blob) * 99 // 100)):
            with open(os.path.join(out, "img_%s.%s" % (name, ext)), "wb") as f:
                f.write(blob[:size])
        open(os.path.join(out, "empty." + ext), "wb").close()
    # An EXR header that claims a 2^30 x 2^30 image.
    blob = bytearray(open(os.path.join(out, "img_full.exr"), "rb").read())
    for key in (b"dataWindow\x00box2i\x00", b"displayWindow\x00box2i\x00"):
        at = blob.find(key)
        if at >= 0:
            at += len(key) + 4
            blob[at:at + 16] = (0).to_bytes(4, "little") * 2 + (1 << 30).to_bytes(4, "little") * 2
    with open(os.path.join(out, "hugeheader.exr"), "wb") as f:
        f.write(blob)


_write_assets = stress.write_assets


def both(out, hfs):
    _write_assets(out, hfs)
    write_assets2(out, hfs)


stress.write_assets = both

if __name__ == "__main__":
    stress.main()
