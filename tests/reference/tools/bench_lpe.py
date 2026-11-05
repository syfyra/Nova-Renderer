"""LPE cost benchmark (docs/design/M6.md M6.6): renders reference scenes with
0, 1, 20 and 74 LPE render vars next to the beauty and reports nova_cli's
render time (minimum of N runs, the sets interleaved per run).

  python bench_lpe.py --cli build/vs2022/tools/nova_cli/Release/nova_cli.exe
                      [--runs 5] [--scenes cornell_rect mtl_spheres mtl_glass]

The sets:
  0   the beauty only
  1   the beauty and C<RD>.*L (combined diffuse; it splits the scattering classes)
  20  Karma's standard set: the beauty and 19 vars (18 distinct expressions
      and the beauty's unoccluded;/shadow; variants, karma-aovs.md 1.3)
  74  Karma's full table (26 LPE vars with the six prefixed diffuse ones) and
      its 16 lighting rules split for the light tags key, fill and rim (48),
      as the Karma Render Settings LOP's per-tag split authors them. The
      scenes' lights have no tags, so those outputs stay black; the automaton
      and the per-lobe splits are the full size.

Layers go to --out (default: a temp directory); nothing is written into the
repository. The renders use the scenes' settings prim /Render/ref (its
product and ray limits), unlike the plain `nova_cli scene.usda` of the
other tables. $HFS/bin must be on PATH for nova_cli.
"""

import argparse
import os
import re
import subprocess
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
SCENES = os.path.join(ROOT, 'tests', 'reference', 'scenes')

KARMA20 = [
    'unoccluded;C.*[LO]', 'shadow;C.*[LO]',
    'C<RD>.*L', 'C<RD>L', 'C<RD>.+L',
    'C<RG>.*L', 'C<RG>L', 'C<RG>.+L',
    'C<TG>.*L', "C<...'coat'>.*L", 'C<TD>.*L', 'CL',
    'C.*O', 'CO', 'C.+O',
    'CV.*L', 'CVL', 'CV.+L',
    'CDA',
]
PREFIXED = ['unoccluded;C<RD>.*L', 'unoccluded;C<RD>L', 'unoccluded;C<RD>.+L',
            'shadow;C<RD>.*L', 'shadow;C<RD>L', 'shadow;C<RD>.+L']


def tag_split(rule, tag):
    if rule.endswith('[LO]'):
        return rule[:-4] + "<L.'%s'>" % tag
    if rule.endswith('L'):
        return rule[:-1] + "<L.'%s'>" % tag
    return None


LIGHTING = ['C.*[LO]'] + [r for r in KARMA20 if tag_split(r, 'x')]
SPLITS = [tag_split(r, t) for r in LIGHTING if r not in ('unoccluded;C.*[LO]', 'shadow;C.*[LO]')
          for t in ('key', 'fill', 'rim')]
SPLITS += [tag_split(r, t) for r in ('unoccluded;C.*[LO]', 'shadow;C.*[LO]') for t in ('key', 'fill', 'rim')]
SETS = {
    0: [],
    1: ['C<RD>.*L'],
    20: KARMA20,
    74: KARMA20 + PREFIXED + SPLITS,
}


def layer(scene, rules):
    path = os.path.join(SCENES, scene + '.usda').replace('\\', '/')
    out = ['#usda 1.0', '(', '    subLayers = [@%s@]' % path, ')', '', 'over "Render"', '{']
    names = ['</Render/Vars/C>'] + ['</Render/BenchVars/v%d>' % i for i in range(len(rules))]
    out += ['    over "Products"', '    {', '        over "beauty"', '        {',
            '            rel orderedVars = [%s]' % ', '.join(names), '        }', '    }']
    out += ['    def Scope "BenchVars"', '    {']
    for i, rule in enumerate(rules):
        out += ['        def RenderVar "v%d"' % i, '        {',
                '            uniform token dataType = "color3f"',
                '            uniform string sourceName = "%s"' % rule.replace('"', '\\"'),
                '            uniform token sourceType = "lpe"',
                '            token driver:parameters:aov:format = "color3f"',
                '            string driver:parameters:aov:name = "v%d"' % i,
                '        }']
    out += ['    }', '}', '']
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--res', default='960x540')
    ap.add_argument('--spp', default='64')
    ap.add_argument('--scenes', nargs='+', default=['cornell_rect', 'mtl_spheres', 'mtl_glass'])
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    # The counts include the beauty.
    assert len(SETS[20]) + 1 == 20 and len(SETS[74]) + 1 == 74, (len(SETS[20]), len(SETS[74]))
    out = args.out or tempfile.mkdtemp(prefix='nova_bench_lpe_')
    os.makedirs(out, exist_ok=True)
    rows = []
    for scene in args.scenes:
        best = {}
        for n, rules in SETS.items():
            with open(os.path.join(out, '%s_lpe%d.usda' % (scene, n)), 'w', encoding='utf-8', newline='\n') as f:
                f.write(layer(scene, rules))
        for run in range(args.runs):
            for n in SETS:
                usd = os.path.join(out, '%s_lpe%d.usda' % (scene, n))
                exr = os.path.join(out, '%s_lpe%d.exr' % (scene, n))
                res = subprocess.run([args.cli, usd, '-o', exr, '--settings', '/Render/ref', '--res', args.res,
                                      '--spp', args.spp],
                                     capture_output=True, text=True)
                m = re.search(r'converged in ([0-9.]+) s \(.*?([0-9.]+) Mrays/s\)', res.stdout)
                if res.returncode != 0 or not m:
                    raise SystemExit('%s %d LPEs failed:\n%s\n%s' % (scene, n, res.stdout, res.stderr))
                t = float(m.group(1))
                best[n] = min(best.get(n, t), t)
                print('%s %2d LPEs run %d: %.3f s' % (scene, n, run, t), flush=True)
        rows.append((scene, best))
    print()
    print('| Scene | 0 LPEs | 1 | 20 (Karma\'s set) | 74 |')
    print('|---|---|---|---|---|')
    for scene, best in rows:
        base = best[0]
        cells = ['%.2f s' % base] + ['%.2f s (%+.1f %%)' % (best[n], 100.0 * (best[n] / base - 1.0)) for n in (1, 20, 74)]
        print('| `%s` | %s |' % (scene, ' | '.join(cells)))


if __name__ == '__main__':
    main()
