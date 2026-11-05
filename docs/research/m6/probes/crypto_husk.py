# Renders a USD file with husk in an isolated Houdini environment (own prefs,
# no user packages, installed Nova skipped; the staged Nova on HOUDINI_PATH
# with --stage) and prints `iinfo -v` of every EXR written to the output dir.
#
#   $HFS/python313/python.exe crypto_husk.py --renderer BRAY_HdKarma --usd x.usda --out dir [--stage S] [-- husk args]
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile

HFS = os.environ.get("HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466")


def isolated_env(stage):
    prefs = tempfile.mkdtemp(prefix="nova_crypto_prefs_")
    env = dict(os.environ)
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for var in ("HOME", "HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH", "HOUDINI_PATH"):
        env.pop(var, None)
    env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    if stage:
        env["HOUDINI_PATH"] = os.path.abspath(stage).replace("\\", "/") + ";&"
    env["PATH"] = os.path.join(HFS, "bin") + os.pathsep + env.get("PATH", "")
    return env, prefs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--renderer", default="BRAY_HdKarma")
    ap.add_argument("--usd", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--stage", default="")
    ap.add_argument("--info", action="store_true", help="only print iinfo of the EXRs in --out")
    ap.add_argument("husk_args", nargs="*")
    a = ap.parse_args()
    env, prefs = isolated_env(a.stage)
    try:
        if not a.info:
            cmd = [os.path.join(HFS, "bin", "husk.exe"), "--renderer", a.renderer, "-V", "2", *a.husk_args, a.usd]
            print(" ".join(cmd), flush=True)
            proc = subprocess.run(cmd, env=env, capture_output=True, text=True, cwd=a.out)
            print("husk exit", proc.returncode)
            print(proc.stdout)
            print(proc.stderr)
        for exr in sorted(glob.glob(os.path.join(a.out, "*.exr"))):
            info = subprocess.run([os.path.join(HFS, "bin", "hoiiotool.exe"), "--info", "-v", "-a", exr], env=env,
                                  capture_output=True, text=True)
            print(info.stdout)
            print(info.stderr)
        for side in sorted(glob.glob(os.path.join(a.out, "*.json"))):
            print("== sidecar", side)
            with open(side, encoding="utf-8") as f:
                print(f.read()[:4000])
    finally:
        shutil.rmtree(prefs, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
