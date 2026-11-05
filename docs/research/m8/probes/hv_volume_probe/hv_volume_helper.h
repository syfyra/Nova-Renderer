// Research probe for M8 (docs/research/m8/houdini-volumes.md), not part of Nova.
//
// C interface of hv_volume_helper.dll: an HDK-linked helper that resolves a
// Hydra volume field (filePath, fieldName, fieldIndex, field prim type) with
// Houdini's own resolver entry points in $HFS/houdini/dso/USD_SopVol.dll
// (SOPgetVDBVolumePrimitiveWithIndex / SOPgetHoudiniVolumePrimitive), reads the
// grid through the public GT/GEO/OpenVDB headers and converts it to NanoVDB.
// The header is HDK-free so an HDK-free Hydra delegate can load the helper with
// LoadLibrary and call it.
#pragma once

#include <cstdint>

extern "C" {

struct HvFieldInfo {
    int kind;                // 0 = not found, 1 = VDB (GT_PrimVDB), 2 = Houdini volume (GT_PrimVolume), -1 = other
    int gtType;              // GT_Primitive::getPrimitiveType()
    char valueType[32];      // VDB value type ("float", "vec3s", ...) or "float" for Houdini volumes
    char gridClass[32];      // VDB grid class ("fog volume", "level set", ...)
    char gridName[64];
    int64_t activeVoxels;    // VDB active voxels / Houdini volume voxel count
    int bboxMin[3];          // index-space active bbox (VDB) / 0..res-1 (Houdini volume)
    int bboxMax[3];
    double voxelSize[3];
    double indexToWorld[16]; // row-major, row vectors (p_world = p_index * M), voxel centres at integer indices
    float valueMin, valueMax;
    int64_t nanoBytes;       // size of the NanoVDB float grid built from it (0 if not converted)
    uint64_t nanoChecksum;
    double msLookup, msConvert;
    char error[256];
};

// houdiniField != 0 -> houdiniFieldAsset (SOPgetHoudiniVolumePrimitive),
// otherwise openvdbAsset (SOPgetVDBVolumePrimitiveWithIndex).
// Returns out->kind.
__declspec(dllexport) int hvQueryField(const char *filePath, const char *fieldName, int fieldIndex, int houdiniField,
                                       HvFieldInfo *out);

typedef int (*HvQueryFieldFn)(const char *, const char *, int, int, HvFieldInfo *);
}
