"""Adaptive sampling benchmark (docs/design/M7.md M7.3): render time at equal
error. For each scene, nova_cli renders uniform ladders (--spp 32 ... 1024)
and the variance oracle (maximum 1024, threshold 0.01, minimum automatic)
with the scene's settings prim /Render/ref; the error is the relMSE against
Nova's own converged image (the mean of two 4096-spp renders with other
seeds, their noise subtracted), so model differences to Karma do not enter.
The uniform time at the oracle's error comes from a log-log fit of the
ladder.

  python bench_adaptive.py --cli build/vs2022/tools/nova_cli/Release/nova_cli.exe
                           [--scenes mtl_glass cornell_rect dome_sky] [--runs 3]

Run with Houdini's Python ($HFS/python313/python.exe: numpy, OpenImageIO);
$HFS/bin must be on PATH for nova_cli. Layers and images go to --out.
"""

import argparse
import math
import os
import re
import subprocess
import tempfile

import numpy as np

if os.environ.get('HFS') and hasattr(os, 'add_dll_directory'):  # OpenImageIO's DLLs
    os.add_dll_directory(os.path.join(os.environ['HFS'], 'bin'))
import OpenImageIO as oiio  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
SCENES = os.path.join(ROOT, 'tests', 'reference', 'scenes')
IMAGES = os.path.join(ROOT, 'tests', 'reference', 'images')


def read(path):
    buf = oiio.ImageBuf(path)
    return np.asarray(buf.get_pixels(oiio.FLOAT), dtype=np.float64)[..., :3]


def layer(scene, oracle, threshold, seed=0):
    path = os.path.join(SCENES, scene + '.usda').replace('\\', '/')
    return '\n'.join(['#usda 1.0', '(', '    subLayers = [@%s@]' % path, ')', '', 'over "Render"', '{',
                      '    over "ref"', '    {', '        token nova:pixelOracle = "%s"' % oracle,
                      '        float nova:oracleThreshold = %g' % threshold, '        int nova:seed = %d' % seed,
                      '    }', '}', ''])


def render(cli, usd, exr, spp):
    r = subprocess.run([cli, usd, '-o', exr, '--settings', '/Render/ref', '--spp', str(spp)], capture_output=True,
                       text=True)
    m = re.search(r'converged in ([0-9.]+) s', r.stdout)
    if r.returncode != 0 or not m:
        raise SystemExit('%s failed:\n%s\n%s' % (usd, r.stdout, r.stderr))
    return float(m.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--scenes', nargs='+', default=['mtl_glass', 'cornell_rect', 'dome_sky'])
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--threshold', type=float, default=0.01)
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    out = args.out or tempfile.mkdtemp(prefix='nova_bench_adaptive_')
    os.makedirs(out, exist_ok=True)
    ladder = [32, 64, 128, 256, 512, 1024]
    print('| Scene | uniform (spp: time, relMSE) | variance oracle | uniform time at equal error | speed-up |')
    print('|---|---|---|---|---|')
    for scene in args.scenes:
        truth = []
        for seed in (1, 2):
            usd = os.path.join(out, '%s_truth%d.usda' % (scene, seed))
            open(usd, 'w', encoding='utf-8', newline='\n').write(layer(scene, 'uniform', args.threshold, seed))
            exr = os.path.join(out, '%s_truth%d.exr' % (scene, seed))
            if not os.path.exists(exr):
                render(args.cli, usd, exr, 4096)
            truth.append(read(exr))
        ref = (truth[0] + truth[1]) / 2
        denom = ref * ref + 0.01
        floor = float(np.mean((truth[0] - truth[1]) ** 2 / 4 / denom)) # the variance of the mean

        def error(path):
            img = read(path)
            return max(float(np.mean((img - ref) ** 2 / denom)) - floor, 1e-12)

        uni = os.path.join(out, scene + '_uniform.usda')
        ada = os.path.join(out, scene + '_variance.usda')
        open(uni, 'w', encoding='utf-8', newline='\n').write(layer(scene, 'uniform', args.threshold))
        open(ada, 'w', encoding='utf-8', newline='\n').write(layer(scene, 'variance', args.threshold))
        points = []
        for spp in ladder:
            exr = os.path.join(out, '%s_u%d.exr' % (scene, spp))
            t = min(render(args.cli, uni, exr, spp) for _ in range(args.runs))
            points.append((spp, t, error(exr)))
        exr = os.path.join(out, scene + '_variance.exr')
        ta = min(render(args.cli, ada, exr, 1024) for _ in range(args.runs))
        ea = error(exr)
        # log(time) against log(error) on the ladder, interpolated (or
        # extrapolated from the nearest two points).
        le = [math.log(p[2]) for p in points]
        lt = [math.log(p[1]) for p in points]
        x = math.log(ea)
        i = 0
        while i + 2 < len(points) and le[i + 1] > x:
            i += 1
        slope = (lt[i + 1] - lt[i]) / (le[i + 1] - le[i])
        tu = math.exp(lt[i] + slope * (x - le[i]))
        print('| `%s` | %s | %.2f s, %.2e | %.2f s | %.2fx |' % (
            scene, ', '.join('%d: %.2f s %.1e' % p for p in points), ta, ea, tu, tu / ta), flush=True)


if __name__ == '__main__':
    main()
