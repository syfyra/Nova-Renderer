# Lists the cryptomatte-related parameters of the Karma Render Settings LOP,
# the Render Var LOP and the Karma Cryptomatte LOP in this Houdini build,
# with menu items.
# Run: run_hython.ps1 -Script crypto_parms.py
import hou

stage = hou.node("/stage")


def menu(p):
    t = p.parmTemplate()
    try:
        items = t.menuItems()
        labels = t.menuLabels()
    except AttributeError:
        return ""
    if not items:
        return ""
    return " menu=" + ", ".join("%s(%s)" % (i, l) for i, l in zip(items, labels))


for type_name in ("karmarenderproperties", "karmarendersettings", "rendervar", "karmastandardrendervars",
                  "karmacryptomatte"):
    try:
        node = stage.createNode(type_name)
    except hou.OperationFailed as e:
        print("no node type", type_name, e)
        continue
    print("==", type_name, node.type().name())
    for p in node.parms():
        t = p.parmTemplate()
        name = p.name()
        if ("crypt" in name.lower() or "crypt" in t.label().lower()
                or (type_name == "karmacryptomatte" and name in ("renderproductmode", "rendersettings"))):
            try:
                val = p.eval()
            except Exception as e:  # noqa
                val = "<%s>" % e
            print("  %-50s %-30r %r%s" % (name, t.label(), val, menu(p)))
            if t.help():
                print("      help:", t.help().replace("\n", " ")[:400])

for name, nt in hou.lopNodeTypeCategory().nodeTypes().items():
    if "crypt" in name.lower() or "crypt" in nt.description().lower():
        print("LOP type:", name, nt.description())
