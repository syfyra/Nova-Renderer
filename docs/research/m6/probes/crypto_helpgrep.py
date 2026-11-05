# Prints the help pages under $HFS/houdini/help/*.zip that mention cryptomatte
# (file names and matching lines; with --show <zip>:<member> the whole page).
#   $HFS/python313/python.exe crypto_helpgrep.py [--show nodes.zip:lop/karmacryptomatte.txt]
import glob
import os
import re
import sys
import zipfile

HFS = os.environ.get("HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466")
help_dir = os.path.join(HFS, "houdini", "help")

if len(sys.argv) > 2 and sys.argv[1] == "--show":
    z, member = sys.argv[2].split(":", 1)
    with zipfile.ZipFile(os.path.join(help_dir, z)) as zf:
        print(zf.read(member).decode("utf-8", "replace"))
    sys.exit(0)

pat = re.compile(r"crypto", re.I)
for path in sorted(glob.glob(os.path.join(help_dir, "*.zip"))):
    with zipfile.ZipFile(path) as zf:
        for info in zf.infolist():
            if not info.filename.endswith(".txt"):
                continue
            text = zf.read(info).decode("utf-8", "replace")
            hits = [l.strip() for l in text.splitlines() if pat.search(l)]
            if hits:
                print("== %s:%s (%d)" % (os.path.basename(path), info.filename, len(hits)))
                for h in hits[:6]:
                    print("    ", h[:220])
