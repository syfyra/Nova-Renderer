import os, sys, subprocess, tempfile
hfs = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
D = os.path.dirname(os.path.abspath(__file__))
env = dict(os.environ)
prefs = tempfile.mkdtemp(prefix="probe_hprefs_")
env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
for var in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
    env.pop(var, None)
env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
env["MSYS_NO_PATHCONV"] = "1"
for settings in sys.argv[2:]:
    out = os.path.join(D, settings.strip("/").replace("/", "_") + ".exr")
    r = subprocess.run([os.path.join(hfs, "bin", "husk.exe"), "--renderer", "BRAY_HdKarma", "--settings", settings,
                        "-o", out, os.path.join(D, sys.argv[1])], env=env, capture_output=True, text=True, timeout=600)
    print(settings, "rc", r.returncode, r.stderr[-1500:])
    r2 = subprocess.run([os.path.join(hfs, "bin", "hoiiotool.exe"), out, "--stats"], capture_output=True, text=True)
    print("\n".join(l for l in r2.stdout.splitlines() if "Avg" in l or "Min" in l or "Max" in l))
