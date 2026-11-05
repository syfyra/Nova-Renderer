// Research probe for M8 (docs/research/m8/houdini-volumes.md), not part of Nova.
// HDK-linked helper DLL, see hv_volume_helper.h. Uses only public HDK headers
// (GT_PrimVDB, GT_PrimVolume, GEO_PrimVolume), OpenVDB and NanoVDB from $HFS.
#include "hv_volume_helper.h"

#include <GEO/GEO_PrimVDB.h>
#include <GEO/GEO_PrimVolume.h>
#include <GT/GT_PrimVDB.h>
#include <GT/GT_PrimVolume.h>
#include <GT/GT_Primitive.h>
#include <GT/GT_PrimitiveTypes.h>
#include <UT/UT_Vector3.h>
#include <UT/UT_VoxelArray.h>

#include <openvdb/openvdb.h>
#include <nanovdb/util/CreateNanoGrid.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using VdbFn = void *(*)(const char *, const char *, int);
using HouFn = void *(*)(const char *, const char *, int);

struct SopVol {
    VdbFn vdb = nullptr;
    HouFn hou = nullptr;
    std::string error;
    SopVol() {
        HMODULE m = GetModuleHandleA("USD_SopVol.dll");
        if (!m) {
            const char *hfs = std::getenv("HFS");
            std::string path = std::string(hfs ? hfs : "C:/Program Files/Side Effects Software/Houdini 22.0.466") +
                               "/houdini/dso/USD_SopVol.dll";
            m = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!m) {
                error = "LoadLibrary failed for " + path;
                return;
            }
        }
        vdb = reinterpret_cast<VdbFn>(GetProcAddress(m, "SOPgetVDBVolumePrimitiveWithIndex"));
        hou = reinterpret_cast<HouFn>(GetProcAddress(m, "SOPgetHoudiniVolumePrimitive"));
        if (!vdb || !hou)
            error = "missing SOPget* exports";
    }
};

SopVol &sopVol() {
    static SopVol s;
    return s;
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void setMatrix(HvFieldInfo *out, const openvdb::math::Mat4d &m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out->indexToWorld[r * 4 + c] = m(r, c);
}

void convertToNano(const openvdb::FloatGrid &grid, HvFieldInfo *out) {
    const auto t0 = std::chrono::steady_clock::now();
    auto handle = nanovdb::createNanoGrid(grid);
    out->msConvert = msSince(t0);
    out->nanoBytes = int64_t(handle.size());
    if (const nanovdb::NanoGrid<float> *g = handle.grid<float>())
        out->nanoChecksum = g->checksum();
}

int queryVdb(const GT_PrimVDB *gt, HvFieldInfo *out) {
    const openvdb::GridBase *base = gt->getGrid();
    if (!base) {
        std::snprintf(out->error, sizeof out->error, "GT_PrimVDB without grid");
        return out->kind = -1;
    }
    std::snprintf(out->valueType, sizeof out->valueType, "%s", base->valueType().c_str());
    std::snprintf(out->gridClass, sizeof out->gridClass, "%s",
                  openvdb::GridBase::gridClassToString(base->getGridClass()).c_str());
    std::snprintf(out->gridName, sizeof out->gridName, "%s", base->getName().c_str());
    out->activeVoxels = int64_t(base->activeVoxelCount());
    const openvdb::CoordBBox bb = base->evalActiveVoxelBoundingBox();
    for (int i = 0; i < 3; ++i) {
        out->bboxMin[i] = bb.min()[i];
        out->bboxMax[i] = bb.max()[i];
        out->voxelSize[i] = base->voxelSize()[i];
    }
    setMatrix(out, base->transform().baseMap()->getAffineMap()->getMat4());
    if (const auto *fg = dynamic_cast<const openvdb::FloatGrid *>(base)) {
        float lo = 1e30f, hi = -1e30f;
        for (auto it = fg->cbeginValueOn(); it; ++it) {
            lo = std::min(lo, *it);
            hi = std::max(hi, *it);
        }
        out->valueMin = lo;
        out->valueMax = hi;
        convertToNano(*fg, out);
    }
    return out->kind = 1;
}

int queryVolume(const GT_PrimVolume *gt, HvFieldInfo *out) {
    const auto *gv = dynamic_cast<const GEO_PrimVolume *>(gt->getGeoPrimitive());
    if (!gv) {
        std::snprintf(out->error, sizeof out->error, "GT_PrimVolume without GEO_PrimVolume");
        return out->kind = -1;
    }
    int rx = 0, ry = 0, rz = 0;
    gv->getRes(rx, ry, rz);
    std::snprintf(out->valueType, sizeof out->valueType, "float");
    std::snprintf(out->gridClass, sizeof out->gridClass, "houdini volume");
    out->activeVoxels = int64_t(rx) * ry * rz;
    out->bboxMax[0] = rx - 1;
    out->bboxMax[1] = ry - 1;
    out->bboxMax[2] = rz - 1;
    UT_Vector3 p0, px, py, pz;
    gv->indexToPos(0, 0, 0, p0);
    gv->indexToPos(1, 0, 0, px);
    gv->indexToPos(0, 1, 0, py);
    gv->indexToPos(0, 0, 1, pz);
    openvdb::math::Mat4d m = openvdb::math::Mat4d::identity();
    const UT_Vector3 rows[4] = {px - p0, py - p0, pz - p0, p0};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            m(r, c) = rows[r][c];
    setMatrix(out, m);
    out->voxelSize[0] = (px - p0).length();
    out->voxelSize[1] = (py - p0).length();
    out->voxelSize[2] = (pz - p0).length();

    const auto t0 = std::chrono::steady_clock::now();
    UT_VoxelArrayReadHandleF h = gv->getVoxelHandle();
    openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.0f);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(m));
    auto acc = grid->getAccessor();
    float lo = 1e30f, hi = -1e30f;
    for (int k = 0; k < rz; ++k)
        for (int j = 0; j < ry; ++j)
            for (int i = 0; i < rx; ++i) {
                const float v = (*h)(i, j, k);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                if (v != 0.0f)
                    acc.setValue(openvdb::Coord(i, j, k), v);
            }
    out->valueMin = lo;
    out->valueMax = hi;
    const double msDense = msSince(t0);
    convertToNano(*grid, out);
    out->msConvert += msDense;
    return out->kind = 2;
}

} // namespace

extern "C" __declspec(dllexport) int hvQueryField(const char *filePath, const char *fieldName, int fieldIndex,
                                                  int houdiniField, HvFieldInfo *out) {
    std::memset(out, 0, sizeof *out);
    static bool init = (openvdb::initialize(), true);
    (void)init;
    SopVol &sv = sopVol();
    if (!sv.error.empty()) {
        std::snprintf(out->error, sizeof out->error, "%s", sv.error.c_str());
        return out->kind = 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    void *p = houdiniField ? sv.hou(filePath, fieldName, fieldIndex) : sv.vdb(filePath, fieldName, fieldIndex);
    out->msLookup = msSince(t0);
    if (!p) {
        std::snprintf(out->error, sizeof out->error, "resolver returned null");
        return out->kind = 0;
    }
    const auto *gt = static_cast<const GT_Primitive *>(p);
    out->gtType = gt->getPrimitiveType();
    if (out->gtType == GT_PRIM_VDB_VOLUME)
        return queryVdb(static_cast<const GT_PrimVDB *>(gt), out);
    if (out->gtType == GT_PRIM_VOXEL_VOLUME)
        return queryVolume(static_cast<const GT_PrimVolume *>(gt), out);
    std::snprintf(out->error, sizeof out->error, "unexpected GT type %d (%s)", out->gtType, gt->className());
    return out->kind = -1;
}
