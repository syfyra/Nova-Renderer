"""husk check of light texture color spaces and texture diagnostics.

The same 8-bit texel (204, 102, 26) stored as PNG, TIFF and TGA lights a grey
ground (albedo 0.5) through a dome. Houdini's OCIO file rules decode all three
as sRGB, as Karma does, so each must give 0.5 * srgbToLinear(texel); a
half-float EXR holding the decoded values must give the same. As in Karma
(values measured with husk --renderer BRAY_HdKarma):
  * .tx files written by Houdini's imaketx are sRGB-encoded, also from a
    float EXR, so they render like their source image;
  * .rat files (Houdini's format, also of the HDRIs in $HFS/houdini/pic/hdri;
    OIIO cannot read them) render like their source image: float data as
    stored, 8-bit sRGB data decoded, a name a file rule matches applied to
    the stored values, under an ACES config converted to ACEScg;
  * 1-channel images are linear, even if a file rule says sRGB;
  * only real file rules count: color space names in directories or in file
    names of formats without a rule are ignored;
  * under an ACES config (scene_linear = ACEScg) every texture is converted
    into ACEScg, so EXR and PNG give 0.5 * Rec709ToACEScg(linear texel).
A missing texture must be reported on stderr, because husk does not print Tf
warnings.

Surface textures (M3.8): an unlit quad whose emission is a MaterialX image
node shows the decoded texel exactly (PNG, an imaketx .tx, half EXR, an
*ACEScg* EXR through the matrix, a UsdUVTexture with scale and bias); the
converted files land in nova:textureCacheDir and a second render reuses
them; a missing surface texture gives the node's default and one warning.

Needs OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_textures.py ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

TEXEL = (204, 102, 26)

SCENE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "ground"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-10, 0, -10), (-10, 0, 10), (10, 0, 10), (10, 0, -10)]
        color3f[] primvars:displayColor = [(0.5, 0.5, 0.5)]
        uniform token subdivisionScheme = "none"
    }

    def DomeLight "sky"
    {
        asset inputs:texture:file = @./TEXTURE@
        float inputs:intensity = 1
    }

    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 20
        float verticalAperture = 20
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 5, 0)
        float xformOp:rotateX = -90
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
    }
}

def Scope "Render"
{
    def RenderSettings "settings"
    {
        rel camera = </World/cam>
        int2 resolution = (16, 16)
        int nova:samples = 64
        int nova:maxDepth = 1
        token nova:pixelFilter = "box"
    }
}
"""


SURFACE_SCENE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "quad" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/Looks/mat>
    }

    def Scope "Looks"
    {
        def Material "mat"
        {
            token outputs:mtlx:surface.connect = </World/Looks/mat/surface.outputs:out>
            def Shader "surface"
            {
                uniform token info:id = "ND_surface_unlit"
                color3f inputs:emission_color.connect = </World/Looks/mat/IMAGE_OUTPUT>
                token outputs:out
            }
            def Shader "image"
            {
                uniform token info:id = "ND_image_color3"
                asset inputs:file = @./TEXTURE@
                color3f inputs:default = (0.25, 0.5, 0.75)
                color3f outputs:out
            }
            def Shader "uvtex"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./TEXTURE@
                float4 inputs:scale = (2, 2, 2, 1)
                float4 inputs:bias = (0.1, 0.1, 0.1, 0)
                float3 outputs:rgb
            }
        }
    }

    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 10
        float verticalAperture = 10
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}

def Scope "Render"
{
    def RenderSettings "settings"
    {
        rel camera = </World/cam>
        int2 resolution = (8, 8)
        int nova:samples = 4
        int nova:maxDepth = 1
        token nova:pixelFilter = "box"
        string nova:textureCacheDir = "CACHE_DIR"
    }
}
"""


def srgb_to_linear(v):
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


# Linear Rec.709 -> ACEScg (Bradford D65 -> D60), as in the ACES OCIO configs.
REC709_TO_ACESCG = ((0.6131, 0.3395, 0.0474), (0.0702, 0.9164, 0.0134), (0.0206, 0.1096, 0.8698))


def to_acescg(c):
    return [sum(m * v for m, v in zip(row, c)) for row in REC709_TO_ACESCG]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--scenes", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    linear = [srgb_to_linear(v / 255.0) for v in TEXEL]
    expected = [0.5 * c for c in linear]

    def write_texture(name, fmt, values, attributes=()):
        path = os.path.join(args.out, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        spec = oiio.ImageSpec(16, 8, len(values), fmt)
        for key, value in attributes:
            spec.attribute(key, value)
        out = oiio.ImageOutput.create(path)
        out.open(path, spec)
        out.write_image(np.tile(np.array(values, dtype=np.float32), (8, 16, 1)))
        out.close()

    for name in ("tex.png", "tex.tif", "tex.tga"):
        write_texture(name, oiio.UINT8, [v / 255.0 for v in TEXEL])
    # A PNG with a gAMA chunk and no sRGB chunk: OIIO reports "Gamma2.2",
    # which no config knows; Karma decodes it like any 8-bit PNG.
    write_texture("gamma.png", oiio.UINT8, [v / 255.0 for v in TEXEL], (("oiio:ColorSpace", "Gamma2.2"),))
    check(oiio.ImageInput.open(os.path.join(args.out, "gamma.png")).spec().get_string_attribute("oiio:ColorSpace")
          == "Gamma2.2", "the gAMA PNG reads back as Gamma2.2")
    write_texture("tex.exr", oiio.HALF, linear)

    renders = []

    def render(texture, render_env=None, tag=""):
        # Neutral output names: husk applies the OCIO file rules to the output
        # file too (an output named *srgb_tx* would be written sRGB-encoded).
        renders.append(texture + tag)
        stem = f"render{len(renders):02d}"
        scene = os.path.join(args.out, stem + ".usda")
        with open(scene, "w") as f:
            f.write(SCENE.replace("TEXTURE", texture))
        image = os.path.join(args.out, stem + ".exr")
        cmd = [tool(args, "husk.exe"), "--renderer", "Nova", "--settings", "/Render/settings", "-o", image, scene]
        proc = run(cmd, render_env or env, args.out)
        return proc, image

    def mean_of(texture, render_env=None, tag=""):
        proc, image = render(texture, render_env, tag)
        check(proc.returncode == 0, f"husk with a {texture} dome{tag} exits with 0")
        pixels = oiio.ImageBuf(image).get_pixels(oiio.FLOAT)
        return [float(pixels[..., c].mean()) for c in range(3)]

    def expect(name, got, wanted, tol=0.01):
        worst = max(abs(m - e) / max(abs(e), 1e-3) for m, e in zip(got, wanted))
        check(worst < tol, f"{name}: {[round(m, 5) for m in got]} vs {[round(e, 5) for e in wanted]}"
                           f" (max rel. deviation {worst:.4f})")

    env, prefs = isolated_env(args)
    try:
        means = {}
        for texture in ("tex.png", "tex.tif", "tex.tga", "tex.exr"):
            means[texture] = mean_of(texture)
            worst = max(abs(m - e) / e for m, e in zip(means[texture], expected))
            check(worst < 0.01, f"{texture} dome lights the ground like Karma: {[round(m, 5) for m in means[texture]]}"
                                f" vs {[round(e, 5) for e in expected]} (max rel. deviation {worst:.4f})")
        expect("a PNG tagged Gamma2.2 decodes like any PNG", mean_of("gamma.png"), means["tex.png"], 1e-4)
        same = max(abs(a - b) for a, b in zip(means["tex.png"], means["tex.tif"]))
        check(same < 1e-4, f"PNG and TIFF textures with the same texels render the same (max difference {same:.2g})")

        # imaketx writes sRGB-encoded .tx files without color space metadata,
        # also from linear float images; Karma decodes them as sRGB.
        hdr_texel = [4.0, 2.0, 0.5]
        write_texture("hdr_src.exr", oiio.HALF, hdr_texel)
        for src, dst in (("hdr_src.exr", "hdr.tx"), ("tex.png", "tex.tx")):
            proc = run([tool(args, "imaketx.exe"), os.path.join(args.out, src), os.path.join(args.out, dst)],
                       env, args.out)
            check(proc.returncode == 0, f"imaketx {src} {dst} exits with 0")
        expect("an imaketx .tx of a float EXR lights like the EXR", mean_of("hdr.tx"), [0.5 * v for v in hdr_texel])
        expect("an imaketx .tx of a PNG lights like the PNG", mean_of("tex.tx"), expected)

        # .rat files (Houdini's format, also of its HDRIs; OIIO cannot read
        # them) light as in Karma: float data as stored, 8-bit sRGB data
        # decoded, and a name a file rule matches applies to the stored values.
        # Before, the dome lost its texture and lit with its plain color.
        for src, dst in (("hdr_src.exr", "hdr.rat"), ("tex.png", "tex.rat")):
            proc = run([tool(args, "imaketx.exe"), os.path.join(args.out, src), os.path.join(args.out, dst)],
                       env, args.out)
            check(proc.returncode == 0, f"imaketx {src} {dst} exits with 0")
        shutil.copyfile(os.path.join(args.out, "hdr.rat"), os.path.join(args.out, "hdr_srgb_tx.rat"))
        expect("a float .rat lights like its EXR", mean_of("hdr.rat"), [0.5 * v for v in hdr_texel], 1e-4)
        expect("an 8-bit .rat lights like its PNG", mean_of("tex.rat"), expected, 2e-3)
        expect("a float .rat named *srgb_tx* is decoded from its stored values", mean_of("hdr_srgb_tx.rat"),
               [0.5 * srgb_to_linear(v) for v in hdr_texel], 1e-3)
        # Houdini's own HDRI (with a sun up to 3.5e5), against the same pixels
        # as an EXR (iconvert writes a float .rat's stored values) and the
        # ground they light, 0.5 E / pi, summed over the texels (+Y up). Its
        # camera-visible texels match Karma's exactly (docs/design/M3.md).
        kiara = os.path.join(args.hfs, "houdini", "pic", "hdri", "HDRIHaven_kiara_5_noon_2k.rat")
        if os.path.exists(kiara):
            shutil.copyfile(kiara, os.path.join(args.out, "kiara.rat"))
            proc = run([tool(args, "iconvert.exe"), kiara, os.path.join(args.out, "kiara.exr")], env, args.out)
            check(proc.returncode == 0, "iconvert of Houdini's kiara HDRI exits with 0")
            kiara_rat = mean_of("kiara.rat")
            expect("Houdini's kiara .rat HDRI lights like its EXR", kiara_rat, mean_of("kiara.exr"), 1e-4)
            texels = oiio.ImageBuf(os.path.join(args.out, "kiara.exr")).get_pixels(oiio.FLOAT)[..., :3]
            h, w = texels.shape[:2]
            theta = (np.arange(h) + 0.5) / h * np.pi
            weight = np.where(np.cos(theta) > 0, (2 * np.pi / w) * (np.pi / h) * np.sin(theta) * np.cos(theta), 0.0)
            irradiance = (texels.astype(np.float64) * weight[:, None, None]).sum(axis=(0, 1))
            expect("Houdini's kiara .rat HDRI lights the ground with its texels' irradiance", kiara_rat,
                   [0.5 * float(e) / np.pi for e in irradiance], 3e-3)
        else:
            check(False, f"Houdini's HDRI not found: {kiara}")

        # 1-channel images are linear, also where a file rule says sRGB.
        grey = 153 / 255.0
        write_texture("grey1.png", oiio.UINT8, [grey])
        write_texture("grey1_srgb_tx.png", oiio.UINT8, [grey])
        expect("a 1-channel PNG is linear", mean_of("grey1.png"), [0.5 * grey] * 3)
        expect("a 1-channel PNG named *srgb_tx* is linear", mean_of("grey1_srgb_tx.png"), [0.5 * grey] * 3)

        # Only real file rules: names in directories or in .hdr file names
        # (no rule of their own) do not pick a color space.
        sky = [0.8, 0.4, 0.1]
        for name in ("plain/sky.hdr", "acescct/sky.hdr", "hdri_acescg/sky.hdr", "sky_acescg.hdr"):
            write_texture(name, oiio.FLOAT, sky)
        plain = mean_of("plain/sky.hdr")
        stored = oiio.ImageBuf(os.path.join(args.out, "plain", "sky.hdr")).get_pixels(oiio.FLOAT)
        expect("a linear .hdr lights linearly", plain, [0.5 * float(stored[..., c].mean()) for c in range(3)])
        for name in ("acescct/sky.hdr", "hdri_acescg/sky.hdr", "sky_acescg.hdr"):
            expect(f"{name} ignores the color space name in its path", mean_of(name), plain, 1e-4)

        # An ACES config converts every texture into its scene_linear, ACEScg.
        config = os.path.join(args.hfs, "packages", "ocio", "cg-config-v3.0.0_aces-v2.0_ocio-v2.5.ocio")
        if os.path.exists(config):
            aces = dict(env, OCIO=config.replace("\\", "/"))
            converted = [0.5 * v for v in to_acescg(linear)]
            expect("ACES config: an EXR texture is converted to ACEScg", mean_of("tex.exr", aces, "_aces"), converted)
            expect("ACES config: a PNG texture is decoded and converted to ACEScg", mean_of("tex.png", aces, "_aces"),
                   converted)
            expect("ACES config: a PNG tagged Gamma2.2 is decoded and converted like any PNG",
                   mean_of("gamma.png", aces, "_aces"), converted)
            expect("ACES config: a .hdr texture is converted like an EXR", mean_of("plain/sky.hdr", aces, "_aces"),
                   to_acescg(plain), 2e-3)
            # Karma converts a .rat's stored lin_rec709 data as well.
            expect("ACES config: a float .rat is converted to ACEScg", mean_of("hdr.rat", aces, "_aces"),
                   [0.5 * v for v in to_acescg(hdr_texel)], 2e-3)
            expect("ACES config: an 8-bit .rat is decoded and converted like its PNG", mean_of("tex.rat", aces, "_aces"),
                   converted, 3e-3)
        else:
            check(False, f"ACES config not found: {config}")

        proc, _ = render("missing.exr")
        check("[Nova] Warning: cannot open texture" in proc.stderr,
              "a missing texture is reported on stderr: " + proc.stderr.strip()[:200])

        # ---- surface textures (MaterialX image, UsdUVTexture) ----
        cache = os.path.join(args.out, "txcache")
        shutil.rmtree(cache, ignore_errors=True)

        def surface(texture, output="image.outputs:out", render_env=None):
            renders.append("surface " + texture + " " + output)
            stem = f"render{len(renders):02d}"
            scene = os.path.join(args.out, stem + ".usda")
            with open(scene, "w") as f:
                f.write(SURFACE_SCENE.replace("TEXTURE", texture).replace("IMAGE_OUTPUT", output)
                        .replace("CACHE_DIR", cache.replace("\\", "/")))
            image = os.path.join(args.out, stem + ".exr")
            cmd = [tool(args, "husk.exe"), "--renderer", "Nova", "--settings", "/Render/settings", "-o", image, scene]
            proc = run(cmd, render_env or env, args.out)
            check(proc.returncode == 0, f"husk with a {texture} surface texture exits with 0")
            pixels = oiio.ImageBuf(image).get_pixels(oiio.FLOAT)
            # The quad covers the 4x4 center pixels.
            return proc, [float(pixels[2:6, 2:6, c].mean()) for c in range(3)]

        for texture in ("tex.png", "tex.tx", "tex.exr"):
            _, got = surface(texture)
            expect(f"an image node shows the decoded {texture} texel", got, linear, 2e-3)
        acescg = [0.2, 0.5, 0.7]
        write_texture("surf_ACEScg.exr", oiio.HALF, acescg)
        aces_to_709 = ((1.7051, -0.6218, -0.0833), (-0.1303, 1.1408, -0.0105), (-0.0240, -0.1290, 1.1530))
        _, got = surface("surf_ACEScg.exr")
        expect("an *ACEScg* EXR reaches Linear Rec.709 through the matrix", got,
               [sum(m * v for m, v in zip(row, acescg)) for row in aces_to_709], 3e-3)
        _, got = surface("tex.png", "uvtex.outputs:rgb")
        expect("a UsdUVTexture applies scale and bias", got, [2 * v + 0.1 for v in linear], 2e-3)
        _, got = surface("hdr.rat")
        expect("an image node shows a float .rat's texel", got, hdr_texel, 1e-4)
        if os.path.exists(config):
            # Karma converts a .rat into the rendering space as any file
            # (before, Nova used its lin_rec709 data as ACEScg).
            _, got = surface("hdr.rat", render_env=aces)
            expect("ACES config: an image node converts a float .rat to ACEScg", got, to_acescg(hdr_texel), 2e-3)
        converted = sorted(os.listdir(cache)) if os.path.isdir(cache) else []
        check(len(converted) >= 3, f"untiled and sRGB textures are converted into the cache: {converted}")
        times = {f: os.path.getmtime(os.path.join(cache, f)) for f in converted}
        surface("tex.png")
        check(all(os.path.getmtime(os.path.join(cache, f)) == t for f, t in times.items()),
              "a second render reuses the converted files")
        proc, got = surface("missing_surface.png")
        expect("a missing surface texture gives the image's default", got, [0.25, 0.5, 0.75], 1e-4)
        check(proc.stderr.count("cannot read texture") == 1,
              "a missing surface texture is reported once on stderr: " + proc.stderr.strip()[:300])
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
