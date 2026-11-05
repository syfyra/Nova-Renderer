"""Compare noisy / denoised renders against a converged reference.

Houdini's Python (numpy + OpenImageIO):

    $HFS/python313/python.exe -I dn_analyze.py --ref truth.exr img1.exr [img2.exr ...]
                              [--plane C] [--json out.json]

For every image and every EXR part it prints the part name, channels and
statistics. For the beauty part (default "C", the first part of husk's
multi-part files) it prints, against the reference:
  rmse      root mean squared error of RGB
  relmse    mean of (x - ref)^2 / (ref^2 + 1e-2), the usual denoiser metric
  relmse_detail / relmse_flat
            relmse over pixels next to an albedo or normal edge of the
            reference (its "albedo" and "N" parts, dilated by one pixel) and
            over the rest
  relmse_specular
            relmse over geometry whose reference diffuse albedo is < 0.02
            (metals, glass), outside the detail mask
  hf        a reference-free noise measure: RMS of the difference between the
            image and its 3x3 box blur (edges contribute too; compare it with
            the reference's own value)
  mean      mean RGB (a denoiser must not change it much)
and whether the non-beauty parts are bit-identical between the images (do
the filters modify the guide planes?).
"""
import argparse
import json
import os
import sys

import numpy as np

HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
os.add_dll_directory(os.path.join(HFS, 'bin'))
import OpenImageIO as oiio  # noqa: E402


def read_parts(path):
    inp = oiio.ImageInput.open(path)
    if inp is None:
        raise RuntimeError('cannot open %s: %s' % (path, oiio.geterror()))
    parts = {}
    order = []
    sub = 0
    while inp.seek_subimage(sub, 0):
        spec = inp.spec()
        name = spec.getattribute('name') or ('part%d' % sub)
        pixels = inp.read_image(sub, 0, 0, spec.nchannels, 'float')
        parts[name] = (list(spec.channelnames), np.asarray(pixels).reshape(spec.height, spec.width, spec.nchannels))
        order.append(name)
        sub += 1
    inp.close()
    return order, parts


def box3(img):
    p = np.pad(img, ((1, 1), (1, 1), (0, 0)), mode='edge')
    acc = np.zeros_like(img)
    for dy in range(3):
        for dx in range(3):
            acc += p[dy:dy + img.shape[0], dx:dx + img.shape[1]]
    return acc / 9.0


def hf(img):
    return float(np.sqrt(np.mean((img - box3(img)) ** 2)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref')
    ap.add_argument('--plane', default='C')
    ap.add_argument('--json')
    ap.add_argument('images', nargs='+')
    a = ap.parse_args()

    ref = None
    edges = None
    spec_mask = None
    if a.ref:
        _, rparts = read_parts(a.ref)
        ref = rparts[a.plane][1][..., :3]
        print('reference %s: mean %s hf %.5f' % (a.ref, np.round(ref.reshape(-1, 3).mean(0), 5), hf(ref)))
        # Detail mask: pixels next to an albedo or normal discontinuity of the
        # reference (where guides are supposed to help), dilated by 1 pixel.
        if 'albedo' in rparts and 'N' in rparts:
            g = np.zeros(ref.shape[:2], bool)
            for name, thr in (('albedo', 0.05), ('N', 0.2)):
                p = rparts[name][1][..., :3]
                dx = np.abs(np.diff(p, axis=1)).max(-1) > thr
                dy = np.abs(np.diff(p, axis=0)).max(-1) > thr
                g[:, 1:] |= dx
                g[:, :-1] |= dx
                g[1:, :] |= dy
                g[:-1, :] |= dy
            m = g.copy()
            m[1:, :] |= g[:-1, :]
            m[:-1, :] |= g[1:, :]
            m[:, 1:] |= g[:, :-1]
            m[:, :-1] |= g[:, 1:]
            edges = m
            print('detail mask: %d of %d pixels' % (int(m.sum()), m.size))
            # Specular mask: geometry whose diffuse albedo (CDA) is ~0 (metals,
            # glass), where a diffuse-only albedo guide carries no information.
            alb = rparts['albedo'][1][..., :3]
            nrm = rparts['N'][1][..., :3]
            spec_mask = (alb.max(-1) < 0.02) & (np.abs(nrm).max(-1) > 0.5) & ~edges
            print('specular mask: %d pixels' % int(spec_mask.sum()))

    results = {}
    first_parts = None
    for path in a.images:
        order, parts = read_parts(path)
        print('\n%s' % path)
        for name in order:
            ch, px = parts[name]
            flat = px.reshape(-1, px.shape[2])
            print('  part %-10s %-40s min %s max %s mean %s' % (
                name, ','.join(ch), np.round(flat.min(0), 4), np.round(flat.max(0), 4), np.round(flat.mean(0), 4)))
        if a.plane not in parts and len(order) == 1:
            parts[a.plane] = parts[order[0]]  # single-part file (the spike's output)
        if a.plane not in parts:
            print('  no plane', a.plane)
            continue
        img = parts[a.plane][1][..., :3]
        r = {'mean': img.reshape(-1, 3).mean(0).tolist(), 'hf': hf(img)}
        if ref is not None:
            d = img - ref
            r['rmse'] = float(np.sqrt(np.mean(d ** 2)))
            rel = d ** 2 / (ref ** 2 + 1e-2)
            r['relmse'] = float(np.mean(rel))
            if edges is not None:
                r['relmse_detail'] = float(np.mean(rel[edges]))
                r['relmse_flat'] = float(np.mean(rel[~edges]))
                if spec_mask is not None and spec_mask.any():
                    r['relmse_specular'] = float(np.mean(rel[spec_mask]))
            r['mean_rel_diff'] = ((img.reshape(-1, 3).mean(0) - ref.reshape(-1, 3).mean(0)) /
                                  ref.reshape(-1, 3).mean(0)).tolist()
        if parts[a.plane][1].shape[2] > 3:
            alpha = parts[a.plane][1][..., 3]
            r['alpha_min_max_mean'] = [float(alpha.min()), float(alpha.max()), float(alpha.mean())]
        print('  %s: %s' % (a.plane, json.dumps({k: (np.round(v, 6).tolist() if isinstance(v, list) else round(v, 6))
                                                  for k, v in r.items()})))
        if first_parts is None:
            first_parts = (path, parts)
        else:
            same = {}
            for name, (ch, px) in parts.items():
                if name == a.plane or name not in first_parts[1]:
                    continue
                other = first_parts[1][name][1]
                same[name] = bool(other.shape == px.shape and np.array_equal(other, px))
            r['guides_identical_to_first'] = same
            print('  other parts identical to %s: %s' % (first_parts[0], same))
        results[path] = r
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(results, f, indent=1)


if __name__ == '__main__':
    main()
