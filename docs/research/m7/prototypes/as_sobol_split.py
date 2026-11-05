"""M7 adaptive sampling prototype: which half of a pixel's samples can serve
as the second estimate of a half-buffer error estimate with Nova's sampler.

A numpy port of core/math/sobol.h (Sampler::make/get2D, checked against the
golden values of tests/test_sobol.cpp) generates the 2D samples of many
pixels. The samples [0, n) of a pixel are split into halves A and B in four
ways:
  prefix   A = [0, n/2), B = [n/2, n)                     (checkpoints at 2^k)
  evenodd  A = even sample indices, B = odd
  classA   A = popcount(index & 0xaaaaaaaa) even, B = odd  (Cycles' sample_is_class_A,
           after Christensen et al. 2018, section 10.2.1)
  random   A = a random half (reference: what an unstratified split does)
For each split it reports (1) how often A is a (0, log2(n/2), 2)-net, and
(2) for test integrands on the pixel square the calibration of the
half-buffer estimate |A - B| / 2 against the true error |I - mu| of the
full mean I: ratio = sqrt(E[(A-B)^2/4] / E[(I-mu)^2]) over pixels, and the
rank correlation of the estimate with the true error over pixels.

    $HFS/python313/python.exe -I as_sobol_split.py [--pixels 4000] [--kmax 9]
"""
import argparse
import os

HFS = os.environ.get("HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466")
import numpy as np  # noqa: E402

U32 = np.uint32
M = 0xFFFFFFFF


def u32(x):
    return np.asarray(x, dtype=np.uint64) & M


def mul(a, b):
    return (u32(a) * np.uint64(b & M if isinstance(b, int) else 0) if isinstance(b, int) else (u32(a) * u32(b))) & M


def reverse_bits(v):
    v = u32(v)
    v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1)
    v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2)
    v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4)
    v = ((v >> 8) & 0x00FF00FF) | ((v & 0x00FF00FF) << 8)
    return ((v >> 16) | (v << 16)) & M


def mix32(x):
    x = u32(x)
    x ^= x >> 16
    x = (x * np.uint64(0x7feb352d)) & M
    x ^= x >> 15
    x = (x * np.uint64(0x846ca68b)) & M
    x ^= x >> 16
    return x


def hash_combine(a, b):
    return mix32(u32(a) ^ ((mix32(b) + np.uint64(0x9e3779b9)) & M))


def lk_hash(v, seed):
    v, seed = u32(v), u32(seed)
    v ^= (v * np.uint64(0x3d20adea)) & M
    v = (v + seed) & M
    v = (v * ((seed >> 16) | np.uint64(1))) & M
    v ^= (v * np.uint64(0x05526c56)) & M
    v ^= (v * np.uint64(0x53a22864)) & M
    return v


def owen_scramble(v, seed):
    return reverse_bits(lk_hash(reverse_bits(v), seed))


def sobol_dim1(index):
    index = u32(index)
    x = np.zeros_like(index)
    v = np.full_like(index, 1 << 31)
    for _ in range(32):
        x ^= np.where(index & 1, v, 0)
        index >>= 1
        v ^= v >> 1
    return x


def sobol_dim1_reversed(i):
    i = u32(i)
    i ^= (i >> 1) & 0x55555555
    i ^= (i >> 2) & 0x33333333
    i ^= (i >> 4) & 0x0F0F0F0F
    i ^= (i >> 8) & 0x00FF00FF
    i ^= (i >> 16) & 0x0000FFFF
    return i


def unit_float(x):
    return (u32(x) >> 8).astype(np.float64) * (1.0 / 16777216.0)


def pixel_seed(x, y, seed=0):
    return hash_combine(hash_combine(u32(x), u32(y)), (seed * 0x68bc21eb + 0x02e5be93) & M)


def get2d(pseed, index, stream):
    """Sampler{pseed, index}.get2D(stream), broadcasting over arrays."""
    s = hash_combine(pseed, stream)
    i = reverse_bits(lk_hash(reverse_bits(index), s))
    x = unit_float(reverse_bits(lk_hash(i, mix32(s ^ 0x5bd1e995))))
    y = unit_float(reverse_bits(lk_hash(sobol_dim1_reversed(i), mix32(s ^ 0x1b873593))))
    return x, y


def golden():
    assert int(owen_scramble(0x12345678, 0x9abcdef0)) == 0x1f8b4945
    assert int(owen_scramble(0xffffffff, 1)) == 0x76d5b94c
    assert int(sobol_dim1(np.array([0xdeadbeef]))[0]) == 0x0c66ae4d
    i = np.arange(1 << 12)
    assert np.array_equal(reverse_bits(sobol_dim1(i)), sobol_dim1_reversed(i))


def is_net(x, y, m_log2):
    """(0, m, 2)-net test for 2^m points: every elementary box holds one."""
    n = 1 << m_log2
    xi = (x * n).astype(np.int64)
    yi = (y * n).astype(np.int64)
    for a in range(m_log2 + 1):
        b = m_log2 - a
        cell = (xi >> (m_log2 - a)) * (1 << b) + (yi >> (m_log2 - b))
        if np.bincount(cell, minlength=n).max() != 1:
            return False
    return True


def integrands(rng, pixels):
    """Per pixel a random instance of each test function on [0,1)^2 and its
    exact mean (numerically, on a fine grid)."""
    g = (np.arange(512) + 0.5) / 512
    gx, gy = np.meshgrid(g, g)
    out = {}
    # Vertical edge (x < t), a slanted edge, a smooth bump, a narrow highlight.
    t = rng.uniform(0.2, 0.8, pixels)
    out["vertical edge"] = (lambda x, y, p: (x < t[p]).astype(float), t)
    ang = rng.uniform(0, np.pi, pixels)
    off = rng.uniform(-0.25, 0.25, pixels)
    nx, ny = np.cos(ang), np.sin(ang)

    def slanted(x, y, p):
        return ((x - 0.5) * nx[p] + (y - 0.5) * ny[p] < off[p]).astype(float)
    mu = np.array([((gx - 0.5) * nx[p] + (gy - 0.5) * ny[p] < off[p]).mean() for p in range(pixels)])
    out["slanted edge"] = (slanted, mu)
    cx, cy = rng.uniform(0.3, 0.7, pixels), rng.uniform(0.3, 0.7, pixels)

    def bump(x, y, p):
        return np.exp(-((x - cx[p]) ** 2 + (y - cy[p]) ** 2) / 0.08)
    mu = np.array([np.exp(-((gx - cx[p]) ** 2 + (gy - cy[p]) ** 2) / 0.08).mean() for p in range(pixels)])
    out["smooth bump"] = (bump, mu)

    def spot(x, y, p):
        return ((x - cx[p]) ** 2 + (y - cy[p]) ** 2 < 0.01).astype(float) * 10.0
    mu = np.array([(((gx - cx[p]) ** 2 + (gy - cy[p]) ** 2 < 0.01) * 10.0).mean() for p in range(pixels)])
    out["small bright spot"] = (spot, mu)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pixels", type=int, default=3000)
    ap.add_argument("--kmax", type=int, default=9)
    ap.add_argument("--stream", type=int, default=0, help="0 = kPixelFilter; 9 = bounce 0 kBsdf")
    args = ap.parse_args()
    golden()
    print("numpy port matches the golden values of tests/test_sobol.cpp")
    rng = np.random.default_rng(1)
    pix = rng.integers(0, 4096, size=(args.pixels, 2))
    pseeds = pixel_seed(pix[:, 0], pix[:, 1])
    funcs = integrands(rng, args.pixels)

    for k in range(4, args.kmax + 1):
        n = 1 << k
        idx = np.arange(n, dtype=np.uint64)
        X, Y = get2d(pseeds[:, None], idx[None, :], args.stream)  # pixels x n
        pop_odd = np.array([bin(int(i) & 0xaaaaaaaa).count("1") & 1 for i in idx])
        splits = {
            "prefix": idx < n // 2,
            "evenodd": (idx & 1) == 0,
            "classA": pop_odd == 0,
        }
        rnd_masks = np.zeros((args.pixels, n), dtype=bool)
        for p in range(args.pixels):
            rnd_masks[p, rng.permutation(n)[: n // 2]] = True
        print(f"\nn = {n}: share of pixels whose half A is a (0,{k - 1},2)-net "
              f"(first 300 pixels; stream {args.stream})")
        for name, mask in splits.items():
            nets = sum(is_net(X[p, mask], Y[p, mask], k - 1) for p in range(min(300, args.pixels)))
            print(f"   {name:8s} {nets / min(300, args.pixels):6.1%}")
        print(f"   calibration sqrt(E[(A-B)^2/4] / E[(I-mu)^2]) and Spearman rank correlation of |A-B|/2 with |I-mu|:")
        for fname, (f, mu) in funcs.items():
            vals = np.stack([f(X[p], Y[p], p) for p in range(args.pixels)])  # pixels x n
            I = vals.mean(1)
            err = I - mu
            row = []
            for name in list(splits) + ["random"]:
                mask = rnd_masks if name == "random" else np.broadcast_to(splits[name], vals.shape)
                A = (vals * mask).sum(1) / mask.sum(1)
                B = (vals * ~mask).sum(1) / (~mask).sum(1)
                est = (A - B) / 2
                ratio = np.sqrt(np.mean(est ** 2) / max(np.mean(err ** 2), 1e-30))
                ra = np.argsort(np.argsort(np.abs(est)))
                rb = np.argsort(np.argsort(np.abs(err)))
                rho = np.corrcoef(ra, rb)[0, 1]
                row.append(f"{name} {ratio:6.2f} ({rho:+.2f})")
            print(f"   {fname:18s} rms err {np.sqrt(np.mean(err ** 2)):.2e} | " + " | ".join(row))


if __name__ == "__main__":
    main()
