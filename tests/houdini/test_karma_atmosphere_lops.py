"""The Karma Sky Atmosphere and Karma Physical Sky LOPs still author what
tests/husk/check_karma_atmosphere.py was measured with (docs/design/M11-P7.md;
ctest houdini_karma_atmosphere_lops): rebuilds the scenes of
tests/husk/karma_atmosphere with the LOPs (karma_atmosphere/make_scenes.py)
and compares every authored attribute of the KarmaSkyAtmosphere prims (the
primvars) and of the lights (the Physical Sky's sun) with the checked-in
files. A difference means a Houdini build changed the LOPs: re-measure Karma
and regenerate the scenes.

Usage: hython test_karma_atmosphere_lops.py <outdir>
"""

import os
import runpy
import sys

from pxr import Usd

HERE = os.path.dirname(os.path.abspath(__file__))
SCENES = os.path.join(HERE, "..", "husk", "karma_atmosphere")
OUT = os.path.abspath(sys.argv[1])
FAILURES = []


def check(cond, message):
    print(("ok   " if cond else "FAIL ") + message, flush=True)
    if not cond:
        FAILURES.append(message)


def authored(path):
    """{prim path: {attribute: value}} of the atmosphere and light prims."""
    stage = Usd.Stage.Open(path)
    out = {}
    for prim in stage.Traverse():
        if prim.GetTypeName() not in ("KarmaSkyAtmosphere", "DistantLight"):
            continue
        out[str(prim.GetPath())] = {a.GetName(): a.Get() for a in prim.GetAuthoredAttributes()
                                    if a.GetName().startswith(("inputs:", "primvars:", "xformOp"))}
    return out


def same(a, b):
    if isinstance(a, float) or isinstance(b, float):
        return abs(float(a) - float(b)) <= 1e-5 * max(1.0, abs(float(b)))
    try:
        if len(a) == len(b) and not isinstance(a, str):
            return all(same(x, y) for x, y in zip(a, b))
    except TypeError:
        pass
    return a == b


saved_argv = sys.argv
sys.argv = [os.path.join(SCENES, "make_scenes.py"), OUT]
runpy.run_path(sys.argv[0], run_name="__main__")
sys.argv = saved_argv

names = sorted(f for f in os.listdir(SCENES) if f.endswith(".usda"))
check(len(names) == 14, "14 checked-in scenes")
for name in names:
    built = os.path.join(OUT, name)
    if not os.path.exists(built):
        check(False, name + " was rebuilt")
        continue
    want, got = authored(os.path.join(SCENES, name)), authored(built)
    check(sorted(want) == sorted(got), "%s: prims %s" % (name, sorted(got)))
    for prim, attrs in want.items():
        other = got.get(prim, {})
        diff = [k for k in sorted(set(attrs) | set(other))
                if k not in attrs or k not in other or not same(other[k], attrs[k])]
        check(not diff, "%s %s: the LOP authors the measured values%s" % (
            name, prim, "" if not diff else " (differs: %s)" % ", ".join(
                "%s %r != %r" % (k, other.get(k), attrs.get(k)) for k in diff)))
print("%d failure(s)" % len(FAILURES), flush=True)
sys.exit(1 if FAILURES else 0)
