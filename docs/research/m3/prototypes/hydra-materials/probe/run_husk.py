import os, sys, subprocess, tempfile
hfs = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
P = os.path.dirname(os.path.abspath(__file__))
env = dict(os.environ)
prefs = tempfile.mkdtemp(prefix="probe_hprefs_")
env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
for var in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
    env.pop(var, None)
env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
env["HOUDINI_PATH"] = os.path.join(P, "stage").replace("\\", "/") + ";&"
env["MSYS_NO_PATHCONV"] = "1"
env["PROBE_LOG"] = os.path.join(P, sys.argv[1])
env["PROBE_CONTEXTS"] = sys.argv[2]
env["PROBE_MTLX"] = "1"
env["PROBE_DUMP_SI"] = "1"
if os.path.exists(env["PROBE_LOG"]): os.remove(env["PROBE_LOG"])
args = [os.path.join(hfs, "bin", "husk.exe"), "--renderer", "HdProbeRendererPlugin", "--settings", "/Render/rs",
        "-o", os.path.join(P, "husk_out.exr"), "-V", "2"] + sys.argv[4:] + [os.path.join(P, "scenes", sys.argv[3])]
r = subprocess.run(args, env=env, cwd=os.path.join(P, "scenes"), capture_output=True, text=True, timeout=600)
print("returncode", r.returncode)
print(r.stdout[-4000:])
print(r.stderr[-4000:])
