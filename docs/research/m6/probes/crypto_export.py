# Builds Solaris cryptomatte setups over crypto_scene.usda and exports what
# the LOPs author:
#   out/crypto_lop_layer.usda       the Karma Cryptomatte LOP's own layer (mode 0: add to the
#                                   existing product; primitives, materials, kind, primvar asset_name)
#   out/crypto_render.usda          the flattened stage (for husk)
#   out/crypto_lop_layer_mode1.usda mode 1 (separate render product), materials only
#   out/crypto_render_mode1.usda    its flattened stage
#   out/crypto_rendervar_layer.usda a Render Var LOP with "Enable Cryptomatte"
#   out/*_parms.txt                 parameter dumps
# Run: run_hython.ps1 -Script crypto_export.py <outdir>   (outdir outside the repo)
import os
import sys

import hou

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
fwd = lambda p: p.replace("\\", "/")  # noqa: E731

stage = hou.node("/stage")
sub = stage.createNode("sublayer")
sub.parm("filepath1").set(fwd(os.path.join(here, "crypto_scene.usda")))

krs = stage.createNode("karmarendersettings")
krs.setFirstInput(sub)
for name, value in (("camera", "/World/cam"), ("resolutionx", 160), ("resolutiony", 90),
                    ("picture", fwd(os.path.join(out, "karma_beauty.exr")))):
    p = krs.parm(name)
    if p is None:
        print("karmarendersettings: no parm", name)
        continue
    try:
        p.set(value)
    except hou.Error as e:
        print("karmarendersettings: cannot set", name, type(e).__name__)
with open(os.path.join(out, "krs_parms.txt"), "w") as f:
    for p in krs.parms():
        try:
            v = p.eval()
        except Exception as e:  # noqa
            v = "<%s>" % e
        f.write("%-60s %-35r %r\n" % (p.name(), p.parmTemplate().label(), v))

crypto = stage.createNode("karmacryptomatte")
crypto.setFirstInput(krs)
with open(os.path.join(out, "crypto_parms.txt"), "w") as f:
    for p in crypto.parms():
        try:
            v = p.eval()
        except Exception as e:  # noqa
            v = "<%s>" % e
        f.write("%-40s %-35r %r\n" % (p.name(), p.parmTemplate().label(), v))

crypto.parm("cryptopicture").set(fwd(os.path.join(out, "karma_crypto.exr")))
crypto.parm("doprimcrypto").set(1)
crypto.parm("primcryptofile").set(fwd(os.path.join(out, "karma_crypto_prim.exr")))
crypto.parm("domtlcrypto").set(1)
crypto.parm("mtlcryptofile").set(fwd(os.path.join(out, "karma_crypto_mtl.exr")))
crypto.parm("dokindcrypto").set(1)
crypto.parm("kindcryptofile1").set(fwd(os.path.join(out, "karma_crypto_kind.exr")))
crypto.parm("doprimvarcrypto").set(1)
crypto.parm("primvarcrypto1").set("asset_name")
crypto.parm("primvarcryptofile1").set(fwd(os.path.join(out, "karma_crypto_primvar.exr")))
crypto.cook(force=True)
for n in (sub, krs, crypto):
    for e in n.errors() + n.warnings():
        print(n.name(), "message:", e)

crypto.activeLayer().Export(os.path.join(out, "crypto_lop_layer.usda"))
crypto.stage().Flatten().Export(os.path.join(out, "crypto_render.usda"))

# Mode "Create Render Product" (1): a separate product for the cryptomattes.
crypto2 = stage.createNode("karmacryptomatte")
crypto2.setFirstInput(krs)
crypto2.parm("renderproductmode").set(1)
crypto2.parm("rendersettings").set("/Render/karmarendersettings1")
crypto2.parm("cryptopicture").set(fwd(os.path.join(out, "karma_crypto_separate.exr")))
crypto2.parm("domtlcrypto").set(1)
crypto2.cook(force=True)
for e in crypto2.errors() + crypto2.warnings():
    print(crypto2.name(), "message:", e)
crypto2.activeLayer().Export(os.path.join(out, "crypto_lop_layer_mode1.usda"))
crypto2.stage().Flatten().Export(os.path.join(out, "crypto_render_mode1.usda"))

# A Render Var LOP with "Enable Cryptomatte" (driver:parameters:aov:karma:cryptomatte*).
rv = stage.createNode("rendervar")
rv.setFirstInput(krs)
for p in rv.parms():
    n = p.name()
    if n.endswith("_control_3bckdf") or n.endswith("_control_mickdf") or n.endswith("_control_inckdf"):
        p.set("set")
rv.parm("xn__driverparametersaovkarmacryptomatte_2xbkdf").set(1)
rv.parm("xn__driverparametersaovkarmacryptomatterank_l4bkdf").set(4)
rv.parm("xn__driverparametersaovkarmacryptomattesidecar_h9bkdf").set(fwd(os.path.join(out, "rv_manifest.json")))
rv.cook(force=True)
for e in rv.errors() + rv.warnings():
    print(rv.name(), "message:", e)
rv.activeLayer().Export(os.path.join(out, "crypto_rendervar_layer.usda"))
with open(os.path.join(out, "rendervar_parms.txt"), "w") as f:
    for p in rv.parms():
        try:
            v = p.eval()
        except Exception as e:  # noqa
            v = "<%s>" % e
        f.write("%-60s %-35r %r\n" % (p.name(), p.parmTemplate().label(), v))
print("exported to", out)
