"""Helpers shared by make_references.py and check_references.py.

Standard library only (the system Python has no numpy/pxr); Houdini's husk,
iinfo and hoiiotool do the USD and image work, nova_imgcmp the statistics.
"""

import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import tempfile

SKIP = 77  # ctest SKIP_RETURN_CODE


def load_manifest(refdir):
    with open(os.path.join(refdir, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    return manifest


def scene_config(manifest, name):
    """The scene entry with the defaults merged in (scene values win)."""
    for scene in manifest["scenes"]:
        if scene["name"] == name:
            merged = dict(scene)
            for key in ("test", "reference", "compare"):
                merged[key] = dict(manifest["defaults"].get(key, {}), **scene.get(key, {}))
            return merged
    raise KeyError(f"scene '{name}' is not in manifest.json")


def usd_path(path):
    return os.path.abspath(path).replace("\\", "/")


def tool(hfs, name):
    return os.path.join(hfs, "bin", name)


class IsolatedHoudini:
    """Environment for husk without user packages or an installed Nova
    (same rules as tests/husk/check_scenes.py)."""

    def __init__(self, hfs, stage=None):
        self.prefs = tempfile.mkdtemp(prefix="nova_hprefs_")
        env = dict(os.environ)
        env["HOUDINI_USER_PREF_DIR"] = os.path.join(self.prefs, "houdini__HVER__").replace("\\", "/")
        for var in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
            env.pop(var, None)
        env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
        if stage:
            env["HOUDINI_PATH"] = stage.replace("\\", "/") + ";&"
        # nova_imgcmp needs OpenImageIO_sidefx.dll.
        env["PATH"] = os.path.join(hfs, "bin") + os.pathsep + env.get("PATH", "")
        self.env = env

    def close(self):
        shutil.rmtree(self.prefs, ignore_errors=True)


def run(cmd, env, log=None):
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if log:
        with open(log, "w", encoding="utf-8") as f:
            f.write(" ".join(cmd) + "\n" + proc.stdout + proc.stderr)
    return proc


def husk(hfs, env, renderer, layer, settings, output, extra=(), log=None):
    cmd = [tool(hfs, "husk.exe"), "--renderer", renderer, "--headlight", "none", "-s", settings, "-o", output,
           *extra, layer]
    proc = run(cmd, env, log)
    if proc.returncode != 0 or not os.path.exists(output):
        raise RuntimeError(f"husk failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stderr[-1500:]}")


OVERRIDE = """#usda 1.0
(
    subLayers = [@{scene}@]{timing}
)

over "Render"
{{
    over "{prim}"
    {{
        int nova:samples = {spp}
        int nova:seed = {seed}
    }}
}}
"""

# Stage timing comes from the root layer only, so the override layer repeats
# the scene's; otherwise husk and nova_cli would render frame 0 instead of the
# scene's start frame (the frame the Karma reference shows).
TIMING_KEYS = ("startTimeCode", "endTimeCode", "timeCodesPerSecond", "framesPerSecond")


def layer_timing(scene_file):
    with open(scene_file, encoding="utf-8") as f:
        header = f.read(4096)
    header = header[:header.find(")")] if ")" in header else ""
    return {m.group(1): m.group(2)
            for m in re.finditer(r"^\s*(" + "|".join(TIMING_KEYS) + r")\s*=\s*([-+0-9.eE]+)", header, re.M)}


def nova_override_layer(path, scene_file, settings, spp, seed):
    """A layer over the scene that sets Nova's sample count and seed."""
    parent, prim = settings.rsplit("/", 1)
    assert parent == "/Render", "reference settings prims live under /Render"
    timing = "".join(f"\n    {k} = {v}" for k, v in layer_timing(scene_file).items())
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(OVERRIDE.format(scene=usd_path(scene_file), timing=timing, prim=prim, spp=spp, seed=seed))
    return path


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def scene_inputs(refdir, scene_file):
    """The scene layer, its sublayers and assets that live in tests/reference
    (by scanning @...@ asset paths in the .usda text), with their hashes."""
    inputs, todo = {}, [os.path.normpath(scene_file)]
    while todo:
        path = todo.pop()
        rel = os.path.relpath(path, refdir).replace("\\", "/")
        if rel in inputs or not os.path.exists(path):
            continue
        inputs[rel] = sha1(path)
        if path.endswith(".usda"):
            with open(path, encoding="utf-8") as f:
                for asset in re.findall(r"@([^@]+)@", f.read()):
                    todo.append(os.path.normpath(os.path.join(os.path.dirname(path), asset)))
    return inputs


def reference_inputs(refdir, scene):
    """The inputs a reference depends on: the scene Nova renders and, if the
    Karma reference renders its own variant (reference.file), that one too."""
    inputs = scene_inputs(refdir, os.path.join(refdir, scene["file"]))
    karma_file = scene["reference"].get("file")
    if karma_file:
        inputs.update(scene_inputs(refdir, os.path.join(refdir, karma_file)))
    return inputs


def exr_attribute(hfs, exr, name):
    """A metadata attribute of an image as text (None if absent)."""
    out = subprocess.run([tool(hfs, "iinfo.exe"), "-v", exr], capture_output=True, text=True).stdout
    m = re.search(rf"^\s*{re.escape(name)} \(type \w+\):= \"?([^\"\n]*)\"?\s*$", out, re.MULTILINE)
    return m.group(1) if m else None


def render_stats(hfs, exr):
    """husk:render_stats of a Karma EXR (JSON with render_settings, ray_counts,
    karma_version, ...)."""
    out = subprocess.run([tool(hfs, "iinfo.exe"), "-v", exr], capture_output=True, text=True).stdout
    line = next((l for l in out.splitlines() if "husk:render_stats" in l), None)
    if not line:
        return {}
    return json.loads(line[line.index('"{') + 1: line.rindex('}"') + 1])


def image_size(hfs, exr):
    out = subprocess.run([tool(hfs, "iinfo.exe"), exr], capture_output=True, text=True).stdout
    m = re.search(r"(\d+)\s*x\s*(\d+)", out)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def extract_plane(hfs, env, exr, plane, out):
    """One part of a multi-part EXR (by its name, the RenderVar's
    driver:parameters:aov:name) as its own image."""
    proc = subprocess.run([tool(hfs, "hoiiotool.exe"), exr, "--subimage", plane, "-o", out], env=env,
                          capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.exists(out):
        raise RuntimeError(f"cannot extract plane {plane} of {exr}: {proc.stderr[-500:]}")
    return out


def plane_compare(scene, plane):
    """The comparison settings of an AOV plane: the scene's, with the plane's
    own entries of compare.aovs (regions replace the scene's)."""
    compare = {k: v for k, v in scene["compare"].items() if k != "aovs"}
    compare.update(scene["compare"].get("aovs", {}).get(plane, {}))
    compare.setdefault("regions", [])
    return compare


def imgcmp_args(compare):
    if "pixel_tol" in compare:  # deterministic images: per pixel |T - R| <= abs + rel |R|
        return ["--pixel-tol", ",".join(str(x) for x in compare["pixel_tol"])]
    args = ["--block", str(compare["block"]), "--alpha-fwer", str(compare["alpha_fwer"]),
            "--k-global", str(compare["k_global"]), "--block-rel-tol", str(compare["block_rel_tol"]),
            "--global-rel-tol", str(compare["global_rel_tol"]), "--abs-tol", str(compare["abs_tol"])]
    # Documented differences to the reference renderer, per image region:
    # {"name", "center": [x, y] (pixels, row 0 = top), "radius", "rel_tol" (region mean),
    #  "block_rel_tol" (8x8 blocks, default rel_tol), "reason"}.
    for r in compare.get("regions", []):
        args += ["--region", f"{r['name']},{r['center'][0]},{r['center'][1]},{r['radius']},{r['rel_tol']},"
                             f"{r.get('block_rel_tol', r['rel_tol'])}"]
    return args


def run_imgcmp(imgcmp, env, ref_args, test_a, test_b, compare, json_out, flip_map=None):
    cmd = [imgcmp, *ref_args, "--test", test_a, "--test2", test_b, *imgcmp_args(compare), "--json", json_out]
    if flip_map:
        cmd += ["--flip-map", flip_map]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    result = {}
    if os.path.exists(json_out):
        with open(json_out, encoding="utf-8") as f:
            result = json.load(f)
    return proc.returncode, result, proc.stderr


def dome_irradiance(hfs, texture, intensity):
    """E = integral over the upper hemisphere (dome pole = +Y, the plane's
    normal) of L cos, for the bilinear lookup of a latlong texture, by
    midpoint quadrature (texels via hoiiotool --dumpdata)."""
    out = subprocess.run([tool(hfs, "hoiiotool.exe"), "--dumpdata", texture], capture_output=True, text=True).stdout
    texels, w, h = {}, 0, 0
    for m in re.finditer(r"Pixel \((\d+), (\d+)\): ([-0-9.e]+) ([-0-9.e]+) ([-0-9.e]+)", out):
        x, y = int(m.group(1)), int(m.group(2))
        texels[(x, y)] = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
        w, h = max(w, x + 1), max(h, y + 1)

    def lookup(u, v):
        x, y = u * w - 0.5, v * h - 0.5
        fx, fy = math.floor(x), math.floor(y)
        tx, ty = x - fx, y - fy
        x0, x1 = int(fx) % w, (int(fx) + 1) % w
        y0, y1 = min(max(int(fy), 0), h - 1), min(max(int(fy) + 1, 0), h - 1)
        return [(texels[(x0, y0)][c] * (1 - tx) + texels[(x1, y0)][c] * tx) * (1 - ty)
                + (texels[(x0, y1)][c] * (1 - tx) + texels[(x1, y1)][c] * tx) * ty for c in range(3)]

    n_theta, n_phi = 256, 1024
    e = [0.0, 0.0, 0.0]
    for i in range(n_theta):
        theta = 0.5 * math.pi * (i + 0.5) / n_theta
        weight = math.cos(theta) * math.sin(theta) * (0.5 * math.pi / n_theta) * (2.0 * math.pi / n_phi)
        for j in range(n_phi):
            radiance = lookup((j + 0.5) / n_phi, theta / math.pi)
            for c in range(3):
                e[c] += weight * radiance[c]
    return [intensity * v for v in e]
