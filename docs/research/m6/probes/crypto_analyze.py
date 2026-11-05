# Decodes the cryptomatte parts of an EXR written by husk and checks them
# against the Cryptomatte spec:
#   - metadata key  cryptomatte/<7 hex> = first 7 hex digits of MurmurHash3_x86_32(layer name, seed 0)
#   - manifest      name -> 8 hex digits of the float id's bits
#   - id float      MurmurHash3_x86_32(name) with the exponent fixup (exp 0 or 255 -> flip bit 23)
#   - rank layers   <layer>NN.RGBA = (id, coverage, id, coverage), pairs sorted by coverage
# and prints per-pixel statistics (coverage sums, pair counts, preview channels).
#
#   $HFS/python313/python.exe crypto_analyze.py file.exr [--pixels x,y ...]
import argparse
import json
import os
import struct
import sys

import numpy as np

os.add_dll_directory(os.path.join(os.environ.get("HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466"), "bin"))
import OpenImageIO as oiio  # noqa: E402


def murmur3_32(data: bytes, seed: int = 0) -> int:
    c1, c2 = 0xCC9E2D51, 0x1B873593
    h = seed & 0xFFFFFFFF
    n = len(data) // 4
    for i in range(n):
        k = int.from_bytes(data[4 * i:4 * i + 4], "little")
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        k = (k * c2) & 0xFFFFFFFF
        h ^= k
        h = ((h << 13) | (h >> 19)) & 0xFFFFFFFF
        h = (h * 5 + 0xE6546B64) & 0xFFFFFFFF
    tail = data[4 * n:]
    k = 0
    if len(tail) >= 3:
        k ^= tail[2] << 16
    if len(tail) >= 2:
        k ^= tail[1] << 8
    if len(tail) >= 1:
        k ^= tail[0]
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        k = (k * c2) & 0xFFFFFFFF
        h ^= k
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16
    return h


def crypto_id(name: str) -> int:
    """uint32 bits of the float id (spec: exponent 0 or 255 -> flip bit 23)."""
    h = murmur3_32(name.encode("utf-8"))
    exp = (h >> 23) & 0xFF
    if exp in (0, 255):
        h ^= 1 << 23
    return h


def bits_to_float(u: int) -> float:
    return struct.unpack("<f", struct.pack("<I", u))[0]


def read_parts(path):
    inp = oiio.ImageInput.open(path)
    if not inp:
        sys.exit(oiio.geterror())
    parts = []
    i = 0
    while inp.seek_subimage(i, 0):
        spec = inp.spec()
        pix = inp.read_image(i, 0, 0, spec.nchannels, "float")  # explicit subimage (OIIO 3)
        parts.append((spec, np.asarray(pix).reshape(spec.height, spec.width, spec.nchannels)))
        i += 1
    inp.close()
    return parts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exr")
    ap.add_argument("--pixels", nargs="*", default=[])
    ap.add_argument("--raw", action="store_true", help="dump the raw rank-layer values at --pixels")
    a = ap.parse_args()
    parts = read_parts(a.exr)
    by_name = {spec.getattribute("oiio:subimagename") or spec.getattribute("name"): (spec, px) for spec, px in parts}
    head, beauty = parts[0]
    alpha = beauty[..., 3] if beauty.shape[2] == 4 else None
    cryptos = {}
    for attr in head.extra_attribs:
        if attr.name.startswith("cryptomatte/"):
            _, key, field = attr.name.split("/", 2)
            cryptos.setdefault(key, {})[field] = attr.value
    for key, md in sorted(cryptos.items(), key=lambda kv: kv[1].get("name", "")):
        name = md.get("name", "?")
        expect_key = "%08x" % murmur3_32(name.encode())
        print("== %s  key %s  mmh3(name)=%s  key-ok=%s  fields=%s" %
              (name, key, expect_key, expect_key[:7] == key, sorted(md)))
        if "manifest" in md:
            manifest = json.loads(md["manifest"])
        elif "manif_file" in md:
            with open(os.path.join(os.path.dirname(os.path.abspath(a.exr)), md["manif_file"]), encoding="utf-8") as f:
                manifest = json.load(f)
            print("   sidecar manifest %s (%d entries)" % (md["manif_file"], len(manifest)))
        else:
            manifest = {}
        raw_ok = fixed_ok = 0
        for obj, hx in manifest.items():
            raw = murmur3_32(obj.encode())
            fixed = crypto_id(obj)
            raw_ok += ("%08x" % raw) == hx
            fixed_ok += ("%08x" % fixed) == hx
            if ("%08x" % fixed) != hx:
                print("   MISMATCH %r manifest %s raw %08x fixed %08x" % (obj, hx, raw, fixed))
        print("   manifest entries %d, match raw hash %d, match fixed-up hash %d" % (len(manifest), raw_ok, fixed_ok))
        # Rank layers.
        ranks = sorted(n for n in by_name if n.startswith(name) and n[len(name):].isdigit())
        if not ranks:
            print("   no rank layers")
            continue
        stack = np.concatenate([by_name[n][1] for n in ranks], axis=2)  # H x W x 4*L
        ids = stack[..., 0::2]
        cov = stack[..., 1::2]
        idbits = ids.astype(np.float32).view(np.uint32)
        known = {int(v, 16): k for k, v in manifest.items()}
        seen = set(np.unique(idbits[cov != 0]).tolist())
        print("   rank layers %s (%d pairs/pixel)" % (ranks, ids.shape[2]))
        unknown = sorted(b for b in seen if b not in known)
        print("   ids with coverage != 0: %s; %d unknown ids %s" %
              (sorted(known[b] for b in seen if b in known), len(unknown), ["0x%08x" % b for b in unknown[:8]]))
        for p in a.pixels if a.raw else []:
            x, y = map(int, p.split(","))
            print("   raw (%d,%d): %s" % (x, y, ["%.6g" % v for v in stack[y, x]]))
        nz = (cov != 0).sum(axis=2)
        print("   pairs used per pixel: max %d, histogram %s" % (nz.max(), np.bincount(nz.ravel()).tolist()))
        s = cov.sum(axis=2)
        print("   coverage sum: min %.6f max %.6f  (pixels with any coverage: %d of %d)" %
              (s.min(), s.max(), int((s > 0).sum()), s.size))
        desc = np.all(np.diff(np.abs(cov), axis=2) <= 1e-7, axis=2)
        print("   pairs sorted by |coverage| descending: %s; negative coverage values: %d" %
              (bool(desc.all()), int((cov < 0).sum())))
        # Zero-coverage slots: id value.
        print("   id bits where coverage == 0: %s" % sorted({"0x%08x" % b for b in np.unique(idbits[cov == 0]).tolist()})[:5])
        if alpha is not None:
            d = np.abs(s - alpha)
            print("   |coverage sum - beauty alpha|: max %.6f mean %.6f; pixels 0<sum<0.999: %d, 0<alpha<0.999: %d" %
                  (d.max(), d.mean(), int(((s > 0) & (s < 0.999)).sum()), int(((alpha > 0) & (alpha < 0.999)).sum())))
        if name in by_name:
            spec, prev = by_name[name]
            print("   preview part %r channels %s range %s..%s" %
                  (name, list(spec.channelnames), prev.reshape(-1, prev.shape[2]).min(0), prev.reshape(-1, prev.shape[2]).max(0)))
            # Preview = sum over stored pairs of coverage * (h/2^32, (h<<8 mod 2^32)/2^32, (h<<16 mod 2^32)/2^32)?
            h = idbits.astype(np.uint64)
            col = np.stack([h / 2.0 ** 32, ((h << 8) & 0xFFFFFFFF) / 2.0 ** 32, ((h << 16) & 0xFFFFFFFF) / 2.0 ** 32], -1)
            pred = (col * cov[..., None]).sum(axis=2)
            err = np.abs(pred - prev[..., :3])
            single = nz == 1
            multi_full = (nz > 1) & (nz < ids.shape[2])  # several ids, none dropped by the rank limit
            print("   preview vs sum(cov * hashcolor): max err all %.5f, single-id %.6f, multi-id below rank %.6f, "
                  "rank-full pixels %.5f" %
                  (err.max(), err[single].max() if single.any() else 0,
                   err[multi_full].max() if multi_full.any() else 0,
                   err[nz == ids.shape[2]].max() if (nz == ids.shape[2]).any() else 0))
            q = cov[cov != 0] * 9.0
            print("   coverage values that are multiples of 1/9: %d of %d" %
                  (int((np.abs(q - np.round(q)) < 1e-4).sum()), q.size))
        for p in a.pixels:
            x, y = map(int, p.split(","))
            pairs = [(known.get(int(idbits[y, x, k]), "0x%08x" % idbits[y, x, k]), float(cov[y, x, k]))
                     for k in range(ids.shape[2]) if cov[y, x, k] != 0]
            pv = by_name[name][1][y, x].tolist() if name in by_name else None
            print("   pixel (%d,%d): %s preview %s" % (x, y, pairs, pv))
    return 0


if __name__ == "__main__":
    sys.exit(main())
