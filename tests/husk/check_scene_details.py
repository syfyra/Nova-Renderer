"""husk checks of what Karma does with cameras, line lights, mesh lights and the details of shapes
(docs/bug-audit-2026-11.md, section 23), each against the values Karma CPU renders of the same file.

tests/scenes/camera_exposure.usda: the camera's exposure (its exposure
attribute, and time, ISO, f-stop and responsivity), karma:camera:tint and
clippingPlanes. Nova rendered all five cameras alike.

tests/scenes/line_light.usda: a CylinderLight with treatAsLine: a point light
at the line's middle, of the angle under which a point sees the line. Nova
rendered the cylinder of its radius.

tests/scenes/mesh_light.usda: UsdLux MeshLightAPI: the light of the mesh is
its material's glow times the light's colour and intensity. Nova rendered
the mesh of its material, the API unread.

tests/scenes/shape_details.usda: half-transparent spheres, points and tubes
are crossed twice as a cube is; one value of widths is every curve's width;
a polygon that is not flat is shaded by its face's normal; a normal per face
is used; a subdivision surface of holes alone renders nothing.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_scene_details.py ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)


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

    env, prefs = isolated_env(args)

    def read(path, part=None):
        """The image, or its part of that name (husk writes one per render var)."""
        inp = oiio.ImageInput.open(path)
        sub = 0
        while part is not None:
            if not inp.seek_subimage(sub, 0):
                inp.close()
                return None
            spec = inp.spec()
            if spec.get_string_attribute("name") == part or (spec.channelnames and spec.channelnames[0].startswith(part + ".")):
                break
            sub += 1
        px = inp.read_image(sub, 0, 0, inp.spec().nchannels, oiio.FLOAT)
        inp.close()
        return px

    def husk(renderer, scene, output, settings):
        if os.path.exists(output):
            os.remove(output)
        cmd = [tool(args, "husk.exe"), "--renderer", renderer, "--headlight", "none", "--settings", settings, "-o", output,
               os.path.join(args.scenes, scene)]
        proc = run(cmd, env, args.out)
        check(proc.returncode == 0 and os.path.exists(output), f"husk {scene} {settings} ({renderer}) writes its image")
        return output if os.path.exists(output) else None

    def block(px, col, row, half):
        return px[row - half:row + half, col - half:col + half].reshape(-1, px.shape[2]).mean(axis=0)

    def text(v):
        return "(" + ", ".join(f"{float(x):.4f}" for x in v) + ")"

    try:
        for renderer, label in (("Nova", "CPU"), ("Nova XPU", "XPU")):
            tag = label.lower()

            # 1. The camera: the image's mean per camera. Karma's plain mean, and its other cameras over
            # it; what the rule says: 2^1.5; a time of 0.5 x ISO 400 / 100 / f-stop 2 squared = 0.5; the
            # tint (1, 0.5, 0.25) x 2^1.
            means, images = {}, {}
            for name in ("plain", "exposed", "photo", "tinted", "clipped"):
                out = husk(renderer, "camera_exposure.usda", os.path.join(args.out, f"camera_{name}_{tag}.exr"), "/Render/" + name)
                if out:
                    images[name] = read(out, "C") if name == "tinted" else read(out)
                    means[name] = images[name][..., :3].reshape(-1, 3).mean(axis=0)
                    if name == "tinted":
                        images["direct"], images["N"] = read(out, "direct"), read(out, "N")
            if len(means) == 5:
                plain = means["plain"]
                check(abs(float(plain.mean()) - 0.27223) < 0.004, f"{label} camera: the plain image's mean {plain.mean():.5f} (Karma 0.27223)")
                for name, karma, rule in (("exposed", (2.8279,) * 3, (2.0 ** 1.5,) * 3), ("photo", (0.4999,) * 3, (0.5,) * 3),
                                          ("tinted", (1.9996, 0.9998, 0.4999), (2.0, 1.0, 0.5))):
                    ratio = means[name] / plain
                    check(all(abs(ratio[c] - rule[c]) < 0.002 * rule[c] and abs(ratio[c] - karma[c]) < 0.002 * rule[c] for c in range(3)),
                          f"{label} camera {name}: {text(ratio)} of the plain image (Karma {text(karma)}, the rule {text(rule)})")
                # The exposure is the beauty's and the light planes', not alpha's or a normal's.
                direct = images["direct"][..., :3].reshape(-1, 3).mean(axis=0)
                check(all(abs(direct[c] - k) < 0.02 * k for c, k in enumerate((0.50608, 0.25304, 0.12652))),
                      f"{label} camera tinted: its direct light {text(direct)} (Karma (0.5061, 0.2530, 0.1265))")
                a, b = float(images["tinted"][..., 3].mean()), float(images["plain"][..., 3].mean())
                check(abs(a - b) < 1e-6, f"{label} camera tinted: alpha as the plain camera's: {a:.5f} and {b:.5f}")
                n = float(np.abs(images["N"][..., :3]).max())
                check(n <= 1.0 + 1e-4, f"{label} camera tinted: the normals stay normals: at most {n:.4f}")
                # The clipping plane cuts off what lies left of x = -0.5 in the camera's space.
                clipped = images["clipped"]
                ratio = float(means["clipped"].mean() / plain.mean())
                alpha, left = float(clipped[..., 3].mean()), float(clipped[:, :50, 3].max())
                check(abs(ratio - 0.5975) < 0.012 and abs(alpha - 0.2517) < 0.004 and left < 1e-6,
                      f"{label} camera clipped: {ratio:.4f} of the plain image, alpha {alpha:.4f}, in the left third at most "
                      f"{left:.4f} (Karma 0.5975, 0.2517, 0)")

            # 2. The line light, 20 pixels per unit around the line's middle: blocks of 6 x 6 pixels at (x, z).
            out = husk(renderer, "line_light.usda", os.path.join(args.out, f"line_light_{tag}.exr"), "/Render/rs")
            if out:
                px = read(out)
                worst, at = 0.0, None
                points = (("under its middle", 0, 0, 2.04103), ("along it", -2.75, 0, 0.53703), ("across it", 0, 2, 1.09658),
                          ("a corner", -3, -2, 0.4031), ("beside the box", 1.8, -1.5, 0.83035), ("the box's shadow", 2.9, 1.0, 0.0),
                          ("the box's top", 1.8, 0.6, 0.82275))
                for name, x, z, karma in points:
                    got = float(block(px, int(80 + x * 20), int(60 + z * 20), 3)[:3].mean())
                    d = abs(got - karma) / (karma + 0.02)
                    if at is None or d > worst:
                        worst, at = d, (name, got, karma)
                check(worst < 0.01,
                      f"{label} line light: {len(points)} places within {worst * 100:.2f} % of Karma (the furthest {at[0]}: "
                      f"{at[1]:.5f}, Karma {at[2]:.5f})")

            # 3. The mesh light: the ground under a card of a red glow of 9 and a light of (0.6, 3, 0.6).
            # The material alone would light it with 9 (0.327 under the card), the light's colour alone green.
            out = husk(renderer, "mesh_light.usda", os.path.join(args.out, f"mesh_light_{tag}.exr"), "/Render/rs")
            if out:
                px = read(out)
                for name, x, z, karma in (("under the card", 0, 0, 0.19598), ("2 units off", 2, 0, 0.05509), ("a corner", -3, -2, 0.01227)):
                    got = block(px, int(80 + x * 20), int(60 + z * 20), 4)[:3]
                    check(abs(got[0] - karma) < 0.03 * karma + 0.0005 and got[1] < 1e-5 and got[2] < 1e-5,
                          f"{label} mesh light, {name}: {text(got)} (Karma ({karma:.4f}, 0, 0))")

            # 4. Details of shapes, 20 pixels per unit, cells of 3 units.
            out = husk(renderer, "shape_details.usda", os.path.join(args.out, f"shape_details_{tag}.exr"), "/Render/rs")
            if out:
                px, normals = read(out, "C"), read(out, "N")
                # Unlit shapes of opacity 0.5 over black: a ray crosses two surfaces, alpha 0.75 (one: 0.5).
                for name, x in (("sphere", -4.5), ("point", -1.5), ("tube", 1.5), ("cube", 4.5)):
                    got = block(px, int((x + 6) * 20), 30, 4)
                    check(abs(got[3] - 0.75) < 0.012 and all(abs(got[c] - k) < 0.012 for c, k in enumerate((0.225, 0.45, 0.675))),
                          f"{label} a {name} of opacity 0.5 is crossed twice: {text(got)} (Karma (0.2250, 0.4500, 0.6750, 0.7500))")
                # One value of widths, whatever its interpolation says: a ribbon of 0.4 x 2 units.
                covered = float(px[60:120, 0:60, 3].sum())
                check(abs(covered - 370.2) < 6.0, f"{label} one width for the whole curve: {covered:.1f} pixels (Karma 370.2)")
                # A saddle of one quad and a quad with a normal per face: Karma's normal plane and colour, and
                # one shade over the face (the dome's noise is 0.014 there; the fold of two triangles was 0.165).
                for name, x, karma_n, karma_c in (("a quad that is not flat has its face's normal", -1.5, (0.0, 1.0, 0.0), 0.4142),
                                                  ("a normal per face", 1.5, (0.6, 0.8, 0.0), 0.3339)):
                    col = int((x + 6) * 20)
                    n, c = block(normals, col, 90, 12)[:3], float(block(px, col, 90, 12)[0])
                    flat = float(px[78:102, col - 12:col + 12, 0].max() - px[78:102, col - 12:col + 12, 0].min())
                    check(all(abs(n[k] - karma_n[k]) < 0.004 for k in range(3)) and abs(c - karma_c) < 0.006 and flat < 0.03,
                          f"{label} {name}: N {text(n)}, colour {c:.4f}, within the face {flat:.4f} "
                          f"(Karma {text(karma_n)}, {karma_c:.4f})")
                holes = float(px[70:110, 180:240, 3].sum())
                check(holes < 1e-6, f"{label} a subdivision surface of holes alone renders nothing: alpha {holes:.4f}")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
