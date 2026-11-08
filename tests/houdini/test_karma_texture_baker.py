"""The Karma Texture Baker LOP baked by Nova through husk (docs/design/M11-P8.md;
ctest houdini_karma_texture_baker): builds the LOP on a box (low, UVs over
two UDIM tiles) and a subdivided box (high), checks that it still authors
the bake settings Nova reads, saves the stage and renders both tiles' render
settings with husk and Nova into the LOP's PNG products: the products exist,
the tangent-space normals are remapped to [0, 1] and point along the low
normal, the islands are expanded into every texel, the occlusion and the
alpha are plausible.

Usage (isolated): run_hython.ps1 -Script test_karma_texture_baker.py -Stage <stage> <outdir>
"""

import os
import subprocess
import sys

import hou

OUT = os.path.abspath(sys.argv[1])
os.makedirs(OUT, exist_ok=True)
FAILURES = []


def check(cond, message):
    print(("ok   " if cond else "FAIL ") + message, flush=True)
    if not cond:
        FAILURES.append(message)


stage = hou.node("/stage")
for child in stage.children():
    child.destroy()


def sop_mesh(name, build):
    lop = stage.createNode("sopcreate", name)
    lop.parm("primpath").set("/" + name)
    net = lop.node("sopnet/create")
    last = build(net)
    out = net.createNode("output")
    out.setInput(0, last)
    out.setDisplayFlag(True)
    out.setRenderFlag(True)
    return lop


def low(net):
    box = net.createNode("box")
    uv = net.createNode("uvflatten")
    uv.setInput(0, box)
    tile = net.createNode("attribwrangle")
    tile.parm("class").set("vertex")
    tile.parm("snippet").set("if (@primnum > 2) v@uv.x += 1;")  # faces 3-5 into UDIM 1002
    tile.setInput(0, uv)
    nrm = net.createNode("normal")
    nrm.setInput(0, tile)
    return nrm


def high(net):
    box = net.createNode("box")
    sub = net.createNode("subdivide")
    sub.parm("iterations").set(2)
    sub.setInput(0, box)
    return sub


lo = sop_mesh("low", low)
hi = sop_mesh("high", high)
merge = stage.createNode("merge")
merge.setInput(0, lo)
merge.setInput(1, hi)
merge.cook(force=True)
mesh = [str(p.GetPath()) for p in merge.stage().Traverse() if p.GetTypeName() == "Mesh"
        and str(p.GetPath()).startswith("/low")][0]

baker = stage.createNode("karmatexturebaker")
baker.setInput(0, merge)
baker.parm("bakemesh").set(mesh)
baker.parm("picture").set(os.path.join(OUT, "bake_<UDIM>.png").replace("\\", "/"))
baker.parm("resolutionx").set(64)
baker.parm("resolutiony").set(64)
for aov in ("Oc", "N"):
    baker.parm("aov_" + aov).set(1)
baker.cook(force=True)
check(not baker.errors(), "the Texture Baker cooks without errors: %s" % (baker.errors(),))
layer = baker.stage().Flatten()
usd = os.path.join(OUT, "bake.usda")
layer.Export(usd)

stage_usd = baker.stage()
settings = stage_usd.GetPrimAtPath("/Render/karmabake_1001")
check(settings.IsValid(), "the LOP authors /Render/karmabake_1001")
check(stage_usd.GetPrimAtPath("/Render/karmabake_1002").IsValid(), "and /Render/karmabake_1002 for the second tile")
for name in ("bake_mode", "bake_low", "bake_uv", "bake_udim", "bake_use_mikkt", "bake_trace_bias", "bake_trace_max",
             "bake_oc_samples", "bake_cu_sdist", "bake_cu_bias", "bake_pbox_mode", "imagefilter", "samplesperpixel"):
    check(settings.GetAttribute("karma:global:" + name).IsValid(), "karma:global:%s is authored" % name)
check(settings.GetAttribute("karma:global:bake_low").Get() == mesh, "bake_low names the low mesh")
low_prim = stage_usd.GetPrimAtPath(mesh)
vis = low_prim.GetAttribute("primvars:karma:object:rendervisibility")
check(vis.IsValid() and vis.Get() == "", "the low mesh is hidden from every ray")

env = dict(os.environ)
husk = os.path.join(os.environ["HFS"], "bin", "husk.exe")
import OpenImageIO as oiio  # noqa: E402  (Houdini's)
import numpy as np  # noqa: E402

for udim in (1001, 1002):
    proc = subprocess.run([husk, "--renderer", "Nova", "-s", "/Render/karmabake_%d" % udim, usd], env=env,
                          capture_output=True, text=True, cwd=OUT)
    check(proc.returncode == 0, "husk Nova bakes UDIM %d (exit %d)" % (udim, proc.returncode))
    if proc.returncode:
        print(proc.stdout[-2000:], proc.stderr[-2000:])
    images = {}
    for var in ("Nt", "Af", "Oc", "N"):
        path = os.path.join(OUT, "bake_%d_%s.png" % (udim, var))
        check(os.path.exists(path), "product %s" % os.path.basename(path))
        if os.path.exists(path):
            buf = oiio.ImageBuf(path)
            images[var] = np.asarray(buf.get_pixels(oiio.FLOAT)).reshape(buf.spec().height, buf.spec().width, -1)
    if len(images) < 4:
        continue
    af = images["Af"][..., 0]
    covered = af > 0.99
    check(covered.mean() > 0.2, "UDIM %d: the islands cover %.0f %% of the tile" % (udim, 100 * covered.mean()))
    nt = images["Nt"]
    if covered.any():
        mean = nt[covered].mean(axis=0)
        check(abs(mean[0] - 0.5) < 0.08 and abs(mean[1] - 0.5) < 0.08 and mean[2] > 0.75,
              "UDIM %d: Nt remapped to [0, 1], along the low normal (mean %s)" % (udim, mean[:3]))
        oc = images["Oc"][covered][..., 0].mean()
        check(0.6 < oc <= 1.0, "UDIM %d: occlusion of a convex box %.3f" % (udim, oc))
    check(float(nt[..., 2].min()) > 0.3, "UDIM %d: every texel of Nt is filled (min blue %.3f)" % (udim, nt[..., 2].min()))

print("%d failure(s)" % len(FAILURES))
sys.exit(1 if FAILURES else 0)
