"""husk checks of the render settings added in 0.2.0, by what each does to an image
(docs/bug-audit-2026-11.md, section 18; tests/scenes/settings.usda), on the CPU and on Nova XPU:

  nova:sampleScale        a quarter of the samples
  nova:timeLimit          a frame that would never end ends, with the samples it has
  nova:animateNoise       off: two frames of a still scene are the same image
  nova:lightOverride      the headlight and the dome instead of the stage's lights
  nova:simplifiedShading  display colours by their facing, the fog by distance, occlusion
  nova:outline            lines in the beauty of a render without an outline AOV
  nova:object:holdoutMode, nova:object:renderVisibility
                          on the render settings (every object) and on a prim
  nova:denoise            on a stage without render vars (husk's own beauty), with Open Image Denoise

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_settings.py ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

SCENE = "settings.usda"


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

    def husk(renderer, name, tag, extra=(), suffix=""):
        """Renders /Render/<name>; returns (pixels or None, the samples the image says it has, husk's output)."""
        output = os.path.join(args.out, f"{name}{suffix}_{tag}.exr")
        if os.path.exists(output):
            os.remove(output)
        cmd = [tool(args, "husk.exe"), "--renderer", renderer, "--settings", "/Render/" + name, "-o", output,
               *extra, os.path.join(args.scenes, SCENE)]
        proc = run(cmd, env, args.out)
        check(proc.returncode == 0 and os.path.exists(output), f"husk {name} ({renderer}) writes its image")
        if not os.path.exists(output):
            return None, -1, proc.stdout + proc.stderr
        inp = oiio.ImageInput.open(output)
        samples = inp.spec().getattribute("samples")
        px = inp.read_image(0, 0, 0, 4, oiio.FLOAT)
        inp.close()
        return px, -1 if samples is None else int(samples), proc.stdout + proc.stderr

    # Regions (rows from the top, 20 pixels per unit): the ground far from everything, the cube's top, where the
    # hidden cube stands and the same place on the other side, the ground next to the cube.
    corner, top = (slice(5, 25), slice(135, 155)), (slice(45, 75), slice(65, 95))
    ghost, mirrored = (slice(93, 107), slice(13, 27)), (slice(93, 107), slice(133, 147))
    beside = (slice(50, 70), slice(102, 108))

    def mean(px, region):
        return px[region[0], region[1], :3].reshape(-1, 3).mean(axis=0)

    try:
        for renderer, label in (("Nova", "CPU"), ("Nova XPU", "XPU")):
            tag = label.lower()

            # ---- The stage as it is: the hidden cube is not seen and shadows the ground.
            base, samples, _ = husk(renderer, "base", tag)
            if base is not None:
                check(samples == 64, f"{label}: 64 samples: {samples}")
                g, m = mean(base, ghost), mean(base, mirrored)
                check(abs(g[0] - g[1]) < 0.02 and float(base[ghost[0], ghost[1], 3].min()) > 0.99,
                      f"{label}: a prim's nova:object:renderVisibility -primary hides it from the camera: {g.round(3)}")
                check(g[1] < 0.8 * m[1], f"{label}: the hidden cube shadows the ground: {g[1]:.3f} against {m[1]:.3f}")
                t = mean(base, top)
                check(t[1] > 1.5 * t[0], f"{label}: the cube's top is green: {t.round(3)}")

            # ---- nova:sampleScale.
            _, samples, _ = husk(renderer, "scale", tag)
            check(samples == 16, f"{label} nova:sampleScale 0.25 of 64 samples: {samples}")

            # ---- nova:timeLimit: 1.8 seconds of a million samples.
            px, samples, log = husk(renderer, "limit", tag)
            check(0 < samples < 1000000, f"{label} nova:timeLimit ends the frame early: {samples} samples")
            check("time limit" in log, f"{label} nova:timeLimit says so")
            if px is not None:
                check(abs(float(mean(px, corner)[0]) - 1.0) < 0.05,
                      f"{label} nova:timeLimit: the image is the scene's: {float(mean(px, corner)[0]):.3f} on the ground")

            # ---- nova:animateNoise: frames 1 and 2 of a still scene.
            frames = {}
            for name in ("still", "moving"):
                for frame in (1, 2):
                    frames[name, frame], _, _ = husk(renderer, name, tag, ["--frame", str(frame)], f"_f{frame}")
            if all(v is not None for v in frames.values()):
                still = float(np.abs(frames["still", 1] - frames["still", 2]).max())
                moving = float(np.abs(frames["moving", 1] - frames["moving", 2]).mean())
                check(still == 0.0, f"{label} nova:animateNoise off: two frames are one image (largest difference {still:.6f})")
                check(moving > 1e-4, f"{label} by default every frame has its own noise (mean difference {moving:.5f})")

            # ---- nova:lightOverride.
            px, _, _ = husk(renderer, "headlight", tag)
            if px is not None:
                want = 0.5 * 2.0 / np.pi
                got, t = float(mean(px, corner)[0]), mean(px, top)
                check(abs(got - want) < 0.006, f"{label} headlight: the ground is 2 a / pi = {want:.4f}: {got:.4f}")
                check(abs(float(t[1]) - 0.6 * 2.0 / np.pi) < 0.008 and abs(float(t[0]) - 0.2 * 2.0 / np.pi) < 0.005,
                      f"{label} headlight: the cube's top too: {t.round(4)}")
            px, _, _ = husk(renderer, "dome", tag)
            if px is not None:
                got = float(mean(px, corner)[0])
                check(0.44 < got < 0.505, f"{label} dome override: the ground is its albedo 0.5 less what the cube hides: {got:.4f}")

            # ---- nova:simplifiedShading with the fog, and with occlusion.
            px, _, _ = husk(renderer, "simplified", tag)
            if px is not None:
                g, t = mean(px, corner), mean(px, top)
                check(np.allclose(g, (0.25, 0.25, 0.75), atol=0.01),
                      f"{label} simplified shading: the ground is half its colour, half fog: {g.round(4)}")
                check(np.allclose(t, (0.15, 0.45, 0.55), atol=0.01),
                      f"{label} simplified shading: the cube's top a quarter fog: {t.round(4)}")
            px, _, _ = husk(renderer, "occlusion", tag)
            if px is not None:
                far, near = float(mean(px, corner)[0]), float(mean(px, beside)[0])
                check(abs(far - 0.5) < 0.01, f"{label} occlusion: open ground keeps its colour: {far:.4f}")
                check(near < 0.92 * far, f"{label} occlusion: the ground next to the cube is darker: {near:.4f}")

            # ---- nova:outline: red lines around the cube, no outline AOV in the render.
            px, _, _ = husk(renderer, "outline", tag)
            if px is not None and base is not None:
                red = lambda image: int(((image[..., 0] - image[..., 1]) > 0.25).sum())  # noqa: E731
                check(red(base) == 0 and red(px) > 150,
                      f"{label} nova:outline draws lines into the beauty: {red(px)} red pixels ({red(base)} without)")
                inside = float(np.abs(px[top[0], top[1], :3] - base[top[0], top[1], :3]).mean())
                check(inside < 0.02, f"{label} nova:outline leaves surfaces away from edges alone: {inside:.4f}")

            # ---- Holdouts and ray visibility as the render settings' defaults.
            px, _, _ = husk(renderer, "matte", tag)
            if px is not None:
                check(float(np.abs(px).max()) < 1e-4,
                      f"{label} nova:object:holdoutMode matte on the render settings: nothing in the image "
                      f"({float(np.abs(px).max()):.5f})")
            px, _, _ = husk(renderer, "phantom", tag)
            if px is not None:
                check(float(px[..., 3].max()) < 1e-4,
                      f"{label} nova:object:renderVisibility -primary on the render settings: the camera sees no object "
                      f"(alpha {float(px[..., 3].max()):.5f})")

            # ---- Denoising husk's own beauty (a stage without render vars).
            noisy, _, _ = husk(renderer, "noisy", tag)
            clean, _, log = husk(renderer, "denoised", tag)
            if noisy is not None and clean is not None:
                def rough(image):  # the mean step between neighbours on the open ground
                    part = image[10:110, 104:156, 0]
                    return float(np.abs(np.diff(part, axis=1)).mean())
                check(rough(clean) < 0.5 * rough(noisy),
                      f"{label} nova:denoise on husk's own beauty: {rough(noisy):.4f} noisy, {rough(clean):.4f} denoised")
                check(abs(float(clean[..., :3].mean()) - float(noisy[..., :3].mean())) < 0.05 * float(noisy[..., :3].mean()),
                      f"{label} nova:denoise keeps the image's mean: {float(noisy[..., :3].mean()):.4f}, "
                      f"{float(clean[..., :3].mean()):.4f}")
                check(np.array_equal(clean[..., 3], noisy[..., 3]), f"{label} nova:denoise leaves alpha alone")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
