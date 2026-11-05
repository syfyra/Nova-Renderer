# M8 research probe (houdini-volumes.md): can an HDK helper DLL read the
# volumes behind the field prims Houdini's LOPs author -- in-memory SOP volumes
# ("op:/...sop.volumes:SDF_FORMAT_ARGS:...") as well as .vdb / .bgeo.sc files?
# Loads hv_scene.hip (hv_build_scene.py), cooks the LOP networks in this hython
# process (as the Solaris viewport would) and calls hv_volume_helper.dll's
# hvQueryField (ctypes) with exactly the filePath/fieldName/fieldIndex the
# field prims carry.
#
#   powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m8\probes\hv_query_fields.py <scene dir> <hv_volume_helper.dll>
import ctypes
import os
import sys

import hou
from pxr import Sdf, Usd

scene = os.path.abspath(sys.argv[1])
helper = ctypes.CDLL(os.path.abspath(sys.argv[2]))


class HvFieldInfo(ctypes.Structure):
    _fields_ = [('kind', ctypes.c_int), ('gtType', ctypes.c_int), ('valueType', ctypes.c_char * 32),
                ('gridClass', ctypes.c_char * 32), ('gridName', ctypes.c_char * 64), ('activeVoxels', ctypes.c_int64),
                ('bboxMin', ctypes.c_int * 3), ('bboxMax', ctypes.c_int * 3), ('voxelSize', ctypes.c_double * 3),
                ('indexToWorld', ctypes.c_double * 16), ('valueMin', ctypes.c_float), ('valueMax', ctypes.c_float),
                ('nanoBytes', ctypes.c_int64), ('nanoChecksum', ctypes.c_uint64), ('msLookup', ctypes.c_double),
                ('msConvert', ctypes.c_double), ('error', ctypes.c_char * 256)]


helper.hvQueryField.restype = ctypes.c_int
helper.hvQueryField.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.POINTER(HvFieldInfo)]


def query(path, name, index, houdini):
    info = HvFieldInfo()
    helper.hvQueryField(path.encode(), name.encode(), index, 1 if houdini else 0, ctypes.byref(info))
    m = info.indexToWorld
    return ('kind %d gt %d %s/%s active %d bbox %s-%s voxel %.4f range [%.4g, %.4g] origin (%.3f %.3f %.3f) '
            'nano %d B lookup %.2f ms convert %.2f ms %s' % (
                info.kind, info.gtType, info.valueType.decode(), info.gridClass.decode(), info.activeVoxels,
                tuple(info.bboxMin), tuple(info.bboxMax), info.voxelSize[0], info.valueMin, info.valueMax,
                m[12], m[13], m[14], info.nanoBytes, info.msLookup, info.msConvert, info.error.decode()))


hou.hipFile.load(os.path.join(scene, 'hv_scene.hip').replace('\\', '/'), suppress_save_prompt=True, ignore_load_warnings=True)
hou.setFrame(16)
for lop in ('/stage/merge_all', '/stage/pyro_stage/OUT'):
    stage = hou.node(lop).stage()
    tc = Usd.TimeCode(hou.frame())
    print('=====', lop)
    for prim in stage.Traverse():
        t = str(prim.GetTypeName())
        if t not in ('OpenVDBAsset', 'HoudiniFieldAsset'):
            continue
        fp = prim.GetAttribute('filePath').Get(tc)
        name = str(prim.GetAttribute('fieldName').Get(tc))
        idx_attr = prim.GetAttribute('fieldIndex')
        idx = int(idx_attr.Get(tc)) if idx_attr and idx_attr.Get(tc) is not None else -1
        path = fp.resolvedPath or fp.path
        print('%-36s %-17s %-12s idx %d %s' % (prim.GetPath(), t, name, idx, 'op:' if path.startswith('op:') else 'file'))
        print('    ' + query(path, name, idx, t == 'HoudiniFieldAsset'))

# Same lookups through the files the USD ROP wrote (hv_dump_stage.py save) and
# the original files, as husk would see them.
print('===== files')
for path, name, idx, hou_ in ((scene + '/pyro.vdb', 'density', 0, False),
                              (scene + '/pyro.vdb', 'temperature', 0, False),
                              (scene + '/pyro.vdb', 'density', -1, False),
                              (scene + '/pyro.vdb', 'density', 0, True),
                              (scene + '/native.bgeo.sc', 'density', 0, True),
                              (scene + '/native.bgeo.sc', 'temperature', 1, True),
                              (scene + '/usd/import_pyro.usd.volumes/0.bgeo.sc', 'density', 5, True),
                              (scene + '/usd/sopcreate_vdb.usd.volumes/2.vdb', 'flame', 0, False),
                              (scene + '/missing.vdb', 'density', 0, False)):
    print('%s %s idx %d houdini %d' % (path.replace(scene, '<scene>'), name, idx, hou_))
    print('    ' + query(path, name, idx, hou_))
# Lookup cost when called again (cache)
print('second call: ' + query(scene + '/pyro.vdb', 'density', 0, False))
