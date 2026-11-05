"""M9 research probe (docs/research/m9/lights.md, section 4.2): what a
third-party Hydra delegate receives for light linking in Houdini 22.0.466,
in husk and in-process (UsdImagingGL, as in the Solaris viewport).

Needs the LinkProbe delegate (lt_link_probe/, built stand-alone and staged
with -Stage <builddir>/pkg). Isolated hython only:
  powershell -NoProfile -ExecutionPolicy Bypass -File tests\\houdini\\run_hython.ps1 \\
      -Script docs\\research\\m9\\probes\\lt_link_hydra.py -Stage <builddir>/pkg <outdir> [husk|inprocess]
'husk' runs husk once per LT_LINK_SI mode (none, hdsi, houdini) and also
with Karma (BRAY_HdKarma, TF_DEBUG off) to check the scene renders;
'inprocess' drives the delegate through UsdImagingGL.Engine in this process
for the mode in the environment variable LT_LINK_SI.
Writes <outdir>/lt_link_scene.usda and <outdir>/lt_out_link_hydra_<mode>.txt.

Scene: boxes sphA, sphB, other, a ground quad, a point instancer of boxes
(/World/pi), two native instances of a box (/World/ni1, /World/ni2), a box
with MeshLightAPI (/World/glow), a point instancer of sphere lights
(/World/lpi) and two native instances of a sphere light (/World/nil1, nil2);
lights: key (rect; lightLink includeRoot 0 with sphA, pi, ni1; shadowLink
excludes sphB; karma:light:samplingquality 2), key2 (membershipExpression
/World/sph*, includeRoot unauthored), key3 (lightLink excludes the instance
proxy /World/ni2/m), dome (lightLink excludes ground), sun (no linking).
"""

import os
import subprocess
import sys

OUT = os.path.abspath(sys.argv[1]).replace('\\', '/')
WHAT = sys.argv[2] if len(sys.argv) > 2 else 'husk'
os.makedirs(OUT, exist_ok=True)
HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')


def quad(name, y, size, extra=''):
    s = size
    return '''
    def Mesh "%s"%s
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-%g, %g, %g), (%g, %g, %g), (%g, %g, -%g), (-%g, %g, -%g)]
        float3[] extent = [(-%g, %g, -%g), (%g, %g, %g)]
    }''' % (name, extra, s, y, s, s, y, s, s, y, s, s, y, s, s, y, s, s, y, s)


def box(name, x, apis=''):
    return '''
    def Mesh "%s"%s
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
        point3f[] points = [(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (0.5, 0.5, 0.5),
                            (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5)]
        float3[] extent = [(-0.5, -0.5, -0.5), (0.5, 0.5, 0.5)]
        double3 xformOp:translate = (%g, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }''' % (name, apis, x)


SCENE = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{''' + box('sphA', -1.5) + box('sphB', 1.5) + quad('ground', -0.5, 5) + box('other', 0) + '''
    def PointInstancer "pi"
    {
        point3f[] positions = [(-2, 2, 0), (0, 2, 0), (2, 2, 0)]
        int[] protoIndices = [0, 0, 0]
        rel prototypes = [</World/pi/protos/p0>]
        def Scope "protos"
        {''' + box('p0', 0) + '''
        }
    }
    def Xform "ni1" (instanceable = true; references = </Proto>)
    {
        double3 xformOp:translate = (-2, 4, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Xform "ni2" (instanceable = true; references = </Proto>)
    {
        double3 xformOp:translate = (2, 4, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }''' + box('glow', 0, ' (prepend apiSchemas = ["MeshLightAPI"])').replace(
    'double3 xformOp:translate = (0, 0, 0)', 'double3 xformOp:translate = (0, 6, 0)') + '''
    def PointInstancer "lpi"
    {
        point3f[] positions = [(-1, 8, 0), (1, 8, 0)]
        int[] protoIndices = [0, 0]
        rel prototypes = [</World/lpi/protos/l0>]
        def Scope "protos"
        {
            def SphereLight "l0"
            {
                float inputs:radius = 0.1
            }
        }
    }
    def Xform "nil1" (instanceable = true; references = </LProto>)
    {
        double3 xformOp:translate = (-1, 9, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Xform "nil2" (instanceable = true; references = </LProto>)
    {
        double3 xformOp:translate = (1, 9, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Camera "cam"
    {
        double3 xformOp:translate = (0, 2, 15)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
class Xform "Proto"
{''' + box('m', 0) + '''
}
class Xform "LProto"
{
    def SphereLight "l"
    {
        float inputs:radius = 0.1
    }
}
def Scope "lights"
{
    def RectLight "key" (prepend apiSchemas = ["CollectionAPI:lightLink", "CollectionAPI:shadowLink"])
    {
        uniform bool collection:lightLink:includeRoot = 0
        rel collection:lightLink:includes = [</World/sphA>, </World/pi>, </World/ni1>]
        rel collection:shadowLink:excludes = [</World/sphB>]
        float inputs:karma:light:samplingquality = 2
        double3 xformOp:translate = (0, 5, 3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def SphereLight "key2" (prepend apiSchemas = ["CollectionAPI:lightLink"])
    {
        pathExpression collection:lightLink:membershipExpression = "/World/sph*"
        double3 xformOp:translate = (0, 5, -3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def SphereLight "key3" (prepend apiSchemas = ["CollectionAPI:lightLink"])
    {
        rel collection:lightLink:excludes = [</World/ni2/m>]
        double3 xformOp:translate = (3, 5, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def DomeLight "dome" (prepend apiSchemas = ["CollectionAPI:lightLink"])
    {
        rel collection:lightLink:excludes = [</World/ground>]
    }
    def DistantLight "sun"
    {
    }
}
def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        int2 resolution = (32, 32)
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = 1
    }
}
'''

usd = OUT + '/lt_link_scene.usda'
with open(usd, 'w', encoding='utf-8', newline='\n') as f:
    f.write(SCENE)


def keep(text):
    return '\n'.join(l for l in text.splitlines() if '[linkprobe]' in l or l.startswith('    ') or 'rror' in l)


if WHAT == 'husk':
    for mode in ('none', 'hdsi', 'houdini'):
        env = dict(os.environ, LT_LINK_SI=mode)
        run = subprocess.run([os.path.join(HFS, 'bin', 'husk.exe'), '--renderer', 'HdLinkProbeRendererPlugin',
                              '--headlight', 'none', '-s', '/Render/s', '-o', OUT + '/probe.exr', usd],
                             capture_output=True, text=True, cwd=OUT, env=env, timeout=600)
        text = 'husk exit %d, LT_LINK_SI=%s\n' % (run.returncode, mode) + keep(run.stdout + run.stderr)
        with open('%s/lt_out_link_hydra_husk_%s.txt' % (OUT, mode), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text.replace(OUT, '<out>') + '\n')
        print(text[:3000], flush=True)
    run = subprocess.run([os.path.join(HFS, 'bin', 'husk.exe'), '--renderer', 'BRAY_HdKarma', '--headlight', 'none',
                          '-s', '/Render/s', '-o', OUT + '/karma.exr', usd],
                         capture_output=True, text=True, cwd=OUT, timeout=600)
    print('karma exit', run.returncode, (run.stdout + run.stderr)[-1500:], flush=True)
else:
    import hou  # noqa: F401  (hython: Houdini's USD plugins are registered)
    from pxr import Gf, Usd, UsdImagingGL
    mode = os.environ.get('LT_LINK_SI', 'none')
    stage = Usd.Stage.Open(usd)
    params = UsdImagingGL.Engine.Parameters()
    params.gpuEnabled = False
    engine = UsdImagingGL.Engine(params)
    print('SetRendererPlugin', engine.SetRendererPlugin('HdLinkProbeRendererPlugin'), 'mode', mode, flush=True)
    engine.SetRenderBufferSize(Gf.Vec2i(32, 32))
    engine.SetRenderViewport(Gf.Vec4d(0, 0, 32, 32))
    cam = Gf.Matrix4d().SetLookAt(Gf.Vec3d(0, 2, 15), Gf.Vec3d(0, 2, 0), Gf.Vec3d(0, 1, 0))
    proj = Gf.Frustum()
    proj.SetPerspective(30.0, 1.0, 0.1, 1000.0)
    engine.SetCameraState(cam, proj.ComputeProjectionMatrix())
    rp = UsdImagingGL.RenderParams()
    for i in range(2):
        engine.Render(stage.GetPseudoRoot(), rp)
    print('render calls done', flush=True)
