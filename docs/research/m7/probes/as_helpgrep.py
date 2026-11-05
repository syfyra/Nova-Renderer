# Lists the help pages under $HFS/houdini/help/*.zip that mention adaptive
# sampling terms (file names and matching lines); with --show <zip>:<member>
# it prints one page. The output is for reading only: the report describes
# what the pages say, it never copies them.
#   $HFS/python313/python.exe -I as_helpgrep.py [regex] > <scratch>/help_hits.txt
#   $HFS/python313/python.exe -I as_helpgrep.py --show nodes.zip:lop/karmarenderproperties.txt
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
        sys.stdout.buffer.write(zf.read(member))
    sys.exit(0)

default = r"oracle|varianceaa|convergence.mode|pathtracedsamples|samplesperpixel|primarysamples|noise level|minrays|adaptive sampl"
pat = re.compile(sys.argv[1] if len(sys.argv) > 1 else default, re.I)
for path in sorted(glob.glob(os.path.join(help_dir, "*.zip"))):
    with zipfile.ZipFile(path) as zf:
        for info in zf.infolist():
            if not info.filename.endswith(".txt"):
                continue
            text = zf.read(info).decode("utf-8", "replace")
            hits = [l.strip() for l in text.splitlines() if pat.search(l)]
            if hits:
                print("== %s:%s (%d)" % (os.path.basename(path), info.filename, len(hits)))
                for h in hits[:8]:
                    print("    ", h[:240])
