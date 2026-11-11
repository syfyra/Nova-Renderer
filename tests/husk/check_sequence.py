"""husk check that a frame of a sequence is the frame rendered alone.

husk keeps one render index for all frames of a run, so the delegate sees a
sequence as edits: whatever a frame leaves behind may not show in the next.
Three things are checked, with Nova (CPU) and Nova XPU (when it renders on
this machine):

- sequence: frames 1 to 4 of an animated scene rendered in one husk run are
  bit-identical, in every AOV, to the same frames rendered by a run of their
  own. The scene moves its camera from far to near over displaced meshes (a
  textured ground, a subdivided cube, a deforming sheet), and animates a
  transform under motion blur, a mesh whose topology changes, visibility, a
  material colour, a light's intensity and a point instancer of point
  instancers.
- dicing: displacement is diced from the camera of the frame that is
  rendered, not from the first frame's (docs/bug-audit-2026-11.md, section
  14, S-1): the near frame has the triangles it has alone, and no mesh is
  diced twice for a frame.
- noise: the sample pattern changes from frame to frame (S-2). A scene in
  which nothing moves renders frames 1 and 2 with different noise around the
  same mean; with the same frame number, the same image.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_sequence.py --hfs ... --stage ... --out ...
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "reference", "scenes", "assets")

SCENE = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    startTimeCode = 1
    endTimeCode = 4
    timeCodesPerSecond = 24
    framesPerSecond = 24
)

def Xform "World"
{
    def Scope "Looks"
    {
        def Material "bumps"
        {
            token outputs:mtlx:displacement.connect = </World/Looks/bumps/disp.outputs:out>
            token outputs:mtlx:surface.connect = </World/Looks/bumps/surface.outputs:out>
            def Shader "surface"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color = (0.6, 0.55, 0.5)
                float inputs:specular_roughness = 0.4
                token outputs:out
            }
            def Shader "tex"
            {
                uniform token info:id = "ND_image_float"
                asset inputs:file = @%(assets)s/height_bumps.exr@
                float outputs:out
            }
            def Shader "disp"
            {
                uniform token info:id = "ND_displacement_float"
                float inputs:displacement.connect = </World/Looks/bumps/tex.outputs:out>
                float inputs:scale = 0.08
                token outputs:out
            }
        }
        def Material "checker"
        {
            token outputs:mtlx:surface.connect = </World/Looks/checker/surface.outputs:out>
            def Shader "surface"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color.connect = </World/Looks/checker/tex.outputs:out>
                float inputs:specular_roughness = 0.3
                token outputs:out
            }
            def Shader "tex"
            {
                uniform token info:id = "ND_image_color3"
                asset inputs:file = @%(assets)s/checker_lin.exr@
                color3f outputs:out
            }
        }
        def Material "anim"
        {
            token outputs:mtlx:surface.connect = </World/Looks/anim/surface.outputs:out>
            def Shader "surface"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color%(color)s
                float inputs:specular_roughness = 0.5
                token outputs:out
            }
        }
    }
    def Mesh "plane" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-3, 0, 3), (3, 0, 3), (3, 0, -3), (-3, 0, -3)]
        texCoord2f[] primvars:st = [(0, 0), (3, 0), (3, 3), (0, 3)] (
            interpolation = "vertex"
        )
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        rel material:binding = </World/Looks/bumps>
    }
    def Mesh "cube" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
        point3f[] points = [(0.7, 0.1, 0.4), (1.5, 0.1, 0.4), (0.7, 0.9, 0.4), (1.5, 0.9, 0.4), (0.7, 0.9, -0.4), (1.5, 0.9, -0.4), (0.7, 0.1, -0.4), (1.5, 0.1, -0.4)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1), (0, 0), (1, 0), (1, 1), (0, 1), (0, 0), (1, 0), (1, 1), (0, 1), (0, 0), (1, 0), (1, 1), (0, 1), (0, 0), (1, 0), (1, 1), (0, 1), (0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "faceVarying"
        )
        uniform token subdivisionScheme = "catmullClark"
        uniform bool doubleSided = 1
        rel material:binding = </World/Looks/bumps>
    }
    def Mesh "sheet" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points%(sheet)s
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        rel material:binding = </World/Looks/bumps>
    }
    def Mesh "fixed" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(1.8, 0.2, 1.2), (2.6, 0.2, 1.2), (2.6, 1.0, 1.0), (1.8, 1.0, 1.0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        int primvars:nova:dicingDepth = 4 (interpolation = "constant")
        rel material:binding = </World/Looks/bumps>
    }
    def Mesh "ball" (
        prepend apiSchemas = ["MaterialBindingAPI"]
        prepend references = @%(assets)s/sphere_st.usda@
    )
    {
        rel material:binding = </World/Looks/checker>
        double3 xformOp:translate%(ball)s
        double3 xformOp:scale = (0.35, 0.35, 0.35)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
    }
    def Mesh "morph" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts%(morph_counts)s
        int[] faceVertexIndices%(morph_indices)s
        point3f[] points%(morph_points)s
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        rel material:binding = </World/Looks/anim>
    }
    def Mesh "blink" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(1.8, 0.2, -1.2), (2.6, 0.2, -1.2), (2.6, 1.4, -1.2), (1.8, 1.4, -1.2)]
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        rel material:binding = </World/Looks/anim>
        token visibility%(blink)s
    }
    def PointInstancer "outer"
    {
        rel prototypes = [</World/outer/inner>]
        int[] protoIndices = [0, 0, 0]
        point3f[] positions%(outer)s
        def PointInstancer "inner"
        {
            rel prototypes = [</World/outer/inner/proto>, </World/outer/inner/box>]
            int[] protoIndices = [0, 1, 0, 1]
            point3f[] positions%(inner)s
            color3f[] primvars:displayColor = [(0.9, 0.2, 0.2), (0.2, 0.9, 0.2), (0.2, 0.2, 0.9), (0.9, 0.9, 0.2)] (
                interpolation = "instance"
            )
            def Sphere "proto"
            {
                double radius = 0.1
            }
            def Cube "box"
            {
                double size = 0.16
            }
        }
    }
    def DomeLight "sky"
    {
        float inputs:intensity = 0.5
        asset inputs:texture:file = @%(assets)s/sky_soft.exr@
        uniform token[] xformOpOrder = []
    }
    def RectLight "key"
    {
        float inputs:intensity%(key)s
        float inputs:width = 1.5
        float inputs:height = 0.6
        double3 xformOp:translate = (-3, 2.2, 0.5)
        float3 xformOp:rotateXYZ = (-35, -80, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
    def Camera "cam"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 20.25
        float2 clippingRange = (0.01, 10000)
        double shutter:open = -0.25
        double shutter:close = 0.25
        double3 xformOp:translate%(cam)s
        float3 xformOp:rotateXYZ = (-22, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
}

def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        int2 resolution = (160, 90)
        int nova:samples = %(spp)d
        rel products = </Render/Products/p>
    }
    def Scope "Products"
    {
        def RenderProduct "p"
        {
            token productName = "out.exr"
            rel orderedVars = [</Render/Vars/C>, </Render/Vars/N>, </Render/Vars/depth>]
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
        def RenderVar "N" (prepend apiSchemas = ["HuskRenderVarAPI"])
        {
            uniform token dataType = "normal3f"
            uniform string sourceName = "normal"
            uniform token sourceType = "raw"
            string driver:parameters:aov:husk:name = "N"
        }
        def RenderVar "depth" (prepend apiSchemas = ["HuskRenderVarAPI"])
        {
            uniform token dataType = "float"
            uniform string sourceName = "depth"
            uniform token sourceType = "raw"
            string driver:parameters:aov:husk:name = "depth"
        }
    }
}
'''


def samples(values):
    """An attribute's value: time samples from {frame: text}, or one text."""
    if isinstance(values, str):
        return " = " + values
    return ".timeSamples = {\n" + "".join("            %s: %s,\n" % kv for kv in values.items()) + "        }"


# Along its view direction, from 45 units to 2.5 units off the scene's centre.
CAMERA_MOVES = {1: "(0, 17.16, 41.72)", 2: "(0, 6.5, 15.3)", 3: "(0, 2.36, 5.1)", 4: "(0, 1.24, 2.32)", 5: "(0, 1.2, 2.2)"}
CAMERA_STAYS = "(0, 2.36, 5.1)"

ANIMATED = {
    "color": {1: "(0.8, 0.1, 0.1)", 2: "(0.1, 0.8, 0.1)", 4: "(0.1, 0.1, 0.8)"},
    "sheet": {1: "[(-2.6, 0.2, 1.4), (-1.6, 0.2, 1.4), (-1.6, 1.0, 1.0), (-2.6, 1.0, 1.0)]",
              2: "[(-2.6, 0.2, 1.4), (-1.6, 0.2, 1.4), (-1.6, 1.1, 1.1), (-2.6, 1.0, 1.0)]",
              3: "[(-2.6, 0.2, 1.4), (-1.6, 0.2, 1.4), (-1.6, 1.2, 1.2), (-2.6, 1.1, 1.0)]",
              4: "[(-2.6, 0.2, 1.4), (-1.6, 0.2, 1.4), (-1.6, 1.3, 1.3), (-2.6, 1.2, 1.0)]",
              5: "[(-2.6, 0.2, 1.4), (-1.6, 0.2, 1.4), (-1.6, 1.4, 1.4), (-2.6, 1.3, 1.0)]"},
    "ball": {1: "(-1.6, 0.55, 0.2)", 2: "(-1.3, 0.75, 0.2)", 3: "(-1.0, 0.55, 0.3)", 4: "(-0.7, 0.85, 0.4)",
             5: "(-0.4, 0.55, 0.5)"},
    "morph_counts": {1: "[4]", 3: "[3, 3]", 4: "[4, 4]"},
    "morph_indices": {1: "[0, 1, 2, 3]", 3: "[0, 1, 2, 0, 2, 3]", 4: "[0, 1, 2, 3, 3, 2, 4, 5]"},
    "morph_points": {1: "[(-2.6, 0.2, -1), (-1.6, 0.2, -1), (-1.6, 1.2, -1), (-2.6, 1.2, -1)]",
                     2: "[(-2.6, 0.2, -1), (-1.6, 0.2, -1), (-1.6, 1.4, -1), (-2.6, 1.3, -1)]",
                     3: "[(-2.6, 0.2, -1), (-1.6, 0.2, -1), (-1.6, 1.5, -1), (-2.6, 1.2, -1)]",
                     4: "[(-2.6, 0.2, -1), (-1.6, 0.2, -1), (-1.6, 1.2, -1), (-2.6, 1.2, -1), (-1.6, 1.9, -1.4), "
                        "(-2.6, 1.9, -1.4)]"},
    "blink": {1: '"inherited"', 3: '"invisible"', 4: '"inherited"'},
    "outer": {1: "[(-1.5, 0.3, 1.8), (0.8, 0.3, 1.9), (2.0, 0.3, 0.2)]",
              3: "[(-1.2, 0.4, 1.8), (0.9, 0.5, 1.9), (2.1, 0.3, 0.4)]",
              5: "[(-0.9, 0.3, 1.8), (1.0, 0.3, 1.9), (2.2, 0.6, 0.6)]"},
    "inner": {1: "[(-0.2, 0, 0), (0.2, 0, 0), (0, 0.25, 0.1), (0, 0, -0.25)]",
              5: "[(-0.3, 0, 0), (0.3, 0, 0), (0, 0.45, 0.1), (0, 0, -0.35)]"},
    "key": {1: "6", 3: "14", 5: "9"},
}
STILL = {name: next(iter(values.values())) for name, values in ANIMATED.items()}


def scene(animated, camera, spp):
    values = dict(ANIMATED if animated else STILL, cam=camera)
    text = {name: samples(value) for name, value in values.items()}
    return SCENE % dict(text, assets=ASSETS.replace("\\", "/"), spp=spp)


def read_parts(oiio, np, path):
    """Every subimage of an EXR as {name: float array}, or None."""
    image = oiio.ImageInput.open(path)
    if not image:
        return None
    parts = {}
    k = 0
    while image.seek_subimage(k, 0):
        spec = image.spec()
        parts[spec.getattribute("name") or str(k)] = np.asarray(image.read_image(k, 0, 0, spec.nchannels, oiio.FLOAT))
        k += 1
    image.close()
    return parts


def diced(log):
    """The triangles of every dicing the log reports (TF_DEBUG=HDNOVA_SUBDIV), per mesh in order."""
    counts = {}
    for path, triangles in re.findall(r"Nova: (\S+) displaced: (\d+) triangles", log):
        counts.setdefault(path.rsplit("/", 1)[-1], []).append(int(triangles))
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--renderers", nargs="*", default=["cpu", "xpu"])
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)
    env["TF_DEBUG"] = "HDNOVA_SUBDIV"
    plugins = {"cpu": "Nova", "xpu": "HdNovaXpuRendererPlugin"}
    files = {"moving": scene(True, CAMERA_MOVES, 8), "staying": scene(True, CAMERA_STAYS, 4),
             "still": scene(False, CAMERA_STAYS, 16)}
    for name, text in files.items():
        with open(os.path.join(args.out, name + ".usda"), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def husk(tag, name, first, count, label):
        """Renders `count` frames from `first`; returns the log, or None."""
        for k in range(first, first + count):
            exr = os.path.join(args.out, "%s_%s_%s.%04d.exr" % (name, tag, label, k))
            if os.path.exists(exr):
                os.remove(exr)
        proc = run([tool(args, "husk.exe"), "--renderer", plugins[tag], "--headlight", "none", "-s", "/Render/s",
                    "-f", str(first), "-n", str(count), "-o",
                    os.path.join(args.out, "%s_%s_%s.$F4.exr" % (name, tag, label)),
                    os.path.join(args.out, name + ".usda")], env, args.out)
        written = all(os.path.exists(os.path.join(args.out, "%s_%s_%s.%04d.exr" % (name, tag, label, k)))
                      for k in range(first, first + count))
        if proc.returncode != 0 or not written:
            check(False, f"husk {name} ({tag}) frames {first} to {first + count - 1} exits with 0 and writes its images")
            return None
        return proc.stderr + proc.stdout

    def image(tag, name, label, frame):
        return read_parts(oiio, np, os.path.join(args.out, "%s_%s_%s.%04d.exr" % (name, tag, label, frame)))

    try:
        for tag in args.renderers:
            # 1. The moving camera: a frame of the sequence is the frame alone.
            log = husk(tag, "moving", 1, 4, "seq")
            if log is None:
                continue
            if tag == "xpu" and ("rendering on the CPU" in log or "on the CPU\n" in log):
                print("skip xpu: Nova XPU does not render on this machine")
                continue
            sequence = diced(log)
            for frame in range(1, 5):
                alone_log = husk(tag, "moving", frame, 1, "one")
                if alone_log is None:
                    continue
                a, b = image(tag, "moving", "seq", frame), image(tag, "moving", "one", frame)
                same = set(a) == set(b) and all(np.array_equal(a[p].view(np.uint32), b[p].view(np.uint32)) for p in a)
                if not same:
                    for p in sorted(set(a) & set(b)):
                        diff = np.abs(a[p].astype(np.float64) - b[p].astype(np.float64))
                        print(f"     frame {frame} part {p}: {int((diff > 0).any(axis=-1).sum())} pixels differ, "
                              f"max {float(diff.max()):.4g}")
                check(same, f"moving camera ({tag}): frame {frame} of the sequence is the frame rendered alone")
                if frame == 1:
                    check(float(a["C"][..., :3].mean()) > 0.0 and float(np.abs(a["N"]).max()) > 0.5,
                          f"moving camera ({tag}): the beauty is not black and the normals are there")
                if frame == 4:
                    alone = diced(alone_log)
                    for mesh in ("plane", "cube", "sheet"):
                        check(sequence.get(mesh, [0])[-1] == alone.get(mesh, [-1])[-1],
                              f"moving camera ({tag}): {mesh} has the triangles of the near frame alone at the "
                              f"sequence's last frame ({sequence.get(mesh, [0])[-1]} / {alone.get(mesh, [-1])[-1]})")
            # Every mesh diced from the view once per frame (the sheet moves,
            # the plane and the cube do not), the plane and the cube with more
            # triangles as the camera comes closer (the sheet leaves the view
            # in the last frame and takes the off-screen quality); the mesh of
            # a fixed rate once in all.
            for mesh in ("plane", "cube"):
                counts = sequence.get(mesh, [])
                check(len(counts) == 4 and counts == sorted(counts) and counts[-1] > 4 * counts[0],
                      f"moving camera ({tag}): {mesh} is diced once per frame, finer as the camera approaches: {counts}")
            check(len(sequence.get("sheet", [])) == 4,
                  f"moving camera ({tag}): the deforming sheet is diced once per frame, not once more for the new "
                  f"view: {sequence.get('sheet')}")
            check(len(sequence.get("fixed", [])) == 1,
                  f"moving camera ({tag}): the mesh of a fixed dicing depth is diced once: {sequence.get('fixed')}")

            # 2. The camera stays: what does not move is diced once, the
            # deforming sheet once per frame.
            log = husk(tag, "staying", 1, 4, "seq")
            if log is not None:
                counts = diced(log)
                check(len(counts.get("plane", [])) == 1 and len(counts.get("cube", [])) == 1,
                      f"staying camera ({tag}): the plane and the cube are diced once: {counts.get('plane')}, "
                      f"{counts.get('cube')}")
                check(len(counts.get("sheet", [])) == 4,
                      f"staying camera ({tag}): the deforming sheet is diced once per frame: {counts.get('sheet')}")
                alone_log = husk(tag, "staying", 3, 1, "one")
                if alone_log is not None:
                    a, b = image(tag, "staying", "seq", 3), image(tag, "staying", "one", 3)
                    check(all(np.array_equal(a[p].view(np.uint32), b[p].view(np.uint32)) for p in a),
                          f"staying camera ({tag}): frame 3 of the sequence is the frame rendered alone")

            # 3. Nothing moves: the noise is another in every frame, around
            # the same image.
            if husk(tag, "still", 1, 2, "seq") is not None and husk(tag, "still", 2, 1, "one") is not None:
                a, b = image(tag, "still", "seq", 1)["C"], image(tag, "still", "seq", 2)["C"]
                differing = float((np.abs(a - b).max(axis=-1) > 0).mean())
                check(differing > 0.5, f"still scene ({tag}): frames 1 and 2 have different noise "
                                       f"({differing * 100:.0f} % of the pixels differ)")
                mean_a, mean_b = float(a[..., :3].mean()), float(b[..., :3].mean())
                check(abs(mean_a - mean_b) < 0.03 * mean_a,
                      f"still scene ({tag}): frames 1 and 2 show the same image ({mean_a:.4f} / {mean_b:.4f})")
                alone = image(tag, "still", "one", 2)["C"]
                check(np.array_equal(b.view(np.uint32), alone.view(np.uint32)),
                      f"still scene ({tag}): frame 2 has one noise pattern, in the sequence and alone")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
