"""husk checks of emissive meshes treated as lights against Karma
(docs/design/M9.md D14; research docs/research/m9/lights.md 3.3, 7.2).

The scene is the research's "pair" (probes/lt_karma_emit.py), written here
as plain USDA: a grey diffuse ground seen from above, an emissive polygon
sphere (32 x 16, radius 0.5, centre 1 above the ground) and an emissive quad
facing away from the ground, both standard_surface with base 0, specular 0,
emission 1, colour (2, 1, 0.5), and no lights. Nova renders it through husk
with karma:object:treat_as_lightsource 0 / 1 / 2 and the light multipliers;
red-channel region means are compared with Karma CPU's measured values:
- treated or not, the ground gets the same light (the untreated render is
  noisier and needs more samples); the emitters are CO untreated, CL treated,
  and the ground's light C<RD>O or C<RD>L;
- emission is two-sided (the quad faces away from the ground);
- lightsource_diffusescale scales diffuse receivers (2x, 0), the specular
  scale glossy ones, never the camera's view;
- Auto (2) treats every emitter, a noise-driven emission too;
- at 16 samples the far ground's relative noise drops at least 5x.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_emitters.py ...
"""

import argparse
import math
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

PX = 15  # pixels per unit (8 x 8 units)
RES = 8 * PX


def region(x, z, h):
    """Pixel slices of the square [x-h, x+h] x [z-h, z+h] (row = z, image top = -z)."""
    c0, c1 = int(round((x - h + 4) * PX)), int(round((x + h + 4) * PX))
    r0, r1 = int(round((z - h + 4) * PX)), int(round((z + h + 4) * PX))
    return slice(r0, max(r1, r0 + 1)), slice(c0, max(c1, c0 + 1))


REGIONS = {
    "sphere": region(-1.5, 0.0, 0.2), "gnd_sph": region(-1.5, 0.95, 0.12), "gnd_sph_far": region(-1.5, 2.2, 0.15),
    "quad": region(1.5, 0.0, 0.2), "gnd_quad": region(1.5, 0.95, 0.12), "gnd_quad_far": region(1.5, 2.2, 0.15),
}
GROUND = ["gnd_sph", "gnd_sph_far", "gnd_quad", "gnd_quad_far"]


def sphere_mesh(name, c, r, material, prims, nu=32, nv=16):
    pts = [(c[0], c[1] + r, c[2])]
    for j in range(1, nv):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            pts.append((c[0] + r * math.sin(th) * math.cos(ph), c[1] + r * math.cos(th),
                        c[2] + r * math.sin(th) * math.sin(ph)))
    pts.append((c[0], c[1] - r, c[2]))
    ring = lambda j, i: 1 + (j - 1) * nu + (i % nu)  # noqa: E731
    faces = [[0, ring(1, i + 1), ring(1, i)] for i in range(nu)]
    for j in range(1, nv - 1):
        for i in range(nu):
            faces.append([ring(j, i), ring(j, i + 1), ring(j + 1, i + 1), ring(j + 1, i)])
    last = len(pts) - 1
    faces += [[ring(nv - 1, i), ring(nv - 1, i + 1), last] for i in range(nu)]
    out = []  # every face outwards (right-handed winding)
    for f in faces:
        p0, p1, p2 = (pts[k] for k in f[:3])
        e1 = [p1[a] - p0[a] for a in range(3)]
        e2 = [p2[a] - p0[a] for a in range(3)]
        n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        out.append(f if sum(n[a] * (p0[a] - c[a]) for a in range(3)) > 0 else f[::-1])
    return '''
    def Mesh "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [%s]
        int[] faceVertexIndices = [%s]
        point3f[] points = [%s]
        float3[] extent = [(%g, %g, %g), (%g, %g, %g)]
        rel material:binding = </m/%s>%s
    }''' % (name, ", ".join(str(len(f)) for f in out), ", ".join(str(k) for f in out for k in f),
            ", ".join("(%.6f, %.6f, %.6f)" % p for p in pts), c[0] - r, c[1] - r, c[2] - r, c[0] + r, c[1] + r,
            c[2] + r, material, prims)


def quad_mesh(name, c, s, material, prims):
    h = s / 2
    x, y, z = c
    return '''
    def Mesh "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(%g, %g, %g), (%g, %g, %g), (%g, %g, %g), (%g, %g, %g)]
        float3[] extent = [(%g, %g, %g), (%g, %g, %g)]
        rel material:binding = </m/%s>%s
    }''' % (name, x - h, y, z + h, x + h, y, z + h, x + h, y, z - h, x - h, y, z - h, x - h, y, z - h, x + h, y,
            z + h, material, prims)


def materials(ground_spec):
    return '''
def Scope "m"
{
    def Material "grey"
    {
        token outputs:mtlx:surface.connect = </m/grey/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.5, 0.5, 0.5)
            float inputs:specular = %g
            float inputs:specular_roughness = 0.3
            token outputs:out
        }
    }
    def Material "emit"
    {
        token outputs:mtlx:surface.connect = </m/emit/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 0
            float inputs:specular = 0
            float inputs:emission = 1
            color3f inputs:emission_color = (2, 1, 0.5)
            token outputs:out
        }
    }
    def Material "noisy"
    {
        token outputs:mtlx:surface.connect = </m/noisy/s.outputs:out>
        def Shader "n"
        {
            uniform token info:id = "ND_noise3d_color3"
            color3f inputs:amplitude = (1, 1, 1)
            float inputs:pivot = 1
            color3f outputs:out
        }
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 0
            float inputs:specular = 0
            float inputs:emission = 1
            color3f inputs:emission_color.connect = </m/noisy/n.outputs:out>
            token outputs:out
        }
    }
}
''' % ground_spec


def prims(treat, dscale=1.0, sscale=1.0):
    return '''
        int primvars:karma:object:treat_as_lightsource = %d
        float primvars:karma:object:lightsource_diffusescale = %g
        float primvars:karma:object:lightsource_specularscale = %g''' % (treat, dscale, sscale)


LPES = ["CO", "CL", "C<RD>O", "C<RD>L", "C<R[GS]>O", "C<R[GS]>L"]


def scene(treat, dscale, sscale, gspec, spp, seed=0, sphere_material="emit"):
    body = sphere_mesh("glow_sphere", (-1.5, 1.0, 0.0), 0.5, sphere_material, prims(treat, dscale, sscale)) + \
        quad_mesh("glow_quad", (1.5, 1.0, 0.0), 1.0, "emit", prims(treat, dscale, sscale))
    vars_ = [("C", "C.*[LO]", "color4f")] + [("L%d" % i, e, "color3f") for i, e in enumerate(LPES)]
    vdefs = "".join('''
        def RenderVar "%s"
        {
            uniform token dataType = "%s"
            uniform string sourceName = "%s"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "%s"
            string driver:parameters:aov:name = "%s"
        }''' % (n, d, s, d, n) for n, s, d in vars_)
    order = ", ".join("</Render/Vars/%s>" % n for n, s, d in vars_)
    return '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{
    def Mesh "ground" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-4, 0, 4), (4, 0, 4), (4, 0, -4), (-4, 0, -4)]
        normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (interpolation = "vertex")
        float3[] extent = [(-4, 0, -4), (4, 0, 4)]
        rel material:binding = </m/grey>
    }%s
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 80
        float verticalAperture = 80
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 10, 0)
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
}
%s
def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        int2 resolution = (%d, %d)
        rel products = [</Render/Products/p>]
        token nova:pixelFilter = "box"
        float nova:pixelFilterScale = 0
        int nova:samples = %d
        int nova:seed = %d
        float nova:clampIndirect = 0
    }
    def Scope "Products"
    {
        def RenderProduct "p"
        {
            token productName = "out.exr"
            rel orderedVars = [%s]
        }
    }
    def Scope "Vars"
    {%s
    }
}
''' % (body, materials(gspec), RES, RES, spp, seed, order, vdefs)


# Karma CPU's means (research probes/lt_out_karma_emit.txt): the treated
# ground (identical for 1 and 2) and its glossy part.
KARMA_T1 = {"gnd_sph": 0.09981, "gnd_sph_far": 0.01851, "gnd_quad": 0.10207, "gnd_quad_far": 0.01466}
KARMA_T1_D2 = {"gnd_sph": 0.20086, "gnd_sph_far": 0.03808, "gnd_quad": 0.20486, "gnd_quad_far": 0.02950}

# name -> (treat, diffuse scale, specular scale, ground specular, samples, seed)
CONFIGS = {
    "t0": (0, 1, 1, 0.0, 1024, 0), "t1": (1, 1, 1, 0.0, 256, 0), "t2": (2, 1, 1, 0.0, 256, 0),
    "t1_d2": (1, 2, 1, 0.0, 256, 0), "t1_d0": (1, 0, 1, 0.0, 64, 0), "t1_spec": (1, 1, 1, 1.0, 512, 0),
    "t1_spec_s0": (1, 1, 0, 1.0, 256, 0), "t0_n16": (0, 1, 1, 0.0, 16, 7), "t1_n16": (1, 1, 1, 0.0, 16, 7),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)

    def render(name, text):
        usd = os.path.join(args.out, name + ".usda")
        with open(usd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        exr = os.path.join(args.out, name + ".exr")
        proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "--headlight", "none", "-s", "/Render/s", "-o", exr,
                    usd], env, args.out)
        check(proc.returncode == 0, f"husk {name}.usda exits with 0")
        image = oiio.ImageInput.open(exr)
        if not image:
            check(False, f"{name}: no image")
            return {}
        layers = {}
        part = 0
        while image.seek_subimage(part, 0):
            spec = image.spec()
            px = np.asarray(image.read_image(part, 0, 0, spec.nchannels, oiio.FLOAT)).reshape(spec.height, spec.width, -1)
            layers[spec.getattribute("name")] = px[..., 0]
            part += 1
        image.close()
        return layers

    def mean(layers, layer, where):
        r, c = REGIONS[where]
        return float(layers[layer][r, c].mean()) if layer in layers else float("nan")

    def near(value, expected, rel, absolute=2e-4):
        return abs(value - expected) <= rel * abs(expected) + absolute

    img = {name: render(name, scene(*cfg[:4], spp=cfg[4], seed=cfg[5])) for name, cfg in CONFIGS.items()}
    lpe = {e: "L%d" % i for i, e in enumerate(LPES)}

    # Treated (1 and Auto) ground against Karma; the emitters are CL.
    for name in ("t1", "t2"):
        for where, karma in KARMA_T1.items():
            v = mean(img[name], "C", where)
            check(near(v, karma, 0.03), f"{name} ground {where}: Nova {v:.5f}, Karma {karma:.5f}")
            v = mean(img[name], lpe["C<RD>L"], where)
            check(near(v, karma, 0.03), f"{name} C<RD>L {where}: Nova {v:.5f}, Karma {karma:.5f}")
            check(mean(img[name], lpe["C<RD>O"], where) == 0.0, f"{name} C<RD>O {where} is 0")
        for where in ("sphere", "quad"):
            v, o = mean(img[name], lpe["CL"], where), mean(img[name], lpe["CO"], where)
            check(near(v, 2.0, 1e-3) and o == 0.0, f"{name} camera on the {where}: CL {v:.5f} (2), CO {o:.5f} (0)")
    # Untreated: the same light as O events (near within 3 %, far within 8 %
    # at 1024 samples: it converges slowly).
    for where, karma in KARMA_T1.items():
        v = mean(img["t0"], lpe["C<RD>O"], where)
        tol = 0.08 if where.endswith("far") else 0.03
        check(near(v, karma, tol), f"t0 C<RD>O {where}: Nova {v:.5f}, Karma (treated) {karma:.5f}")
        check(mean(img["t0"], lpe["C<RD>L"], where) == 0.0, f"t0 C<RD>L {where} is 0")
    for where in ("sphere", "quad"):
        v = mean(img["t0"], lpe["CO"], where)
        check(near(v, 2.0, 1e-3), f"t0 camera on the {where}: CO {v:.5f} (2)")
    # Multipliers: diffuse 2 doubles the ground, 0 removes it; the camera's
    # view stays 2.
    for where, karma in KARMA_T1_D2.items():
        v = mean(img["t1_d2"], "C", where)
        check(near(v, karma, 0.03), f"t1_d2 ground {where}: Nova {v:.5f}, Karma {karma:.5f}")
    for where in GROUND:
        v = mean(img["t1_d0"], "C", where)
        check(v == 0.0, f"t1_d0 ground {where}: {v:.5f} (0)")
    for name in ("t1_d2", "t1_d0"):
        v = mean(img[name], "C", "sphere")
        check(near(v, 2.0, 1e-3), f"{name} camera on the sphere: {v:.5f} (2)")
    # Glossy ground: C<R[GS]>L 0.0020 next to the emitters (Karma), 0 with
    # specular scale 0, while C<RD>L stays (0.0963 with scale 0).
    for where in ("gnd_sph", "gnd_quad"):
        v = mean(img["t1_spec"], lpe["C<R[GS]>L"], where)
        check(near(v, 0.0020, 0.2, 2e-4), f"t1_spec C<R[GS]>L {where}: Nova {v:.5f}, Karma 0.0020")
        v = mean(img["t1_spec_s0"], lpe["C<R[GS]>L"], where)
        check(v == 0.0, f"t1_spec_s0 C<R[GS]>L {where}: {v:.5f} (0)")
    v = mean(img["t1_spec_s0"], lpe["C<RD>L"], "gnd_sph")
    check(near(v, 0.0963, 0.03), f"t1_spec_s0 C<RD>L gnd_sph: Nova {v:.5f}, Karma 0.0963")
    # Noise at 16 samples: the far ground's relative pixel deviation.
    for where in ("gnd_sph_far", "gnd_quad_far"):
        r, c = REGIONS[where]
        rel = []
        for name in ("t0_n16", "t1_n16"):
            px = img[name]["C"][r, c] if "C" in img[name] else np.zeros(1)
            rel.append(float(px.std() / px.mean()) if px.mean() > 0 else float("inf"))
        check(rel[1] * 5.0 < rel[0], f"rel. std at 16 samples {where}: untreated {rel[0]:.3f}, treated {rel[1]:.3f}"
              " (Karma: 3.13 / 0.26 sphere, 2.14 / 0.12 quad)")

    # Auto on a noise-driven emission: a light too (CL, not CO).
    layers = render("auto_noisy", scene(2, 1, 1, 0.0, 64, sphere_material="noisy"))
    cl, co = mean(layers, lpe["CL"], "sphere"), mean(layers, lpe["CO"], "sphere")
    check(cl > 0.0 and co == 0.0, f"auto_noisy camera on the sphere: CL {cl:.5f} (> 0), CO {co:.5f} (0)")

    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
