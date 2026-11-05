import os, sys, subprocess, tempfile, struct, math, shutil
HFS = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
D = os.path.dirname(os.path.abspath(__file__))
renderer = os.environ.get("KARMA_RENDERER", "BRAY_HdKarma")
suffix = "" if renderer == "BRAY_HdKarma" else "_xpu"

def env():
    e = dict(os.environ)
    prefs = tempfile.mkdtemp(prefix="nova_hprefs_")
    e["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for v in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
        e.pop(v, None)
    e["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    e["MSYS_NO_PATHCONV"] = "1"
    return e

def read_pfm(path):
    with open(path, "rb") as f:
        hdr = f.readline().strip()
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        n = 3 if hdr == b"PF" else 1
        data = struct.unpack(("<" if scale < 0 else ">") + "f" * (w * h * n), f.read(4 * w * h * n))
    rows = [[data[(y * w + x) * n:(y * w + x) * n + n] for x in range(w)] for y in range(h)]
    rows.reverse()  # PFM is bottom-to-top
    return w, h, rows

def profile(rows, w, h):
    # bins in mu_o
    bins = [(0.97, 1.0), (0.85, 0.9), (0.68, 0.72), (0.48, 0.52), (0.28, 0.32), (0.1, 0.15)]
    acc = {b: [0.0, 0.0, 0.0, 0] for b in bins}
    for j in range(h):
        for i in range(w):
            x = -1 + (i + 0.5) * 2 / w
            y = 1 - (j + 0.5) * 2 / h
            r2 = x * x + y * y
            if r2 >= 1: continue
            mu = math.sqrt(1 - r2)
            for b in bins:
                if b[0] <= mu < b[1]:
                    a = acc[b]; p = rows[j][i]
                    a[0] += p[0]; a[1] += p[1]; a[2] += p[2]; a[3] += 1
    out = []
    for b in bins:
        a = acc[b]
        if a[3]: out.append("mu%.2f:%.4f/%.4f/%.4f" % ((b[0] + b[1]) / 2, a[0] / a[3], a[1] / a[3], a[2] / a[3]))
    bg = rows[0][0]
    return " ".join(out) + "  bg=%.3f" % bg[0]

def main(names):
    e = env()
    for n in names:
        usd = os.path.join(D, n + ".usda")
        exr = os.path.join(D, n + suffix + ".exr")
        pfm = os.path.join(D, n + suffix + ".pfm")
        cmd = [HFS + "/bin/husk.exe", "--renderer", renderer, "--headlight", "none", "-s", "/Render/rs", "-o", exr, usd]
        p = subprocess.run(cmd, env=e, capture_output=True, text=True, cwd=D)
        if p.returncode != 0 or not os.path.exists(exr):
            print(n, "FAILED", p.stderr[-800:]); continue
        subprocess.run([HFS + "/bin/hoiiotool.exe", exr, "--ch", "R,G,B", "-d", "float", "-o", pfm], check=True, capture_output=True)
        w, h, rows = read_pfm(pfm)
        print("%-16s %s" % (n, profile(rows, w, h)), flush=True)

if __name__ == "__main__":
    main(sys.argv[1:])
