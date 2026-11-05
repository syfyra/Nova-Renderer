"""Feasibility: render a MaterialX node graph with MaterialX's own GLSL
reference implementation (PyMaterialXRenderGlsl, offscreen) to get golden
values for VM tests. Graph: texcoord -> noise2d(amplitude 1, pivot 0) -> output."""
import os, sys
HFS = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
os.add_dll_directory(os.path.join(HFS, "bin"))
import MaterialX as mx
import MaterialX.PyMaterialXGenShader as mx_gen
import MaterialX.PyMaterialXGenGlsl as mx_glsl
import MaterialX.PyMaterialXRender as mx_render
import MaterialX.PyMaterialXRenderGlsl as mx_rglsl

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
lib = mx.createDocument()
search = mx.FileSearchPath(os.path.join(HFS, "houdini", "materialx"))
mx.loadLibraries(mx.getDefaultDataLibraryFolders(), search, lib)
doc = mx.createDocument()
doc.importLibrary(lib)
ng = doc.addNodeGraph("NG_test")
tc = ng.addNode("texcoord", "tc", "vector2")
sc = ng.addNode("multiply", "sc", "vector2")
sc.setConnectedNode("in1", tc)
sc.setInputValue("in2", mx.Vector2(8.0, 8.0))
nz = ng.addNode("noise2d", "nz", "float")
nz.setConnectedNode("texcoord", sc)
o = ng.addOutput("out", "float")
o.setConnectedNode(nz)
ok, msg = doc.validate()
print("valid", ok, flush=True)

gen = mx_glsl.GlslShaderGenerator.create()
ctx = mx_gen.GenContext(gen)
ctx.registerSourceCodeSearchPath(search)
ctx.getOptions().hwSrgbEncodeOutput = False
shader = gen.generate("test", o, ctx)
print("generated", len(shader.getSourceCode(mx_gen.PIXEL_STAGE)))
W = H = 64
r = mx_rglsl.GlslRenderer.create(W, H, mx_render.BaseType.FLOAT)
r.initialize()
ih = mx_render.ImageHandler.create(mx_render.StbImageLoader.create())
r.setImageHandler(ih)
r.createProgram(shader)
r.validateInputs()
r.renderTextureSpace(mx.Vector2(0, 0), mx.Vector2(1, 1))
img = mx_render.Image.create(W, H, 4, mx_render.BaseType.FLOAT)
img.createResourceBuffer()
img = r.captureImage(img)
print('captured', flush=True)
mx_render.ImageHandler.create(mx_render.StbImageLoader.create())
path = os.path.join(out, "noise2d.exr")
h = mx_render.ImageHandler.create(mx_render.StbImageLoader.create())
try:
    import MaterialX.PyMaterialXRender as R
    h.addLoader(R.OiioImageLoader.create()) if hasattr(R, "OiioImageLoader") else None
except Exception as e:
    print("no oiio loader", e)
print("saved", ih.saveImage(path.replace(".exr", ".hdr"), img, True))
c = lambda x, y: tuple(round(v, 6) for v in img.getTexelColor(x, y).asTuple())
print("noise px", c(0,0), c(1,0), c(10, 20), c(63, 63))

ng2 = doc.addNodeGraph("NG_tc")
tc2 = ng2.addNode("texcoord", "tc", "vector2")
cv = ng2.addNode("convert", "cv", "color3")
cv.setConnectedNode("in", tc2)
o2 = ng2.addOutput("out", "color3")
o2.setConnectedNode(cv)
shader2 = gen.generate("tc", o2, ctx)
r.createProgram(shader2)
r.validateInputs()
r.renderTextureSpace(mx.Vector2(0, 0), mx.Vector2(1, 1))
img2 = mx_render.Image.create(W, H, 4, mx_render.BaseType.FLOAT)
img2.createResourceBuffer()
img2 = r.captureImage(img2)
c2 = lambda x, y: tuple(round(v, 6) for v in img2.getTexelColor(x, y).asTuple())
print("texcoord px(0,0)", c2(0, 0), "px(63,0)", c2(63, 0), "px(0,63)", c2(0, 63))

import numpy as np
arr = np.array([[img.getTexelColor(x, y).asTuple()[0] for x in range(W)] for y in range(H)], np.float32)  # row y = v index (bottom first)
np.save(os.path.join(out, "noise2d_glsl.npy"), arr)
print("saved npy", arr.min(), arr.max())
