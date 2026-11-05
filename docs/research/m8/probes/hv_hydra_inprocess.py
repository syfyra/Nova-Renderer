# M8 research probe (houdini-volumes.md): drives a Hydra render delegate
# in-process on a cooked LOP stage, like the Solaris viewport does (same
# process as the SOP/LOP cook, so "op:" field paths are live). Uses
# UsdImagingGL.Engine without a GPU and the VolumeProbe delegate
# (hv_volume_probe, staged with -Stage <probe pkg>); the delegate logs to stderr.
#
#   $env:HV_PROBE_HELPER=<hv_volume_helper.dll>
#   powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m8\probes\hv_hydra_inprocess.py -Stage <probe pkg> <scene dir> [lop path] [renderer plugin id]
import os
import sys

import hou
from pxr import Gf, UsdImagingGL

scene = os.path.abspath(sys.argv[1])
lop = sys.argv[2] if len(sys.argv) > 2 else '/stage/pyro_stage/OUT'
plugin = sys.argv[3] if len(sys.argv) > 3 else 'HdVolumeProbeRendererPlugin'
hou.hipFile.load(os.path.join(scene, 'hv_scene.hip').replace('\\', '/'), suppress_save_prompt=True, ignore_load_warnings=True)
hou.setFrame(16)
stage = hou.node(lop).stage()
print('renderer plugins:', list(UsdImagingGL.Engine.GetRendererPlugins()), flush=True)
engine = None
if hasattr(UsdImagingGL.Engine, 'Parameters'):
    params = UsdImagingGL.Engine.Parameters()
    params.gpuEnabled = False
    engine = UsdImagingGL.Engine(params)
else:
    try:
        engine = UsdImagingGL.Engine(gpuEnabled=False)
    except TypeError:
        engine = UsdImagingGL.Engine()
print('engine created, gpu disabled:', hasattr(UsdImagingGL.Engine, 'Parameters'), flush=True)
ok = engine.SetRendererPlugin(plugin)
print('SetRendererPlugin', plugin, ok, flush=True)
engine.SetRenderBufferSize(Gf.Vec2i(64, 64))
engine.SetRenderViewport(Gf.Vec4d(0, 0, 64, 64))
cam = Gf.Matrix4d().SetLookAt(Gf.Vec3d(0, 2, 12), Gf.Vec3d(0, 2, 0), Gf.Vec3d(0, 1, 0))
proj = Gf.Frustum()
proj.SetPerspective(30.0, 1.0, 0.1, 1000.0)
engine.SetCameraState(cam, proj.ComputeProjectionMatrix())
rp = UsdImagingGL.RenderParams()
rp.frame = hou.frame()
for i in range(2):
    engine.Render(stage.GetPseudoRoot(), rp)
print('render calls done; converged', engine.IsConverged(), flush=True)
