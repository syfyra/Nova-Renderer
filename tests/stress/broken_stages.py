"""Broken stages through nova_cli: about 340 stages with inconsistent or non-finite geometry,
lights, cameras, instancers, materials, volumes and render settings, each rendered on the chosen
devices and checked for its exit code, a time limit and non-finite pixels
(docs/bug-audit-2026-11.md, section 12).

  $HFS/python313/python.exe broken_stages.py --cli <nova_cli.exe> --hfs <HFS> --out <dir>
        [--only pattern,...] [--skip pattern,...] [--devices cpu,xpu] [--jobs N] [--limit seconds]

Every line that does not end in "ok" is a finding: RC=<code> (a crash code, 3 = the render did
not converge within nova_cli's --timeout, TIMEOUT = the process did not return), NONFINITE=<n>,
noimage. Exit codes 2 with a message are refusals (no camera, no resolution) and expected for the
stages that ask for them. The stage of a case is <out>/<case>.usda, its output <out>/log.
"""
import argparse
import concurrent.futures as cf
import fnmatch
import json
import os
import subprocess
import sys
import time

HEADER = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
%(layer)s
)
'''

CAMERA = '''
    def Camera "cam"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 24
        float2 clippingRange = (0.1, 1000)
        double3 xformOp:translate = (0, 1.5, 7)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
'''

LIGHTS = '''
    def DomeLight "sky"
    {
        float inputs:intensity = 0.4
        bool inputs:karma:light:renderlightgeo = 1
    }
    def RectLight "key"
    {
        float inputs:intensity = 30
        float inputs:width = 2
        float inputs:height = 2
        double3 xformOp:translate = (0, 5, 0)
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
'''

GROUND = '''
    def Mesh "ground"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-10, 0, 10), (10, 0, 10), (10, 0, -10), (-10, 0, -10)]
        uniform token subdivisionScheme = "none"
        color3f[] primvars:displayColor = [(0.5, 0.5, 0.5)]
    }
'''

BALL = '''
    def Sphere "ball"
    {
        double radius = 1
        double3 xformOp:translate = (0, 1, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
%s
    }
'''

QUAD_PTS = "[(-1, 0.2, 1), (1, 0.2, 1), (1, 2.2, 1), (-1, 2.2, 1)]"

CASES = {}


def case(name, body, camera=CAMERA, lights=LIGHTS, ground=GROUND, layer="", settings="", extra=()):
    text = HEADER % {"layer": layer} + '\ndef Xform "World"\n{\n' + camera + lights + ground + body + "\n}\n" + settings
    CASES[name] = (apply_binding_api(text), list(extra))


def apply_binding_api(text):
    """USD resolves material:binding only on prims that apply MaterialBindingAPI."""
    lines = text.split("\n")
    out = list(lines)
    for i, line in enumerate(lines):
        if "rel material:binding" not in line:
            continue
        indent = len(line) - len(line.lstrip())
        for j in range(i - 1, -1, -1):
            prim = lines[j]
            if prim.lstrip().startswith("def ") and len(prim) - len(prim.lstrip()) < indent:
                pad = prim[:len(prim) - len(prim.lstrip())]
                if "MaterialBindingAPI" in out[j]:
                    break
                if prim.rstrip().endswith("("):
                    out[j] = prim + "\n" + pad + '    prepend apiSchemas = ["MaterialBindingAPI"]'
                else:
                    out[j] = prim + ' (\n' + pad + '    prepend apiSchemas = ["MaterialBindingAPI"]\n' + pad + ")"
                break
    return "\n".join(out)


def mesh(name, attrs, prim="Mesh"):
    return '\n    def %s "%s"\n    {\n%s\n    }\n' % (prim, name, "\n".join("        " + a for a in attrs))


def settings(lines, vars_=None, product=None):
    out = '\ndef Scope "Render"\n{\n    def RenderSettings "rs"\n    {\n'
    out += '        rel camera = </World/cam>\n        int2 resolution = (64, 48)\n'
    for l in lines:
        out += "        " + l + "\n"
    if vars_ is not None:
        out += '        rel products = </Render/prod>\n'
    out += "    }\n"
    if vars_ is not None:
        out += '    def RenderProduct "prod"\n    {\n        token productName = "out.exr"\n'
        for l in (product or []):
            out += "        " + l + "\n"
        out += "        rel orderedVars = [%s]\n    }\n" % ", ".join("</Render/%s>" % v[0] for v in vars_)
        for v in vars_:
            out += '    def RenderVar "%s"\n    {\n        token dataType = "%s"\n        string sourceName = "%s"\n        token sourceType = "%s"\n' % (
                v[0], v[1], v[2], v[3])
            for l in (v[4] if len(v) > 4 else []):
                out += "        " + l + "\n"
            out += "    }\n"
    out += "}\n"
    return out


# ---------------------------------------------------------------- meshes
case("base", BALL % "")
case("mesh_empty", mesh("m", ["int[] faceVertexCounts = []", "int[] faceVertexIndices = []", "point3f[] points = []"]))
case("mesh_no_attrs", mesh("m", []))
case("mesh_points_only", mesh("m", ["point3f[] points = %s" % QUAD_PTS]))
case("mesh_counts_exceed", mesh("m", ["int[] faceVertexCounts = [4, 4, 3]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                      "point3f[] points = %s" % QUAD_PTS]))
case("mesh_counts_short", mesh("m", ["int[] faceVertexCounts = [3]", "int[] faceVertexIndices = [0, 1, 2, 3, 0, 2]",
                                     "point3f[] points = %s" % QUAD_PTS]))
case("mesh_index_oob", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 99]",
                                  "point3f[] points = %s" % QUAD_PTS]))
case("mesh_index_negative", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, -1, 2, 3]",
                                       "point3f[] points = %s" % QUAD_PTS]))
case("mesh_index_huge", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2147483647, 3]",
                                   "point3f[] points = %s" % QUAD_PTS]))
case("mesh_count_negative", mesh("m", ["int[] faceVertexCounts = [-4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                       "point3f[] points = %s" % QUAD_PTS]))
case("mesh_count_zero_faces", mesh("m", ["int[] faceVertexCounts = [0, 1, 2, 4, 0]",
                                         "int[] faceVertexIndices = [0, 0, 1, 0, 1, 2, 3]",
                                         "point3f[] points = %s" % QUAD_PTS]))
case("mesh_count_huge", mesh("m", ["int[] faceVertexCounts = [2147483647]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                   "point3f[] points = %s" % QUAD_PTS]))
case("mesh_nan_points", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                   "point3f[] points = [(-1, 0.2, 1), (nan, 0.2, 1), (1, inf, 1), (-1, 2.2, -inf)]"]))
case("mesh_all_nan", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                "point3f[] points = [(nan, nan, nan), (nan, nan, nan), (nan, nan, nan), (nan, nan, nan)]"]))
case("mesh_huge_points", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                    "point3f[] points = [(-1e30, 0.2, 1), (1e30, 0.2, 1), (1e30, 1e30, 1), (-1e30, 1e30, 1)]"]))
case("mesh_fltmax_points", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                      "point3f[] points = [(-3.4e38, 0.2, 1), (3.4e38, 0.2, 1), (3.4e38, 3.4e38, 1), (-3.4e38, 3.4e38, 1)]"]))
case("mesh_degenerate", mesh("m", ["int[] faceVertexCounts = [3, 3, 4]",
                                   "int[] faceVertexIndices = [0, 0, 0, 0, 1, 1, 0, 1, 2, 3]",
                                   "point3f[] points = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)]"]))
case("mesh_ngon_concave", mesh("m", ["int[] faceVertexCounts = [8]", "int[] faceVertexIndices = [0, 1, 2, 3, 4, 5, 6, 7]",
                                     "point3f[] points = [(-1,0.2,1),(0,0.9,1),(1,0.2,1),(0.3,1.2,1),(1,2.2,1),(0,1.5,1),(-1,2.2,1),(-0.3,1.2,1)]"]))
case("mesh_normals_short", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                      "point3f[] points = %s" % QUAD_PTS, "normal3f[] normals = [(0, 0, 1)]",
                                      'uniform token subdivisionScheme = "none"']))
case("mesh_normals_zero", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                     "point3f[] points = %s" % QUAD_PTS,
                                     "normal3f[] normals = [(0, 0, 0), (0, 0, 0), (nan, 0, 0), (inf, 0, 0)]",
                                     'uniform token subdivisionScheme = "none"']))
case("mesh_normals_fv_short", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                         "point3f[] points = %s" % QUAD_PTS,
                                         'normal3f[] primvars:normals = [(0, 0, 1), (0, 0, 1)] (\n            interpolation = "faceVarying"\n        )',
                                         'uniform token subdivisionScheme = "none"']))
case("mesh_color_short", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                    "point3f[] points = %s" % QUAD_PTS,
                                    'color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0)] (\n            interpolation = "vertex"\n        )']))
case("mesh_color_indexed_oob", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                          "point3f[] points = %s" % QUAD_PTS,
                                          'color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0)] (\n            interpolation = "vertex"\n        )',
                                          "int[] primvars:displayColor:indices = [0, 1, 5, -3]"]))
case("mesh_color_nan", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                  "point3f[] points = %s" % QUAD_PTS,
                                  "color3f[] primvars:displayColor = [(nan, inf, -1)]"]))
case("mesh_color_empty", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                    "point3f[] points = %s" % QUAD_PTS, "color3f[] primvars:displayColor = []"]))
case("mesh_opacity_weird", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                      "point3f[] points = %s" % QUAD_PTS,
                                      "float[] primvars:displayOpacity = [nan]"]))
case("mesh_subdiv_nonmanifold", mesh("m", [
    "int[] faceVertexCounts = [4, 4, 4]", "int[] faceVertexIndices = [0, 1, 2, 3, 0, 1, 4, 5, 0, 1, 6, 7]",
    "point3f[] points = [(-1,0.2,1),(1,0.2,1),(1,2.2,1),(-1,2.2,1),(1,0.2,3),(-1,0.2,3),(1,1.5,-1),(-1,1.5,-1)]",
    'uniform token subdivisionScheme = "catmullClark"']))
case("mesh_subdiv_bad_creases", mesh("m", [
    "int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]", "point3f[] points = %s" % QUAD_PTS,
    'uniform token subdivisionScheme = "catmullClark"', "int[] creaseIndices = [0, 99, -1, 2]",
    "int[] creaseLengths = [2, 5]", "float[] creaseSharpnesses = [nan, 1e30, 3]",
    "int[] cornerIndices = [77, -2]", "float[] cornerSharpnesses = [10]", "int[] holeIndices = [5, -1]"]))
case("mesh_subdiv_degenerate", mesh("m", [
    "int[] faceVertexCounts = [3, 4, 2, 1]", "int[] faceVertexIndices = [0, 0, 1, 0, 1, 2, 3, 0, 1, 2]",
    "point3f[] points = %s" % QUAD_PTS, 'uniform token subdivisionScheme = "catmullClark"']))
case("mesh_subdiv_nan", mesh("m", [
    "int[] faceVertexCounts = [4, 4]", "int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 5, 2]",
    "point3f[] points = [(-1,0.2,1),(1,0.2,1),(1,2.2,1),(-1,2.2,1),(nan,0.2,1),(3,inf,1)]",
    'uniform token subdivisionScheme = "catmullClark"']))
case("mesh_subdiv_loop_quads", mesh("m", [
    "int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]", "point3f[] points = %s" % QUAD_PTS,
    'uniform token subdivisionScheme = "loop"']))
case("mesh_subdiv_unknown", mesh("m", [
    "int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]", "point3f[] points = %s" % QUAD_PTS,
    'uniform token subdivisionScheme = "bogus"', 'uniform token orientation = "sideways"']))
case("mesh_subset_oob", mesh("m", [
    "int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]", "point3f[] points = %s" % QUAD_PTS,
    'def GeomSubset "s"\n        {\n            uniform token elementType = "face"\n            uniform token familyName = "materialBind"\n            int[] indices = [0, 5, -3, 2147483647]\n            rel material:binding = </World/mat>\n        }']) +
     '\n    def Material "mat"\n    {\n        token outputs:surface.connect = </World/mat/s.outputs:surface>\n        def Shader "s"\n        {\n            uniform token info:id = "UsdPreviewSurface"\n            color3f inputs:diffuseColor = (1, 0, 0)\n            token outputs:surface\n        }\n    }\n')
case("mesh_xform_zero", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                   "point3f[] points = %s" % QUAD_PTS, "float3 xformOp:scale = (0, 0, 0)",
                                   'uniform token[] xformOpOrder = ["xformOp:scale"]']))
case("mesh_xform_nan", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                  "point3f[] points = %s" % QUAD_PTS, "double3 xformOp:translate = (nan, 0, inf)",
                                  'uniform token[] xformOpOrder = ["xformOp:translate"]']))
case("mesh_xform_huge", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
                                   "point3f[] points = %s" % QUAD_PTS, "float3 xformOp:scale = (1e30, 1e30, 1e30)",
                                   'uniform token[] xformOpOrder = ["xformOp:scale"]']))
case("mesh_xform_flat", BALL % "        float3 xformOp:scale = (1, 0, 1)\n        uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:scale\"]")
case("sphere_radius_zero", '\n    def Sphere "s"\n    {\n        double radius = 0\n    }\n    def Sphere "t"\n    {\n        double radius = -2\n    }\n    def Sphere "u"\n    {\n        double radius = nan\n    }\n')
case("implicit_weird", '\n    def Cube "c"\n    {\n        double size = 0\n    }\n    def Cylinder "cy"\n    {\n        double height = 0\n        double radius = -1\n    }\n    def Cone "co"\n    {\n        double height = inf\n        double radius = 1\n    }\n    def Capsule "ca"\n    {\n        double height = -1\n        double radius = 0\n    }\n    def Plane "pl"\n    {\n        double width = 0\n        double length = nan\n    }\n')
case("mesh_motion_topology", mesh("m", [
    "int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]",
    "point3f[] points.timeSamples = {\n            0: %s,\n            1: [(-1, 0.2, 1), (1, 0.2, 1)],\n            2: [],\n        }" % QUAD_PTS]),
     layer="    startTimeCode = 1\n    endTimeCode = 1", extra=["--frame", "1"])
case("mesh_velocity_bad", mesh("m", [
    "int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]", "point3f[] points = %s" % QUAD_PTS,
    "vector3f[] velocities = [(nan, 0, 0), (inf, 1e30, 0)]", "vector3f[] accelerations = [(1, 2, 3)]"]))

# ---------------------------------------------------------------- curves
def curves(attrs):
    return mesh("c", attrs, "BasisCurves")


case("curves_empty", curves(['uniform token type = "linear"', "int[] curveVertexCounts = []", "point3f[] points = []"]))
case("curves_no_attrs", curves([]))
case("curves_counts_exceed", curves(['uniform token type = "linear"', "int[] curveVertexCounts = [5, 7]",
                                     "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0)]", "float[] widths = [0.1]"]))
case("curves_single_point", curves(['uniform token type = "linear"', "int[] curveVertexCounts = [1, 0, 2, -3]",
                                    "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0)]", "float[] widths = [0.1]"]))
case("curves_no_widths", curves(['uniform token type = "linear"', "int[] curveVertexCounts = [3]",
                                 "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0)]"]))
case("curves_widths_bad", curves(['uniform token type = "linear"', "int[] curveVertexCounts = [3]",
                                  "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0)]",
                                  'float[] widths = [nan, -1] (\n            interpolation = "vertex"\n        )']))
case("curves_widths_inf", curves(['uniform token type = "linear"', "int[] curveVertexCounts = [3]",
                                  "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0)]", "float[] widths = [inf]"]))
case("curves_widths_zero", curves(['uniform token type = "linear"', "int[] curveVertexCounts = [3]",
                                   "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0)]", "float[] widths = [0]"]))
case("curves_nan_points", curves(['uniform token type = "cubic"', 'uniform token basis = "bspline"',
                                  "int[] curveVertexCounts = [4]",
                                  "point3f[] points = [(0,0.5,0),(nan,1,0),(0,inf,0),(0,2,0)]", "float[] widths = [0.1]"]))
case("curves_coincident", curves(['uniform token type = "cubic"', 'uniform token basis = "bspline"',
                                  "int[] curveVertexCounts = [4, 4]",
                                  "point3f[] points = [(0,1,0),(0,1,0),(0,1,0),(0,1,0),(1,1,0),(1,1,0),(1,2,0),(1,2,0)]",
                                  "float[] widths = [0.1]"]))
case("curves_cubic_short", curves(['uniform token type = "cubic"', 'uniform token basis = "bspline"',
                                   "int[] curveVertexCounts = [2, 3, 1]",
                                   "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0),(1,0.5,0),(1,1,0),(1,2,0)]",
                                   "float[] widths = [0.1]"]))
case("curves_bezier_bad_count", curves(['uniform token type = "cubic"', 'uniform token basis = "bezier"',
                                        "int[] curveVertexCounts = [5, 6]",
                                        "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0),(1,0.5,0),(1,1,0),(1,2,0),(2,0,0),(2,1,0),(2,2,0),(3,0,0),(3,1,0)]",
                                        "float[] widths = [0.1]"]))
case("curves_periodic", curves(['uniform token type = "cubic"', 'uniform token basis = "bspline"', 'uniform token wrap = "periodic"',
                                "int[] curveVertexCounts = [3, 2, 4]",
                                "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0),(1,0.5,0),(1,1,0),(1,2,0),(2,0,0),(2,1,0),(2,2,0)]",
                                "float[] widths = [0.1]"]))
case("curves_pinned", curves(['uniform token type = "cubic"', 'uniform token basis = "catmullRom"', 'uniform token wrap = "pinned"',
                              "int[] curveVertexCounts = [2, 3, 1]",
                              "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0),(1,0.5,0),(1,1,0),(1,2,0)]",
                              "float[] widths = [0.1]"]))
case("curves_unknown_basis", curves(['uniform token type = "cubic"', 'uniform token basis = "hermite"',
                                     "int[] curveVertexCounts = [4]",
                                     "point3f[] points = [(0,0.5,0),(0.3,1,0),(0,1.5,0),(0,2,0)]", "float[] widths = [0.1]"]))
case("curves_normals_bad", curves(['uniform token type = "linear"', "int[] curveVertexCounts = [3]",
                                   "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0)]", "float[] widths = [0.3]",
                                   "normal3f[] normals = [(0, 0, 0)]"]))
case("curves_varying_bad", curves(['uniform token type = "cubic"', 'uniform token basis = "bspline"',
                                   "int[] curveVertexCounts = [6]",
                                   "point3f[] points = [(0,0.5,0),(0.2,1,0),(0,1.3,0),(0.3,1.6,0),(0,1.9,0),(0,2.2,0)]",
                                   'float[] widths = [0.1, 0.2] (\n            interpolation = "varying"\n        )',
                                   'color3f[] primvars:displayColor = [(1,0,0)] (\n            interpolation = "varying"\n        )']))
case("curves_huge_width", curves(['uniform token type = "linear"', "int[] curveVertexCounts = [3]",
                                  "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0)]", "float[] widths = [1e30]"]))

# ---------------------------------------------------------------- points
def points(attrs):
    return mesh("p", attrs, "Points")


case("points_empty", points(["point3f[] points = []"]))
case("points_no_widths", points(["point3f[] points = [(0,1,0),(1,1,0),(-1,1,0)]"]))
case("points_widths_bad", points(["point3f[] points = [(0,1,0),(1,1,0),(-1,1,0)]", "float[] widths = [nan, -0.5, inf]"]))
case("points_widths_short", points(["point3f[] points = [(0,1,0),(1,1,0),(-1,1,0)]",
                                    'float[] widths = [0.5, 0.2] (\n            interpolation = "vertex"\n        )']))
case("points_nan", points(["point3f[] points = [(nan,1,0),(1,inf,0),(-1,1,-inf)]", "float[] widths = [0.5]"]))
case("points_huge", points(["point3f[] points = [(0,1,0)]", "float[] widths = [1e30]"]))
case("points_zero_width", points(["point3f[] points = [(0,1,0),(1,1,0)]", "float[] widths = [0]"]))
case("points_ids_short", points(["point3f[] points = [(0,1,0),(1,1,0),(-1,1,0)]", "float[] widths = [0.5]", "int64[] ids = [7]"]))

# ---------------------------------------------------------------- lights
def light(kind, attrs, name="l"):
    return mesh(name, attrs, kind)


POSE = ["double3 xformOp:translate = (2, 3, 2)", 'uniform token[] xformOpOrder = ["xformOp:translate"]']
case("light_rect_zero", BALL % "" + light("RectLight", ["float inputs:width = 0", "float inputs:height = 0", "float inputs:intensity = 50"] + POSE))
case("light_rect_negative", BALL % "" + light("RectLight", ["float inputs:width = -2", "float inputs:height = -1", "float inputs:intensity = 50"] + POSE))
case("light_rect_nan", BALL % "" + light("RectLight", ["float inputs:width = nan", "float inputs:height = inf", "float inputs:intensity = 50"] + POSE))
case("light_sphere_zero", BALL % "" + light("SphereLight", ["float inputs:radius = 0", "float inputs:intensity = 50"] + POSE))
case("light_sphere_zero_norm", BALL % "" + light("SphereLight", ["float inputs:radius = 0", "bool inputs:normalize = 1", "float inputs:intensity = 50"] + POSE))
case("light_sphere_negative", BALL % "" + light("SphereLight", ["float inputs:radius = -1", "float inputs:intensity = 50"] + POSE))
case("light_sphere_point", BALL % "" + light("SphereLight", ["bool treatAsPoint = 1", "float inputs:radius = 0.5", "float inputs:intensity = 50"] + POSE))
case("light_disk_zero", BALL % "" + light("DiskLight", ["float inputs:radius = 0", "bool inputs:normalize = 1", "float inputs:intensity = 50"] + POSE))
case("light_cyl_zero", BALL % "" + light("CylinderLight", ["float inputs:radius = 0", "float inputs:length = 0", "float inputs:intensity = 50"] + POSE))
case("light_cyl_zero_radius", BALL % "" + light("CylinderLight", ["float inputs:radius = 0", "float inputs:length = 2", "bool inputs:normalize = 1", "float inputs:intensity = 50"] + POSE))
case("light_cyl_line", BALL % "" + light("CylinderLight", ["float inputs:radius = 0.1", "float inputs:length = 2", "bool treatAsLine = 1", "float inputs:intensity = 50"] + POSE))
case("light_intensity_nan", BALL % "" + light("SphereLight", ["float inputs:radius = 0.5", "float inputs:intensity = nan"] + POSE))
case("light_intensity_inf", BALL % "" + light("SphereLight", ["float inputs:radius = 0.5", "float inputs:intensity = inf"] + POSE))
case("light_exposure_huge", BALL % "" + light("SphereLight", ["float inputs:radius = 0.5", "float inputs:exposure = 200"] + POSE))
case("light_exposure_neg_huge", BALL % "" + light("SphereLight", ["float inputs:radius = 0.5", "float inputs:exposure = -2000"] + POSE))
case("light_color_negative", BALL % "" + light("SphereLight", ["float inputs:radius = 0.5", "color3f inputs:color = (-1, nan, inf)"] + POSE))
case("light_temperature_bad", BALL % "" + light("SphereLight", ["float inputs:radius = 0.5", "bool inputs:enableColorTemperature = 1", "float inputs:colorTemperature = 0"] + POSE) +
     light("SphereLight", ["float inputs:radius = 0.5", "bool inputs:enableColorTemperature = 1", "float inputs:colorTemperature = -5"] + POSE, "l2") +
     light("SphereLight", ["float inputs:radius = 0.5", "bool inputs:enableColorTemperature = 1", "float inputs:colorTemperature = 1e30"] + POSE, "l3") +
     light("SphereLight", ["float inputs:radius = 0.5", "bool inputs:enableColorTemperature = 1", "float inputs:colorTemperature = nan"] + POSE, "l4"))
case("light_spot_zero", BALL % "" + light("SphereLight", ["float inputs:radius = 0.2", "float inputs:intensity = 50", "float inputs:shaping:cone:angle = 0", "float inputs:shaping:cone:softness = 0"] + POSE))
case("light_spot_negative", BALL % "" + light("SphereLight", ["float inputs:radius = 0.2", "float inputs:intensity = 50", "float inputs:shaping:cone:angle = -30", "float inputs:shaping:cone:softness = -2"] + POSE))
case("light_spot_wide", BALL % "" + light("SphereLight", ["float inputs:radius = 0.2", "float inputs:intensity = 50", "float inputs:shaping:cone:angle = 400", "float inputs:shaping:cone:softness = 50", "float inputs:shaping:focus = -3"] + POSE))
case("light_spot_nan", BALL % "" + light("SphereLight", ["float inputs:radius = 0.2", "float inputs:intensity = 50", "float inputs:shaping:cone:angle = nan", "float inputs:shaping:cone:softness = nan", "float inputs:shaping:focus = inf"] + POSE))
case("light_spot_focus_huge", BALL % "" + light("RectLight", ["float inputs:intensity = 50", "float inputs:shaping:focus = 1e30", "color3f inputs:shaping:focusTint = (nan, -1, 2)"] + POSE))
case("light_ies_missing", BALL % "" + light("SphereLight", ["float inputs:radius = 0.2", "float inputs:intensity = 50", "asset inputs:shaping:ies:file = @nope.ies@"] + POSE))
case("light_distant_zero", BALL % "" + light("DistantLight", ["float inputs:angle = 0", "float inputs:intensity = 2"]))
case("light_distant_negative", BALL % "" + light("DistantLight", ["float inputs:angle = -5", "float inputs:intensity = 2", "bool inputs:normalize = 1"]))
case("light_distant_360", BALL % "" + light("DistantLight", ["float inputs:angle = 360", "float inputs:intensity = 2", "bool inputs:normalize = 1"]))
case("light_distant_nan", BALL % "" + light("DistantLight", ["float inputs:angle = nan", "float inputs:intensity = 2", "bool inputs:normalize = 1"]))
case("light_distant_tiny_norm", BALL % "" + light("DistantLight", ["float inputs:angle = 1e-20", "float inputs:intensity = 2", "bool inputs:normalize = 1"]))
case("light_xform_zero", BALL % "" + light("RectLight", ["float inputs:intensity = 50", "float3 xformOp:scale = (0, 0, 0)", 'uniform token[] xformOpOrder = ["xformOp:scale"]']))
case("light_xform_flat", BALL % "" + light("SphereLight", ["float inputs:intensity = 50", "float inputs:radius = 0.5", "double3 xformOp:translate = (2, 3, 2)", "float3 xformOp:scale = (1, 0, 1)", 'uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]']))
case("light_xform_nan", BALL % "" + light("RectLight", ["float inputs:intensity = 50", "double3 xformOp:translate = (nan, 3, 2)", 'uniform token[] xformOpOrder = ["xformOp:translate"]']))
case("light_xform_shear", BALL % "" + light("CylinderLight", ["float inputs:intensity = 50", "float inputs:length = 2", "float inputs:radius = 0.2",
                                                            "matrix4d xformOp:transform = ((1, 2, 0, 0), (0, 1e-8, 3, 0), (4, 0, 1, 0), (2, 3, 2, 1))", 'uniform token[] xformOpOrder = ["xformOp:transform"]']))
case("light_xform_mirror", BALL % "" + light("RectLight", ["float inputs:intensity = 50", "float3 xformOp:scale = (-1, 1, 1)", "double3 xformOp:translate = (2, 3, 2)", 'uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]']))
case("light_none", BALL % "", lights="")
case("light_only_dome_zero", BALL % "", lights='\n    def DomeLight "sky"\n    {\n        float inputs:intensity = 0\n    }\n')
case("light_dome_missing_tex", BALL % "", lights='\n    def DomeLight "sky"\n    {\n        asset inputs:texture:file = @does_not_exist.exr@\n    }\n')
case("light_dome_corrupt_tex", BALL % "", lights='\n    def DomeLight "sky"\n    {\n        asset inputs:texture:file = @corrupt.exr@\n    }\n')
case("light_dome_tiny_tex", BALL % "", lights='\n    def DomeLight "sky"\n    {\n        asset inputs:texture:file = @tiny.exr@\n    }\n')
case("light_dome_black_tex", BALL % "", lights='\n    def DomeLight "sky"\n    {\n        asset inputs:texture:file = @black.exr@\n    }\n')
case("light_dome_nan_tex", BALL % "", lights='\n    def DomeLight "sky"\n    {\n        asset inputs:texture:file = @nan.exr@\n        bool inputs:karma:light:renderlightgeo = 1\n    }\n')
case("light_dome_neg_tex", BALL % "", lights='\n    def DomeLight "sky"\n    {\n        asset inputs:texture:file = @neg.exr@\n        bool inputs:karma:light:renderlightgeo = 1\n    }\n')
case("light_dome_format", BALL % "", lights='\n    def DomeLight "sky"\n    {\n        asset inputs:texture:file = @tiny.exr@\n        token inputs:texture:format = "cubeMapVerticalCross"\n    }\n')
case("light_rect_tex_missing", BALL % "" + light("RectLight", ["float inputs:intensity = 50", "asset inputs:texture:file = @does_not_exist.exr@"] + POSE))
case("light_rect_tex_nan", BALL % "" + light("RectLight", ["float inputs:intensity = 50", "asset inputs:texture:file = @nan.exr@", "bool inputs:karma:light:renderlightgeo = 1"] + POSE))
case("light_many_domes", BALL % "", lights="".join('\n    def DomeLight "sky%d"\n    {\n        float inputs:intensity = 0.1\n    }\n' % i for i in range(40)))
case("light_portal_zero", BALL % "" + '\n    def DomeLight "sky2"\n    {\n        rel light:portals = </World/sky2/portal>\n        def PortalLight "portal"\n        {\n            float inputs:width = 0\n            float inputs:height = 0\n        }\n    }\n', lights="")
case("light_portal_missing", BALL % "" + '\n    def DomeLight "sky2"\n    {\n        rel light:portals = </World/nothing>\n    }\n', lights="")
case("light_filter_missing", BALL % "" + light("RectLight", ["float inputs:intensity = 50", "rel light:filters = </World/nothing>"] + POSE))
case("light_link_weird", BALL % "" + light("RectLight", ["float inputs:intensity = 50", 'uniform token collection:lightLink:expansionRule = "bogus"', "rel collection:lightLink:includes = </World/nothing>", "rel collection:shadowLink:excludes = [</World>, </World/ball>]", "uniform bool collection:lightLink:includeRoot = 0"] + POSE))
case("mesh_light_degenerate", mesh("e", ["int[] faceVertexCounts = [3, 3]", "int[] faceVertexIndices = [0, 0, 0, 0, 1, 2]",
                                         "point3f[] points = [(0,3,0),(1e-30,3,0),(0,3,1e-30)]",
                                         "rel material:binding = </World/emat>"]) +
     '\n    def Material "emat"\n    {\n        token outputs:surface.connect = </World/emat/s.outputs:surface>\n        def Shader "s"\n        {\n            uniform token info:id = "UsdPreviewSurface"\n            color3f inputs:emissiveColor = (1e30, 5, 5)\n            token outputs:surface\n        }\n    }\n' + BALL % "", lights="")

# ---------------------------------------------------------------- camera
def cam(attrs):
    return mesh("cam", attrs, "Camera")


CPOSE = ["double3 xformOp:translate = (0, 1.5, 7)", 'uniform token[] xformOpOrder = ["xformOp:translate"]']
case("cam_none", BALL % "", camera="")
case("cam_focal_zero", BALL % "", camera=cam(["float focalLength = 0"] + CPOSE))
case("cam_focal_negative", BALL % "", camera=cam(["float focalLength = -35"] + CPOSE))
case("cam_focal_nan", BALL % "", camera=cam(["float focalLength = nan"] + CPOSE))
case("cam_aperture_zero", BALL % "", camera=cam(["float horizontalAperture = 0", "float verticalAperture = 0"] + CPOSE))
case("cam_aperture_negative", BALL % "", camera=cam(["float horizontalAperture = -36", "float verticalAperture = -24"] + CPOSE))
case("cam_aperture_inf", BALL % "", camera=cam(["float horizontalAperture = inf", "float verticalAperture = 24"] + CPOSE))
case("cam_clip_zero", BALL % "", camera=cam(["float2 clippingRange = (0, 0)"] + CPOSE))
case("cam_clip_inverted", BALL % "", camera=cam(["float2 clippingRange = (100, 1)"] + CPOSE))
case("cam_clip_negative", BALL % "", camera=cam(["float2 clippingRange = (-5, -1)"] + CPOSE))
case("cam_clip_nan", BALL % "", camera=cam(["float2 clippingRange = (nan, inf)"] + CPOSE))
case("cam_dof_zero_focus", BALL % "", camera=cam(["float fStop = 2", "float focusDistance = 0"] + CPOSE))
case("cam_dof_tiny_fstop", BALL % "", camera=cam(["float fStop = 1e-30", "float focusDistance = 7"] + CPOSE))
case("cam_dof_negative", BALL % "", camera=cam(["float fStop = -2", "float focusDistance = -7"] + CPOSE))
case("cam_dof_nan", BALL % "", camera=cam(["float fStop = nan", "float focusDistance = inf"] + CPOSE))
case("cam_ortho", BALL % "", camera=cam(['token projection = "orthographic"', "float horizontalAperture = 80", "float verticalAperture = 60"] + CPOSE))
case("cam_ortho_zero", BALL % "", camera=cam(['token projection = "orthographic"', "float horizontalAperture = 0", "float verticalAperture = 0"] + CPOSE))
case("cam_projection_bogus", BALL % "", camera=cam(['token projection = "fisheye"'] + CPOSE))
case("cam_shutter_inverted", BALL % "", camera=cam(["double shutter:open = 0.5", "double shutter:close = -0.5"] + CPOSE))
case("cam_shutter_nan", BALL % "", camera=cam(["double shutter:open = nan", "double shutter:close = inf"] + CPOSE))
case("cam_shutter_huge", BALL % "", camera=cam(["double shutter:open = -1e30", "double shutter:close = 1e30"] + CPOSE))
case("cam_xform_zero", BALL % "", camera=cam(["float3 xformOp:scale = (0, 0, 0)", 'uniform token[] xformOpOrder = ["xformOp:scale"]']))
case("cam_xform_nan", BALL % "", camera=cam(["double3 xformOp:translate = (nan, 1, 7)", 'uniform token[] xformOpOrder = ["xformOp:translate"]']))
case("cam_xform_scaled", BALL % "", camera=cam(["double3 xformOp:translate = (0, 1.5, 7)", "float3 xformOp:scale = (3, 0.2, -5)", 'uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]']))
case("cam_offset_huge", BALL % "", camera=cam(["float horizontalApertureOffset = 1e30", "float verticalApertureOffset = nan"] + CPOSE))
case("cam_inside_sphere", BALL % "", camera=cam(["double3 xformOp:translate = (0, 1, 0)", 'uniform token[] xformOpOrder = ["xformOp:translate"]']))
case("cam_on_ground", BALL % "", camera=cam(["double3 xformOp:translate = (0, 0, 7)", 'uniform token[] xformOpOrder = ["xformOp:translate"]']))
case("cam_res_1x1", BALL % "", extra=["--res", "1x1"])
case("cam_res_wide", BALL % "", extra=["--res", "2000x1"])
case("cam_res_tall", BALL % "", extra=["--res", "1x2000"])
case("cam_res_zero", BALL % "", extra=["--res", "0x0"])
case("cam_res_negative", BALL % "", extra=["--res", "-5x-5"])
case("cam_lens_props", BALL % "", camera=cam(["float karma:camera:lensdistortion = nan", "float karma:camera:aperturecount = -3", "float karma:camera:chromaticaberration = 1e30", "float fStop = 2", "float focusDistance = 7"] + CPOSE))

# ---------------------------------------------------------------- instancers
PROTO = '\n        def Sphere "proto"\n        {\n            double radius = 0.3\n        }\n'


def inst(attrs, protos=PROTO, rel="</World/pi/proto>"):
    return '\n    def PointInstancer "pi"\n    {\n        rel prototypes = %s\n%s\n%s    }\n' % (rel, "\n".join("        " + a for a in attrs), protos)


case("inst_empty", inst(["point3f[] positions = []", "int[] protoIndices = []"]))
case("inst_no_attrs", inst([]))
case("inst_proto_oob", inst(["point3f[] positions = [(0,1,0),(1,1,0),(-1,1,0)]", "int[] protoIndices = [0, 5, -1]"]))
case("inst_proto_short", inst(["point3f[] positions = [(0,1,0),(1,1,0),(-1,1,0)]", "int[] protoIndices = [0]"]))
case("inst_proto_long", inst(["point3f[] positions = [(0,1,0)]", "int[] protoIndices = [0, 0, 0, 0]"]))
case("inst_orient_short", inst(["point3f[] positions = [(0,1,0),(1,1,0),(-1,1,0)]", "int[] protoIndices = [0, 0, 0]",
                                "quath[] orientations = [(1, 0, 0, 0)]", "float3[] scales = [(1, 1, 1)]"]))
case("inst_orient_zero", inst(["point3f[] positions = [(0,1,0),(1,1,0),(-1,1,0)]", "int[] protoIndices = [0, 0, 0]",
                               "quath[] orientations = [(0, 0, 0, 0), (nan, 0, 0, 0), (inf, 1, 0, 0)]"]))
case("inst_scale_zero", inst(["point3f[] positions = [(0,1,0),(1,1,0),(-1,1,0)]", "int[] protoIndices = [0, 0, 0]",
                              "float3[] scales = [(0, 0, 0), (nan, 1, 1), (-1, -1, -1)]"]))
case("inst_pos_nan", inst(["point3f[] positions = [(nan,1,0),(1,inf,0),(-1,1,0)]", "int[] protoIndices = [0, 0, 0]"]))
case("inst_pos_huge", inst(["point3f[] positions = [(1e30,1,0),(1,1e38,0),(-1,1,0)]", "int[] protoIndices = [0, 0, 0]"]))
case("inst_invisible_bad", inst(["point3f[] positions = [(0,1,0),(1,1,0),(-1,1,0)]", "int[] protoIndices = [0, 0, 0]",
                                 "int64[] invisibleIds = [0, 5, -3]", "int64[] ids = [0, 0]"]))
case("inst_no_protos", inst(["point3f[] positions = [(0,1,0)]", "int[] protoIndices = [0]"], protos="", rel="</World/nothing>"))
case("inst_proto_is_self", inst(["point3f[] positions = [(0,1,0),(1,1,0)]", "int[] protoIndices = [0, 0]"], rel="</World/pi>"))
case("inst_proto_is_world", inst(["point3f[] positions = [(0,1,0),(1,1,0)]", "int[] protoIndices = [0, 0]"], rel="</World>"))
case("inst_velocity_bad", inst(["point3f[] positions = [(0,1,0),(1,1,0),(-1,1,0)]", "int[] protoIndices = [0, 0, 0]",
                                "vector3f[] velocities = [(nan, 0, 0)]", "vector3f[] angularVelocities = [(inf, 0, 0), (1e30, 0, 0), (0, 0, 0)]",
                                "vector3f[] accelerations = [(1e30, 0, 0), (0, 0, 0), (0, 0, 0)]"]))
case("inst_nested", '\n    def PointInstancer "pi"\n    {\n        rel prototypes = </World/pi/inner>\n        point3f[] positions = [(0,0.5,0),(0,1.5,0)]\n        int[] protoIndices = [0, 0]\n        def PointInstancer "inner"\n        {\n            rel prototypes = </World/pi/inner/proto>\n            point3f[] positions = [(-1,0,0),(1,0,0)]\n            int[] protoIndices = [0, 7]\n            def Sphere "proto"\n            {\n                double radius = 0.3\n            }\n        }\n    }\n')
case("inst_primvar_short", inst(["point3f[] positions = [(0,1,0),(1,1,0),(-1,1,0)]", "int[] protoIndices = [0, 0, 0]",
                                 'color3f[] primvars:displayColor = [(1, 0, 0)] (\n            interpolation = "vertex"\n        )']))
case("inst_light_proto", inst(["point3f[] positions = [(0,3,0),(1,3,0)]", "int[] protoIndices = [0, 0]"],
                              protos='\n        def SphereLight "proto"\n        {\n            float inputs:radius = 0.1\n            float inputs:intensity = 20\n        }\n') + BALL % "")
case("inst_many_degenerate", inst(["point3f[] positions = [%s]" % ",".join("(0,1,0)" for _ in range(2000)),
                                   "int[] protoIndices = [%s]" % ",".join("0" for _ in range(2000))]))

# ---------------------------------------------------------------- materials
def material(shader_lines, extra_shaders="", outputs='token outputs:surface.connect = </World/mat/s.outputs:surface>'):
    return ('\n    def Material "mat"\n    {\n        %s\n        def Shader "s"\n        {\n%s\n        }\n%s    }\n'
            % (outputs, "\n".join("            " + l for l in shader_lines), extra_shaders))


BOUND = BALL % "        rel material:binding = </World/mat>"
SS = ['uniform token info:id = "ND_standard_surface_surfaceshader"', "token outputs:surface"]
MTLX = 'token outputs:mtlx:surface.connect = </World/mat/s.outputs:surface>'


def ss_case(name, inputs):
    case(name, BOUND + material(SS + inputs, outputs=MTLX))


ss_case("mat_ss_default", [])
ss_case("mat_ss_nan", ["color3f inputs:base_color = (nan, 0.5, 0.5)", "float inputs:specular_roughness = nan", "float inputs:metalness = nan"])
ss_case("mat_ss_inf", ["color3f inputs:base_color = (inf, 0.5, 0.5)", "float inputs:base = inf", "float inputs:specular = inf"])
ss_case("mat_ss_negative", ["color3f inputs:base_color = (-1, -1, -1)", "float inputs:specular_roughness = -1", "float inputs:metalness = -3", "float inputs:specular = -1", "float inputs:base = -2"])
ss_case("mat_ss_over", ["color3f inputs:base_color = (50, 50, 50)", "float inputs:specular_roughness = 9", "float inputs:metalness = 7", "float inputs:specular = 30", "float inputs:coat = 5", "float inputs:sheen = 5"])
ss_case("mat_ss_ior_zero", ["float inputs:specular_IOR = 0", "float inputs:transmission = 1"])
ss_case("mat_ss_ior_one", ["float inputs:specular_IOR = 1", "float inputs:transmission = 1", "float inputs:specular_roughness = 0"])
ss_case("mat_ss_ior_negative", ["float inputs:specular_IOR = -1.5", "float inputs:transmission = 1"])
ss_case("mat_ss_ior_huge", ["float inputs:specular_IOR = 1e30", "float inputs:transmission = 1", "float inputs:coat = 1", "float inputs:coat_IOR = 1e30"])
ss_case("mat_ss_ior_nan", ["float inputs:specular_IOR = nan", "float inputs:transmission = 1", "float inputs:coat = 1", "float inputs:coat_IOR = nan"])
ss_case("mat_ss_rough_zero", ["float inputs:specular_roughness = 0", "float inputs:metalness = 1", "float inputs:coat = 1", "float inputs:coat_roughness = 0"])
ss_case("mat_ss_aniso_bad", ["float inputs:specular_roughness = 0.3", "float inputs:metalness = 1", "float inputs:specular_anisotropy = 5", "float inputs:specular_rotation = nan"])
ss_case("mat_ss_aniso_neg", ["float inputs:specular_roughness = 0.3", "float inputs:metalness = 1", "float inputs:specular_anisotropy = -5", "float inputs:specular_rotation = 1e30"])
ss_case("mat_ss_transmission_depth", ["float inputs:transmission = 1", "color3f inputs:transmission_color = (0, 0.5, 2)", "float inputs:transmission_depth = -1", "color3f inputs:transmission_scatter = (nan, 1, 1)", "float inputs:transmission_dispersion = 50"])
ss_case("mat_ss_transmission_depth_tiny", ["float inputs:transmission = 1", "color3f inputs:transmission_color = (0.1, 0.5, 0.9)", "float inputs:transmission_depth = 1e-30", "color3f inputs:transmission_scatter = (1, 1, 1)", "float inputs:transmission_scatter_anisotropy = 5"])
ss_case("mat_ss_sss_bad", ["float inputs:subsurface = 1", "color3f inputs:subsurface_color = (2, -1, nan)", "color3f inputs:subsurface_radius = (0, -1, inf)", "float inputs:subsurface_scale = nan", "float inputs:subsurface_anisotropy = 3"])
ss_case("mat_ss_sss_zero_radius", ["float inputs:subsurface = 1", "color3f inputs:subsurface_radius = (0, 0, 0)", "float inputs:subsurface_scale = 0"])
ss_case("mat_ss_sss_huge_radius", ["float inputs:subsurface = 1", "color3f inputs:subsurface_radius = (1e30, 1e30, 1e30)", "float inputs:subsurface_scale = 1e30"])
ss_case("mat_ss_sss_tiny_radius", ["float inputs:subsurface = 1", "color3f inputs:subsurface_color = (1, 1, 1)", "color3f inputs:subsurface_radius = (1e-30, 1e-30, 1e-30)", "float inputs:subsurface_scale = 1"])
ss_case("mat_ss_thinfilm_bad", ["float inputs:thin_film_thickness = -500", "float inputs:thin_film_IOR = 0", "float inputs:metalness = 1"])
ss_case("mat_ss_thinfilm_nan", ["float inputs:thin_film_thickness = nan", "float inputs:thin_film_IOR = inf"])
ss_case("mat_ss_thinfilm_huge", ["float inputs:thin_film_thickness = 1e30", "float inputs:thin_film_IOR = 1e30", "float inputs:transmission = 1"])
ss_case("mat_ss_emission_inf", ["float inputs:emission = inf", "color3f inputs:emission_color = (1, 1, 1)"])
ss_case("mat_ss_emission_nan", ["float inputs:emission = nan", "color3f inputs:emission_color = (nan, -1, 1)"])
ss_case("mat_ss_emission_huge", ["float inputs:emission = 1e30", "color3f inputs:emission_color = (1, 1, 1)"])
ss_case("mat_ss_emission_negative", ["float inputs:emission = -5", "color3f inputs:emission_color = (1, 1, 1)"])
ss_case("mat_ss_opacity_bad", ["color3f inputs:opacity = (nan, -1, 5)"])
ss_case("mat_ss_opacity_zero", ["color3f inputs:opacity = (0, 0, 0)"])
ss_case("mat_ss_opacity_half_thin", ["color3f inputs:opacity = (0.5, 0.2, 0.9)", "bool inputs:thin_walled = 1", "float inputs:transmission = 0.5"])
ss_case("mat_ss_normal_zero", ["vector3f inputs:normal = (0, 0, 0)", "vector3f inputs:tangent = (0, 0, 0)", "vector3f inputs:coat_normal = (nan, 0, 0)", "float inputs:coat = 1"])
ss_case("mat_ss_normal_inf", ["vector3f inputs:normal = (inf, 1e30, -inf)", "vector3f inputs:tangent = (1e30, 1e30, 1e30)", "float inputs:specular_anisotropy = 0.5"])
ss_case("mat_ss_sheen_bad", ["float inputs:sheen = 1", "float inputs:sheen_roughness = 0", "color3f inputs:sheen_color = (nan, 5, -1)"])
ss_case("mat_ss_sheen_rough_neg", ["float inputs:sheen = 1", "float inputs:sheen_roughness = -4"])
ss_case("mat_ss_coat_bad", ["float inputs:coat = 1", "float inputs:coat_roughness = nan", "color3f inputs:coat_color = (-1, 5, nan)", "float inputs:coat_affect_color = 50", "float inputs:coat_affect_roughness = -3", "float inputs:coat_anisotropy = 9"])
ss_case("mat_ss_diffuse_rough", ["float inputs:diffuse_roughness = 50"])
ss_case("mat_ss_diffuse_rough_neg", ["float inputs:diffuse_roughness = -50"])
ss_case("mat_ss_wrong_types", ["float inputs:base_color = 0.5", "color3f inputs:specular_roughness = (0.1, 0.2, 0.3)", "string inputs:metalness = \"lots\"", "int inputs:transmission = 1", "bool inputs:coat = 1"])

OPENPBR = ['uniform token info:id = "ND_open_pbr_surface_surfaceshader"', "token outputs:surface"]


def op_case(name, inputs):
    case(name, BOUND + material(OPENPBR + inputs, outputs=MTLX))


op_case("mat_openpbr_default", [])
op_case("mat_openpbr_nan", ["color3f inputs:base_color = (nan, inf, -1)", "float inputs:specular_roughness = nan", "float inputs:base_metalness = nan", "float inputs:specular_ior = nan"])
op_case("mat_openpbr_extreme", ["float inputs:specular_ior = 0", "float inputs:transmission_weight = 1", "float inputs:coat_weight = 5", "float inputs:coat_ior = 0", "float inputs:fuzz_weight = 9", "float inputs:fuzz_roughness = 0", "float inputs:thin_film_weight = 1", "float inputs:thin_film_thickness = -1", "float inputs:thin_film_ior = 0"])
op_case("mat_openpbr_sss", ["float inputs:subsurface_weight = 1", "float inputs:subsurface_radius = 0", "color3f inputs:subsurface_radius_scale = (0, 0, 0)", "float inputs:subsurface_scatter_anisotropy = -5"])
op_case("mat_openpbr_transmission", ["float inputs:transmission_weight = 1", "float inputs:transmission_depth = 0", "color3f inputs:transmission_color = (0, 0, 0)", "color3f inputs:transmission_scatter = (5, 5, 5)", "float inputs:transmission_dispersion_scale = 5", "float inputs:transmission_dispersion_abbe_number = 0"])
op_case("mat_openpbr_emission", ["float inputs:emission_luminance = 1e30", "color3f inputs:emission_color = (1, 1, 1)", "float inputs:geometry_opacity = nan"])

PREVIEW = ['uniform token info:id = "UsdPreviewSurface"', "token outputs:surface"]
PV = 'token outputs:surface.connect = </World/mat/s.outputs:surface>'


def pv_case(name, inputs, extra=""):
    case(name, BOUND + material(PREVIEW + inputs, extra, outputs=PV))


pv_case("mat_preview_default", [])
pv_case("mat_preview_nan", ["color3f inputs:diffuseColor = (nan, inf, -1)", "float inputs:roughness = nan", "float inputs:metallic = nan", "float inputs:ior = nan", "float inputs:opacity = nan", "float inputs:clearcoat = nan"])
pv_case("mat_preview_extreme", ["float inputs:roughness = -5", "float inputs:metallic = 50", "float inputs:ior = 0", "float inputs:opacity = 0.5", "float inputs:opacityThreshold = 2", "float inputs:clearcoat = 50", "float inputs:clearcoatRoughness = -1", "int inputs:useSpecularWorkflow = 5", "color3f inputs:specularColor = (50, -1, nan)", "float inputs:displacement = 1e30", "float inputs:occlusion = nan"])
pv_case("mat_preview_normal_zero", ["normal3f inputs:normal = (0, 0, 0)"])
pv_case("mat_preview_tex_missing", ["color3f inputs:diffuseColor.connect = </World/mat/tex.outputs:rgb>"],
        '        def Shader "tex"\n        {\n            uniform token info:id = "UsdUVTexture"\n            asset inputs:file = @does_not_exist.png@\n            float3 outputs:rgb\n        }\n')
pv_case("mat_preview_tex_corrupt", ["color3f inputs:diffuseColor.connect = </World/mat/tex.outputs:rgb>", "normal3f inputs:normal.connect = </World/mat/tex.outputs:rgb>"],
        '        def Shader "tex"\n        {\n            uniform token info:id = "UsdUVTexture"\n            asset inputs:file = @corrupt.exr@\n            float3 outputs:rgb\n        }\n')
pv_case("mat_preview_tex_nan", ["color3f inputs:diffuseColor.connect = </World/mat/tex.outputs:rgb>", "color3f inputs:emissiveColor.connect = </World/mat/tex.outputs:rgb>", "float inputs:roughness.connect = </World/mat/tex.outputs:r>", "float inputs:opacity.connect = </World/mat/tex.outputs:g>"],
        '        def Shader "tex"\n        {\n            uniform token info:id = "UsdUVTexture"\n            asset inputs:file = @nan.exr@\n            float4 inputs:scale = (nan, 1e30, -1, 0)\n            float4 inputs:bias = (inf, 0, 0, 0)\n            float3 outputs:rgb\n            float outputs:r\n            float outputs:g\n        }\n')
pv_case("mat_preview_tex_empty_path", ["color3f inputs:diffuseColor.connect = </World/mat/tex.outputs:rgb>"],
        '        def Shader "tex"\n        {\n            uniform token info:id = "UsdUVTexture"\n            asset inputs:file = @@\n            token inputs:wrapS = "bogus"\n            token inputs:sourceColorSpace = "nonsense"\n            float3 outputs:rgb\n        }\n')
pv_case("mat_preview_tex_udim_missing", ["color3f inputs:diffuseColor.connect = </World/mat/tex.outputs:rgb>"],
        '        def Shader "tex"\n        {\n            uniform token info:id = "UsdUVTexture"\n            asset inputs:file = @nothing.<UDIM>.exr@\n            float3 outputs:rgb\n        }\n')
pv_case("mat_preview_st_transform_bad", ["color3f inputs:diffuseColor.connect = </World/mat/tex.outputs:rgb>"],
        '        def Shader "tex"\n        {\n            uniform token info:id = "UsdUVTexture"\n            asset inputs:file = @tiny.exr@\n            float2 inputs:st.connect = </World/mat/xf.outputs:result>\n            float3 outputs:rgb\n        }\n'
        '        def Shader "xf"\n        {\n            uniform token info:id = "UsdTransform2d"\n            float2 inputs:in.connect = </World/mat/rd.outputs:result>\n            float inputs:rotation = nan\n            float2 inputs:scale = (0, inf)\n            float2 inputs:translation = (1e30, -1e30)\n            float2 outputs:result\n        }\n'
        '        def Shader "rd"\n        {\n            uniform token info:id = "UsdPrimvarReader_float2"\n            string inputs:varname = "does_not_exist"\n            float2 outputs:result\n        }\n')
case("mat_cycle", BOUND + material(PREVIEW + ["color3f inputs:diffuseColor.connect = </World/mat/a.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_add_color3"\n            color3f inputs:in1.connect = </World/mat/b.outputs:out>\n            color3f outputs:out\n        }\n'
     '        def Shader "b"\n        {\n            uniform token info:id = "ND_add_color3"\n            color3f inputs:in1.connect = </World/mat/a.outputs:out>\n            color3f outputs:out\n        }\n'))
case("mat_cycle_mtlx", BOUND + material(SS + ["color3f inputs:base_color.connect = </World/mat/a.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_add_color3"\n            color3f inputs:in1.connect = </World/mat/b.outputs:out>\n            color3f outputs:out\n        }\n'
     '        def Shader "b"\n        {\n            uniform token info:id = "ND_add_color3"\n            color3f inputs:in1.connect = </World/mat/a.outputs:out>\n            color3f outputs:out\n        }\n', outputs=MTLX))
case("mat_self_cycle", BOUND + material(SS + ["color3f inputs:base_color.connect = </World/mat/a.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_add_color3"\n            color3f inputs:in1.connect = </World/mat/a.outputs:out>\n            color3f outputs:out\n        }\n', outputs=MTLX))
case("mat_no_id", BOUND + material(["token outputs:surface"]))
case("mat_unknown_id", BOUND + material(['uniform token info:id = "ND_made_up_surfaceshader"', "token outputs:surface"], outputs=MTLX))
case("mat_empty", BOUND + '\n    def Material "mat"\n    {\n    }\n')
case("mat_dangling_output", BOUND + '\n    def Material "mat"\n    {\n        token outputs:surface.connect = </World/mat/nothing.outputs:surface>\n    }\n')
case("mat_bind_missing", BALL % "        rel material:binding = </World/nothing>")
case("mat_bind_non_material", BALL % "        rel material:binding = </World/ground>")
case("mat_surface_is_pattern", BOUND + material(['uniform token info:id = "ND_add_color3"', "color3f outputs:out"], outputs='token outputs:mtlx:surface.connect = </World/mat/s.outputs:out>'))
case("mat_wrong_output_name", BOUND + material(SS, outputs='token outputs:mtlx:surface.connect = </World/mat/s.outputs:nonexistent>'))
case("mat_math_extremes", BOUND + material(SS + ["color3f inputs:base_color.connect = </World/mat/a.outputs:out>", "float inputs:specular_roughness.connect = </World/mat/b.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_divide_color3"\n            color3f inputs:in1 = (1, 1, 1)\n            color3f inputs:in2 = (0, 0, 0)\n            color3f outputs:out\n        }\n'
     '        def Shader "b"\n        {\n            uniform token info:id = "ND_ln_float"\n            float inputs:in = -1\n            float outputs:out\n        }\n', outputs=MTLX))
case("mat_math_power", BOUND + material(SS + ["color3f inputs:base_color.connect = </World/mat/a.outputs:out>", "float inputs:emission.connect = </World/mat/b.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_power_color3"\n            color3f inputs:in1 = (-2, 0, 1e30)\n            color3f inputs:in2 = (0.5, -1, 1e30)\n            color3f outputs:out\n        }\n'
     '        def Shader "b"\n        {\n            uniform token info:id = "ND_sqrt_float"\n            float inputs:in = -4\n            float outputs:out\n        }\n', outputs=MTLX))
case("mat_noise_extremes", BOUND + material(SS + ["color3f inputs:base_color.connect = </World/mat/a.outputs:out>", "float inputs:specular_roughness.connect = </World/mat/b.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_fractal3d_color3"\n            int inputs:octaves = 2000000000\n            float inputs:lacunarity = 1e30\n            float inputs:diminish = nan\n            vector3f inputs:amplitude = (1e30, -1, nan)\n            color3f outputs:out\n        }\n'
     '        def Shader "b"\n        {\n            uniform token info:id = "ND_worleynoise3d_float"\n            float inputs:jitter = 1e30\n            vector3f inputs:position = (nan, inf, 1e38)\n            float outputs:out\n        }\n', outputs=MTLX))
case("mat_image_extremes", BOUND + material(SS + ["color3f inputs:base_color.connect = </World/mat/a.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_image_color3"\n            asset inputs:file = @tiny.exr@\n            float2 inputs:texcoord = (nan, 1e30)\n            string inputs:uaddressmode = "bogus"\n            string inputs:filtertype = "bogus"\n            color3f inputs:default = (nan, 1, 1)\n            color3f outputs:out\n        }\n', outputs=MTLX))
case("mat_tiled_extremes", BOUND + material(SS + ["color3f inputs:base_color.connect = </World/mat/a.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_tiledimage_color3"\n            asset inputs:file = @tiny.exr@\n            float2 inputs:uvtiling = (0, 0)\n            float2 inputs:uvoffset = (inf, nan)\n            float2 inputs:realworldimagesize = (0, 0)\n            float2 inputs:realworldtilesize = (0, 0)\n            color3f outputs:out\n        }\n', outputs=MTLX))
case("mat_normalmap_bad", BOUND + material(SS + ["vector3f inputs:normal.connect = </World/mat/a.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_normalmap_float"\n            vector3f inputs:in = (0.5, 0.5, 0.5)\n            float inputs:scale = 1e30\n            vector3f inputs:normal = (0, 0, 0)\n            vector3f inputs:tangent = (0, 0, 0)\n            vector3f outputs:out\n        }\n', outputs=MTLX))
case("mat_bump_bad", BOUND + material(SS + ["vector3f inputs:normal.connect = </World/mat/a.outputs:out>"],
     '        def Shader "a"\n        {\n            uniform token info:id = "ND_heighttonormal_vector3"\n            float inputs:in.connect = </World/mat/n.outputs:out>\n            float inputs:scale = 1e30\n            vector3f outputs:out\n        }\n'
     '        def Shader "n"\n        {\n            uniform token info:id = "ND_noise3d_float"\n            float inputs:amplitude = 1e30\n            float outputs:out\n        }\n', outputs=MTLX))
DISP_OUT = MTLX + '\n        token outputs:mtlx:displacement.connect = </World/mat/d.outputs:out>'


def disp_case(name, lines, target=BOUND):
    case(name, target + material(SS, '        def Shader "d"\n        {\n            uniform token info:id = "ND_displacement_float"\n%s\n            token outputs:out\n        }\n' % "\n".join("            " + l for l in lines), outputs=DISP_OUT))


QUADMESH = mesh("q", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]", "point3f[] points = %s" % QUAD_PTS,
                      'uniform token subdivisionScheme = "catmullClark"', "rel material:binding = </World/mat>"])
disp_case("mat_disp_nan", ["float inputs:displacement = nan", "float inputs:scale = 1"], QUADMESH)
disp_case("mat_disp_inf", ["float inputs:displacement = inf", "float inputs:scale = 1"], QUADMESH)
disp_case("mat_disp_huge", ["float inputs:displacement = 1e30", "float inputs:scale = 1e8"], QUADMESH)
disp_case("mat_disp_sphere_huge", ["float inputs:displacement = 1e30", "float inputs:scale = 1"])
disp_case("mat_disp_sphere_negative", ["float inputs:displacement = -5", "float inputs:scale = 1"])
disp_case("mat_disp_sphere_nan", ["float inputs:displacement = nan", "float inputs:scale = 1"])
case("mat_volume_on_surface", BOUND + material(['uniform token info:id = "ND_volume_vdf"', "token outputs:out"], outputs='token outputs:mtlx:volume.connect = </World/mat/s.outputs:out>'))
case("mat_light_shader_bound", BOUND + material(['uniform token info:id = "ND_uniform_edf"', "color3f inputs:color = (1e30, 1, 1)", "token outputs:out"], outputs='token outputs:mtlx:surface.connect = </World/mat/s.outputs:out>'))
case("mat_many_layers", BOUND + material(
    ['uniform token info:id = "ND_surface"', "BSDF inputs:bsdf.connect = </World/mat/l9.outputs:out>", "float inputs:opacity = 1", "token outputs:surface"],
    "".join('        def Shader "l%d"\n        {\n            uniform token info:id = "ND_layer_bsdf"\n            BSDF inputs:top.connect = </World/mat/t%d.outputs:out>\n            BSDF inputs:base.connect = </World/mat/%s.outputs:out>\n            token outputs:out\n        }\n'
            '        def Shader "t%d"\n        {\n            uniform token info:id = "ND_dielectric_bsdf"\n            float inputs:ior = %g\n            vector2f inputs:roughness = (%g, %g)\n            token outputs:out\n        }\n'
            % (i, i, ("l%d" % (i - 1)) if i else "base", i, 1.0 + 0.1 * i, 0.05 * i, 0.05 * i) for i in range(10)) +
    '        def Shader "base"\n        {\n            uniform token info:id = "ND_oren_nayar_diffuse_bsdf"\n            token outputs:out\n        }\n', outputs=MTLX))
case("mat_bsdf_extremes", BOUND + material(
    ['uniform token info:id = "ND_surface"', "BSDF inputs:bsdf.connect = </World/mat/m.outputs:out>", "EDF inputs:edf.connect = </World/mat/e.outputs:out>", "float inputs:opacity = nan", "token outputs:surface"],
    '        def Shader "m"\n        {\n            uniform token info:id = "ND_mix_bsdf"\n            BSDF inputs:fg.connect = </World/mat/c.outputs:out>\n            BSDF inputs:bg.connect = </World/mat/g.outputs:out>\n            float inputs:mix = nan\n            token outputs:out\n        }\n'
    '        def Shader "c"\n        {\n            uniform token info:id = "ND_conductor_bsdf"\n            color3f inputs:ior = (0, -1, nan)\n            color3f inputs:extinction = (0, -1, inf)\n            vector2f inputs:roughness = (0, 1e30)\n            token outputs:out\n        }\n'
    '        def Shader "g"\n        {\n            uniform token info:id = "ND_generalized_schlick_bsdf"\n            color3f inputs:color0 = (5, -1, nan)\n            color3f inputs:color82 = (5, -1, nan)\n            color3f inputs:color90 = (5, -1, nan)\n            float inputs:exponent = 0\n            vector2f inputs:roughness = (nan, nan)\n            string inputs:scatter_mode = "RT"\n            token outputs:out\n        }\n'
    '        def Shader "e"\n        {\n            uniform token info:id = "ND_uniform_edf"\n            color3f inputs:color = (nan, 0, 0)\n            token outputs:out\n        }\n', outputs=MTLX))
case("mat_bsdf_weights", BOUND + material(
    ['uniform token info:id = "ND_surface"', "BSDF inputs:bsdf.connect = </World/mat/a.outputs:out>", "token outputs:surface"],
    '        def Shader "a"\n        {\n            uniform token info:id = "ND_add_bsdf"\n            BSDF inputs:in1.connect = </World/mat/x.outputs:out>\n            BSDF inputs:in2.connect = </World/mat/y.outputs:out>\n            token outputs:out\n        }\n'
    '        def Shader "x"\n        {\n            uniform token info:id = "ND_multiply_bsdfF"\n            BSDF inputs:in1.connect = </World/mat/d.outputs:out>\n            float inputs:in2 = 1e30\n            token outputs:out\n        }\n'
    '        def Shader "y"\n        {\n            uniform token info:id = "ND_multiply_bsdfC"\n            BSDF inputs:in1.connect = </World/mat/h.outputs:out>\n            color3f inputs:in2 = (-1, nan, inf)\n            token outputs:out\n        }\n'
    '        def Shader "d"\n        {\n            uniform token info:id = "ND_burley_diffuse_bsdf"\n            float inputs:roughness = -5\n            float inputs:weight = 50\n            token outputs:out\n        }\n'
    '        def Shader "h"\n        {\n            uniform token info:id = "ND_sheen_bsdf"\n            float inputs:roughness = 0\n            string inputs:mode = "zeltner"\n            token outputs:out\n        }\n', outputs=MTLX))
case("mat_hair_extremes", curves(['uniform token type = "cubic"', 'uniform token basis = "bspline"', "int[] curveVertexCounts = [4]",
                                  "point3f[] points = [(0,0.5,0),(0.3,1,0),(0,1.5,0),(0,2,0)]", "float[] widths = [0.2]",
                                  "rel material:binding = </World/mat>"]) + material(
    ['uniform token info:id = "ND_surface"', "BSDF inputs:bsdf.connect = </World/mat/h.outputs:out>", "token outputs:surface"],
    '        def Shader "h"\n        {\n            uniform token info:id = "ND_chiang_hair_bsdf"\n            color3f inputs:tint_R = (5, -1, nan)\n            float inputs:ior = 0\n            vector2f inputs:roughness_R = (0, 0)\n            vector2f inputs:roughness_TT = (-1, 50)\n            vector2f inputs:roughness_TRT = (nan, nan)\n            float inputs:cuticle_angle = 50\n            vector3f inputs:absorption_coefficient = (-1, nan, 1e30)\n            token outputs:out\n        }\n', outputs=MTLX))
case("mat_hair_on_mesh", BOUND + material(
    ['uniform token info:id = "ND_surface"', "BSDF inputs:bsdf.connect = </World/mat/h.outputs:out>", "token outputs:surface"],
    '        def Shader "h"\n        {\n            uniform token info:id = "ND_chiang_hair_bsdf"\n            token outputs:out\n        }\n', outputs=MTLX))

# ---------------------------------------------------------------- volumes
case("vol_missing_file", '\n    def Volume "v"\n    {\n        rel field:density = </World/v/density>\n        def OpenVDBAsset "density"\n        {\n            asset filePath = @nothing.vdb@\n            token fieldName = "density"\n        }\n    }\n')
case("vol_corrupt_file", '\n    def Volume "v"\n    {\n        rel field:density = </World/v/density>\n        def OpenVDBAsset "density"\n        {\n            asset filePath = @corrupt.vdb@\n            token fieldName = "density"\n        }\n    }\n')
case("vol_no_fields", '\n    def Volume "v"\n    {\n    }\n')
case("vol_field_missing_prim", '\n    def Volume "v"\n    {\n        rel field:density = </World/nothing>\n    }\n')
case("vol_field_wrong_type", '\n    def Volume "v"\n    {\n        rel field:density = </World/ground>\n    }\n')
VOLMAT = ('\n    def Material "vmat"\n    {\n        token outputs:mtlx:volume.connect = </World/vmat/s.outputs:out>\n        def Shader "s"\n        {\n            uniform token info:id = "ND_anisotropic_vdf"\n%s\n            token outputs:out\n        }\n    }\n')


def fog(name, lines, geo=None):
    geo = geo or (BALL % "        rel material:binding = </World/vmat>")
    case(name, geo + VOLMAT % "\n".join("            " + l for l in lines))


fog("fog_default", ["color3f inputs:absorption = (0.5, 0.5, 0.5)", "color3f inputs:scattering = (0.5, 0.5, 0.5)"])
fog("fog_nan", ["color3f inputs:absorption = (nan, 0.5, 0.5)", "color3f inputs:scattering = (0.5, inf, 0.5)", "float inputs:anisotropy = nan"])
fog("fog_negative", ["color3f inputs:absorption = (-5, -5, -5)", "color3f inputs:scattering = (-1, 2, 3)", "float inputs:anisotropy = -7"])
fog("fog_huge", ["color3f inputs:absorption = (1e30, 1e30, 1e30)", "color3f inputs:scattering = (1e30, 1e30, 1e30)", "float inputs:anisotropy = 1"])
fog("fog_zero", ["color3f inputs:absorption = (0, 0, 0)", "color3f inputs:scattering = (0, 0, 0)"])
fog("fog_scatter_only_dense", ["color3f inputs:absorption = (0, 0, 0)", "color3f inputs:scattering = (1e6, 1e6, 1e6)", "float inputs:anisotropy = 0.999999"])
fog("fog_open_quad", ["color3f inputs:absorption = (0.5, 0.5, 0.5)", "color3f inputs:scattering = (0.5, 0.5, 0.5)"],
    mesh("q", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]", "point3f[] points = %s" % QUAD_PTS, "rel material:binding = </World/vmat>"]))
fog("fog_camera_inside", ["color3f inputs:absorption = (0.05, 0.05, 0.05)", "color3f inputs:scattering = (0.05, 0.05, 0.05)"],
    '\n    def Sphere "big"\n    {\n        double radius = 50\n        rel material:binding = </World/vmat>\n    }\n' + BALL % "")
fog("fog_on_points", ["color3f inputs:absorption = (0.5, 0.5, 0.5)", "color3f inputs:scattering = (0.5, 0.5, 0.5)"],
    points(["point3f[] points = [(0,1,0),(1,1,0)]", "float[] widths = [0.8]", "rel material:binding = </World/vmat>"]) +
    curves(['uniform token type = "linear"', "int[] curveVertexCounts = [3]", "point3f[] points = [(0,0.5,0),(0,1,0),(0,2,0)]", "float[] widths = [0.3]", "rel material:binding = </World/vmat>"]))
fog("fog_nested", ["color3f inputs:absorption = (0.5, 0.5, 0.5)", "color3f inputs:scattering = (0.5, 0.5, 0.5)"],
    "".join('\n    def Sphere "s%d"\n    {\n        double radius = %g\n        double3 xformOp:translate = (0, 1, 0)\n        uniform token[] xformOpOrder = ["xformOp:translate"]\n        rel material:binding = </World/vmat>\n    }\n' % (i, 0.1 + 0.08 * i) for i in range(12)))

# ---------------------------------------------------------------- render settings
def rs_case(name, lines, vars_=None, product=None, body=None, extra=()):
    case(name, body if body is not None else BALL % "", settings=settings(lines, vars_, product), extra=["--settings", "/Render/rs"] + list(extra))


BEAUTY = ("beauty", "color4f", "C.*[LO]", "lpe")
rs_case("rs_plain", [], [BEAUTY])
rs_case("rs_samples_zero", ["int nova:samples = 0"], [BEAUTY])
rs_case("rs_samples_negative", ["int nova:samples = -5"], [BEAUTY])
rs_case("rs_depth_negative", ["int nova:maxDepth = -1", "int nova:object:diffuseLimit = -5", "int nova:object:reflectLimit = -5", "int nova:object:refractLimit = -5", "int nova:object:volumeLimit = -5", "int nova:object:sssLimit = -5"], [BEAUTY])
rs_case("rs_depth_huge", ["int nova:maxDepth = 2147483647", "int nova:object:diffuseLimit = 2147483647", "int nova:object:reflectLimit = 2147483647", "int nova:object:refractLimit = 2147483647"], [BEAUTY], extra=["--timeout", "20"])
rs_case("rs_res_zero", ["int2 resolution = (0, 0)"], [BEAUTY])
rs_case("rs_res_negative", ["int2 resolution = (-64, -48)"], [BEAUTY])
rs_case("rs_res_one", ["int2 resolution = (1, 1)"], [BEAUTY])
rs_case("rs_window_empty", ["float4 dataWindowNDC = (0.5, 0.5, 0.5, 0.5)"], [BEAUTY])
rs_case("rs_window_inverted", ["float4 dataWindowNDC = (0.8, 0.9, 0.2, 0.1)"], [BEAUTY])
rs_case("rs_window_outside", ["float4 dataWindowNDC = (2, 2, 3, 3)"], [BEAUTY])
rs_case("rs_window_huge", ["float4 dataWindowNDC = (-50, -50, 50, 50)"], [BEAUTY], extra=["--timeout", "30"])
rs_case("rs_window_nan", ["float4 dataWindowNDC = (nan, 0, inf, 1)"], [BEAUTY])
rs_case("rs_par_zero", ["float pixelAspectRatio = 0"], [BEAUTY])
rs_case("rs_par_negative", ["float pixelAspectRatio = -2"], [BEAUTY])
rs_case("rs_par_nan", ["float pixelAspectRatio = nan"], [BEAUTY])
rs_case("rs_conform_bogus", ['token aspectRatioConformPolicy = "bogus"'], [BEAUTY])
rs_case("rs_filter_bogus", ['string nova:pixelFilter = "bogus"', "float nova:pixelFilterWidth = -3"], [BEAUTY])
rs_case("rs_filter_karma_bogus", ['string karma:global:pixelfilter = "bogus"', "float karma:global:pixelfiltersize = 0"], [BEAUTY])
rs_case("rs_filter_karma_huge", ['string karma:global:pixelfilter = "gauss"', "float karma:global:pixelfiltersize = 1e30"], [BEAUTY])
rs_case("rs_filter_karma_nan", ['string karma:global:pixelfilter = "gauss"', "float karma:global:pixelfiltersize = nan"], [BEAUTY])
rs_case("rs_karma_samples_weird", ["int karma:global:pathtracedsamples = -1", "int karma:global:samplesperpixel = 0", "int karma:global:minpathtracedsamples = 500", "float karma:global:pixeloracle:variance = nan"], [BEAUTY])
rs_case("rs_karma_limits_weird", ["int karma:object:diffuselimit = -1", "int karma:object:reflectlimit = 2147483647", "float karma:global:colorlimit = -1", "float karma:object:colorlimitindirect = nan", "int karma:global:volumelimit = -7", "float karma:global:russianroulettecutoff = nan"], [BEAUTY], extra=["--timeout", "20"])
rs_case("rs_colorlimit_zero", ["float karma:global:colorlimit = 0"], [BEAUTY])
rs_case("rs_adaptive_weird", ['string nova:pixelOracle = "variance"', "float nova:varianceThreshold = nan", "int nova:minSamples = 50", "int nova:samples = 4"], [BEAUTY])
rs_case("rs_adaptive_karma", ['string karma:global:pixeloracle = "variance"', "float karma:global:pixeloracle:variance = 0", "int karma:global:minpathtracedsamples = 0", "int karma:global:pathtracedsamples = 8"], [BEAUTY])
rs_case("rs_denoise_tiny", ['bool nova:denoise = 1', "int2 resolution = (1, 1)"], [BEAUTY])
rs_case("rs_denoise", ['bool nova:denoise = 1', 'string nova:denoiseAovs = "beauty bogus N"'], [BEAUTY, ("N", "normal3f", "N", "raw")])
rs_case("rs_lpe_garbage", [], [BEAUTY, ("a", "color3f", "C<<<", "lpe"), ("b", "color3f", "", "lpe"), ("c", "color3f", "C[", "lpe"), ("d", "color3f", "C.*)", "lpe"),
                               ("e", "color3f", "((((((((((C.*))))))))))", "lpe"), ("f", "color3f", "C<RD>*<TS>+<..'x'>.*['a''b']L", "lpe"), ("g", "color3f", "unoccluded;", "lpe"),
                               ("h", "color3f", "shadow;shadow;C.*L", "lpe"), ("i", "color3f", "C" + ".?" * 200 + "L", "lpe"), ("j", "color3f", "C(D|S|R|T){5000}L", "lpe"),
                               ("k", "color3f", "C[^LO]*<L.'nope'>", "lpe"), ("l", "color3f", "\\xff\\x00", "lpe")])
rs_case("rs_lpe_many", [], [BEAUTY] + [("v%d" % i, "color3f", "C<%s%s>%s" % ("RT"[i % 2], "DSG"[i % 3], ".*" if i % 5 else "") + "[LO]", "lpe") for i in range(120)])
rs_case("rs_aov_unknown", [], [BEAUTY, ("x", "color3f", "does_not_exist", "raw"), ("y", "float", "depth", "raw"), ("z", "bogus", "N", "raw"), ("w", "color3f", "foo", "primvar"), ("u", "int", "primId", "raw"), ("q", "color3f", "P", "bogus")])
rs_case("rs_aov_duplicate", [], [BEAUTY, ("N", "normal3f", "N", "raw"), ("N2", "normal3f", "N", "raw", ['string driver:parameters:aov:name = "N"']), ("C2", "color4f", "C.*[LO]", "lpe", ['string driver:parameters:aov:name = "C"'])])
rs_case("rs_aov_formats", [], [BEAUTY, ("N", "normal3f", "N", "raw", ['token driver:parameters:aov:format = "bogus"']), ("P", "point3f", "P", "raw", ['token driver:parameters:aov:format = "int"']),
                               ("d", "float", "depth", "raw", ['token driver:parameters:aov:format = "color4h"', 'float4 driver:parameters:aov:clearValue = (nan, 0, 0, 0)'])])
rs_case("rs_no_vars", [], [])
rs_case("rs_crypto_weird", [], [BEAUTY, ("crypto_object", "color4f", "bogus", "raw", ['string driver:parameters:aov:cryptomatte:rank = "x"']),
                                ("cm", "color4f", "cryptomatte:primvars:nothing", "raw"), ("cm2", "color4f", "crypto_material", "raw")],
        product=['int driver:parameters:cryptomatte:levels = -5'])
rs_case("rs_product_bad_path", [], [BEAUTY], product=['token productName = "Z:/does/not/exist/<>|?*.exr"'])
rs_case("rs_product_no_name", [], [BEAUTY], product=['token productName = ""'])
rs_case("rs_product_type_bogus", [], [BEAUTY], product=['token productType = "bogus"'])
rs_case("rs_product_deep", [], [BEAUTY], product=['token productType = "karma:deep"', 'token productName = "deep.exr"'])
rs_case("rs_product_checkpoint_bad", [], [BEAUTY], product=['token productType = "karma:checkpoint"', 'token productName = "Z:/nothing/cp.exr"'])
rs_case("rs_camera_missing", ["rel camera = </World/nothing>"], [BEAUTY])
rs_case("rs_camera_not_camera", ["rel camera = </World/ground>"], [BEAUTY])
rs_case("rs_shutter_settings", ["bool disableMotionBlur = 1", "int nova:maxMotionSteps = -3", "bool instantaneousShutter = 1"], [BEAUTY])
rs_case("rs_motion_steps_huge", ["int nova:maxMotionSteps = 2147483647"], [BEAUTY])
rs_case("rs_light_strategy_bogus", ['string nova:lightStrategy = "bogus"', 'string nova:lightSamplingMode = "bogus"', "float nova:lightSamplingQuality = nan"], [BEAUTY])
rs_case("rs_guiding_weird", ["bool karma:global:guiding_enable = 1", "int karma:global:guiding_trainingsamples = -1", "float karma:global:guiding_probability = 50", 'token karma:global:guiding_component = "bogus"', "int karma:global:guiding_spatialresolution = 0"], [BEAUTY])
rs_case("rs_device_bogus", ['string nova:device = "quantum"'], [BEAUTY])
rs_case("rs_threads_weird", ["int nova:threads = -8"], [BEAUTY])
rs_case("rs_wrong_types", ['string nova:samples = "many"', "float nova:maxDepth = 3.7", 'int nova:pixelFilter = 3', "bool nova:pixelFilterWidth = 1", "string nova:denoise = \"yes\"", "float3 nova:object:diffuseLimit = (1, 2, 3)", 'token karma:global:pathtracedsamples = "x"', "int[] karma:global:colorlimit = [1, 2]"], [BEAUTY])
rs_case("rs_holdout_weird", ['string karma:global:mattecollection = "/World/nothing /World/ball"', 'string karma:global:phantomcollection = "garbage{{}}"'], [BEAUTY])
rs_case("rs_outline_weird", ["bool karma:global:outline_enable = 1", "float karma:global:outline_width = -5", "float karma:global:outline_depth_threshold = nan"], [BEAUTY])
rs_case("rs_bucket_weird", ["int karma:global:bucketsize = -5", "int nova:bucketSize = 0", 'token karma:global:bucketorder = "bogus"', "int karma:global:rendertime = -5"], [BEAUTY])
rs_case("rs_frame_fps", [], [BEAUTY], extra=["--frame", "1e30"])
rs_case("rs_frame_nan", [], [BEAUTY], extra=["--frame", "nan"])

# ---------------------------------------------------------------- structure
case("empty_stage_world", "", camera="", lights="", ground="")
case("only_camera", "", lights="", ground="")
case("only_lights", "", ground="")
case("deep_hierarchy", "".join('\n    def Xform "x%d"\n    {' % i for i in range(150)) + BALL % "" + "".join("\n    }" for _ in range(150)))
case("many_small_meshes", "".join(mesh("m%d" % i, ["int[] faceVertexCounts = [3]", "int[] faceVertexIndices = [0, 1, 2]",
                                                    "point3f[] points = [(%g, 1, 0), (%g, 1.2, 0), (%g, 1, 0.2)]" % (i * 0.01 - 2, i * 0.01 - 2, i * 0.01 - 2)]) for i in range(400)))
case("visibility_purpose", BALL % '        token visibility = "bogus"\n        uniform token purpose = "bogus"')
case("double_sided_orientation", mesh("m", ["int[] faceVertexCounts = [4]", "int[] faceVertexIndices = [0, 1, 2, 3]", "point3f[] points = %s" % QUAD_PTS,
                                            "uniform bool doubleSided = 0", 'uniform token orientation = "leftHanded"']))
case("native_instances", '\n    def Xform "proto" (\n        instanceable = true\n    )\n    {\n        def Sphere "s"\n        {\n            double radius = 0.4\n        }\n    }\n' +
     "".join('\n    def Xform "i%d" (\n        instanceable = true\n        references = </World/proto>\n    )\n    {\n        double3 xformOp:translate = (%d, 1, 0)\n        float3 xformOp:scale = (%s)\n        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n    }\n' % (i, i - 2, s) for i, s in enumerate(["1, 1, 1", "0, 0, 0", "nan, 1, 1", "-1, 1, 1", "1e30, 1, 1"])))
case("karma_object_props", BALL % ("        int primvars:karma:object:diffuselimit = -5\n        float primvars:karma:object:dicingquality = nan\n        string primvars:karma:object:curvestyle = \"bogus\"\n"
                                   "        bool primvars:karma:object:causticsenable = 1\n        float primvars:karma:object:causticsroughnessclamp = nan\n        int primvars:karma:object:geosamples = -4\n"
                                   "        int primvars:karma:object:xformsamples = 2147483647\n        float primvars:karma:object:mblur = nan\n        token primvars:karma:object:vblur = \"bogus\"\n"
                                   "        string primvars:karma:object:rendervisibility = \"garbage & ~bogus\"\n        float primvars:karma:object:volumesteprate = -1\n        bool primvars:karma:object:treat_as_lightsource = 1\n"
                                   "        float primvars:nova:object:widthScale = nan\n        int primvars:nova:object:refineLevel = -9\n        float primvars:karma:object:displacementbound = nan"))
case("dicing_extremes", QUADMESH + material(SS, '        def Shader "d"\n        {\n            uniform token info:id = "ND_displacement_float"\n            float inputs:displacement.connect = </World/mat/n.outputs:out>\n            float inputs:scale = 0.2\n            token outputs:out\n        }\n'
                                            '        def Shader "n"\n        {\n            uniform token info:id = "ND_noise3d_float"\n            float outputs:out\n        }\n', outputs=DISP_OUT),
     settings=settings(["float karma:object:dicingquality = 1e30", "float nova:dicingQuality = 1e30"], [BEAUTY]), extra=["--settings", "/Render/rs", "--timeout", "40"])
case("subdiv_big_ngon", mesh("m", ["int[] faceVertexCounts = [300]", "int[] faceVertexIndices = [%s]" % ",".join(str(i) for i in range(300)),
                                   "point3f[] points = [%s]" % ",".join("(%g, %g, 0)" % (__import__("math").cos(i * 0.0209) * 2, 1.5 + __import__("math").sin(i * 0.0209)) for i in range(300)),
                                   'uniform token subdivisionScheme = "catmullClark"']))
case("subdiv_valence_high", mesh("m", ["int[] faceVertexCounts = [%s]" % ",".join("3" for _ in range(200)),
                                       "int[] faceVertexIndices = [%s]" % ",".join("0,%d,%d" % (1 + i, 1 + (i + 1) % 200) for i in range(200)),
                                       "point3f[] points = [(0,1.5,0.3),%s]" % ",".join("(%g, %g, 0)" % (__import__("math").cos(i * 0.0314159) * 2, 1.5 + __import__("math").sin(i * 0.0314159)) for i in range(200)),
                                       'uniform token subdivisionScheme = "catmullClark"']))


# ---------------------------------------------------------------- assets
def write_assets(out, hfs):
    sys.path.insert(0, os.path.join(hfs, "python313", "lib", "site-packages"))
    with open(os.path.join(out, "corrupt.exr"), "wb") as f:
        f.write(b"\x76\x2f\x31\x01" + b"this is not an exr file at all" * 20)
    with open(os.path.join(out, "corrupt.vdb"), "wb") as f:
        f.write(b" BDV\x00\x00\x00\x00" + b"garbage" * 50)
    try:
        import numpy as np
        import OpenImageIO as oiio
    except Exception as e:  # noqa
        print("no OpenImageIO:", e)
        return

    def save(name, arr):
        spec = oiio.ImageSpec(arr.shape[1], arr.shape[0], arr.shape[2], oiio.FLOAT)
        o = oiio.ImageOutput.create(os.path.join(out, name))
        o.open(os.path.join(out, name), spec)
        o.write_image(arr)
        o.close()

    save("tiny.exr", np.full((1, 1, 3), 0.5, np.float32))
    save("black.exr", np.zeros((4, 8, 3), np.float32))
    a = np.full((4, 8, 3), 0.5, np.float32)
    a[1, 2, 0] = np.nan
    a[2, 3, 1] = np.inf
    a[3, 4, 2] = -np.inf
    save("nan.exr", a)
    save("neg.exr", np.full((4, 8, 3), -2.0, np.float32))


def analyze(path, hfs):
    import numpy as np
    import OpenImageIO as oiio
    inp = oiio.ImageInput.open(path)
    if not inp:
        return {"error": "cannot open output"}
    res = {"parts": 0, "nonfinite": 0, "max": 0.0, "min": 0.0}
    sub = 0
    while inp.seek_subimage(sub, 0):
        spec = inp.spec()
        px = inp.read_image(sub, 0, 0, spec.nchannels, oiio.FLOAT)
        if px is not None and px.size:
            bad = ~np.isfinite(px)
            res["nonfinite"] += int(bad.sum())
            good = px[~bad]
            if good.size:
                res["max"] = float(max(res["max"], good.max()))
                res["min"] = float(min(res["min"], good.min()))
        sub += 1
    res["parts"] = sub
    inp.close()
    return res


def run_one(args, name, device):
    text, extra = CASES[name]
    scene = os.path.join(args.out, name + ".usda")
    outimg = os.path.join(args.out, "img", "%s_%s.exr" % (name, device))
    if os.path.exists(outimg):
        os.remove(outimg)
    cmd = [args.cli, scene, "-o", outimg, "--device", device, "--threads", "4"]
    if "--res" not in extra and "--settings" not in extra:
        cmd += ["--res", "64x48"]
    if "--spp" not in extra:
        cmd += ["--spp", "4"]
    if "--timeout" not in extra:
        cmd += ["--timeout", "45"]
    cmd += extra
    env = dict(os.environ)
    env["PATH"] = os.path.join(args.hfs, "bin") + os.pathsep + env["PATH"]
    env.pop("PXR_PLUGINPATH_NAME", None)
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=args.out, capture_output=True, text=True, timeout=args.limit, env=env, errors="replace")
        rc, so, se = p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        rc, so, se = "TIMEOUT", (e.stdout or b"").decode("utf8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or ""), \
            (e.stderr or b"").decode("utf8", "replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
    dt = time.time() - t0
    with open(os.path.join(args.out, "log", "%s_%s.txt" % (name, device)), "w", encoding="utf8", errors="replace") as f:
        f.write("cmd: %s\nrc: %s  time: %.1f\n--- stdout\n%s\n--- stderr\n%s\n" % (" ".join(cmd), rc, dt, so, se))
    info = {"name": name, "device": device, "rc": rc, "time": round(dt, 1)}
    if os.path.exists(outimg):
        try:
            info.update(analyze(outimg, args.hfs))
        except Exception as e:  # noqa
            info["error"] = "analyze: %s" % e
    else:
        info["noimage"] = True
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--hfs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="*")
    ap.add_argument("--devices", default="cpu")
    ap.add_argument("--skip", default="")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--limit", type=float, default=120)
    args = ap.parse_args()
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    os.makedirs(os.path.join(args.out, "img"), exist_ok=True)
    os.makedirs(os.path.join(args.out, "log"), exist_ok=True)
    write_assets(args.out, args.hfs)
    names = [n for n in CASES if any(fnmatch.fnmatch(n, p) for p in args.only.split(","))]
    names = [n for n in names if not any(fnmatch.fnmatch(n, p) for p in args.skip.split(",") if p)]
    for n in names:
        with open(os.path.join(args.out, n + ".usda"), "w", encoding="utf8") as f:
            f.write(CASES[n][0])
    results = []
    for device in args.devices.split(","):
        jobs = args.jobs if device == "cpu" else max(1, args.jobs // 2)
        with cf.ThreadPoolExecutor(jobs) as ex:
            for info in ex.map(lambda n: run_one(args, n, device), names):
                results.append(info)
                flag = []
                if info["rc"] != 0:
                    flag.append("RC=%s" % info["rc"])
                if info.get("nonfinite"):
                    flag.append("NONFINITE=%d" % info["nonfinite"])
                if info.get("noimage"):
                    flag.append("noimage")
                if info.get("error"):
                    flag.append(info["error"])
                if info.get("max", 0) > 1e6 or info.get("min", 0) < -1e6:
                    flag.append("range[%g,%g]" % (info.get("min", 0), info.get("max", 0)))
                if info["time"] > 30:
                    flag.append("slow")
                print("%-34s %-3s %6.1fs %s" % (info["name"], device, info["time"], " ".join(flag) if flag else "ok"), flush=True)
    with open(os.path.join(args.out, "results_%s.json" % args.devices.replace(",", "_")), "w") as f:
        json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
