// nova_volume.dll: the volume bridge (docs/design/M8.md D1).
//
// Karma resolves every volume field through two C exports of Houdini's
// USD_SopVol.dll (research H 3.1): SOPgetVDBVolumePrimitiveWithIndex and
// SOPgetHoudiniVolumePrimitive take (path, field name, index) and return a
// GT_PrimVDB or a GT_PrimVolume for in-memory "op:" SOP volumes as well as
// for .vdb and .bgeo.sc files. The bridge calls them, converts the result at
// once into NanoVDB float grids and never keeps the GT primitive (its owner
// is Houdini's cache). Calls are serialized. Without USD_SopVol (or when it
// returns nothing), .vdb files are read with OpenVDB directly.

#include "core/volume/bridge.h"

#include <GEO/GEO_PrimVDB.h>
#include <GEO/GEO_PrimVolume.h>
#include <GT/GT_PrimVDB.h>
#include <GT/GT_PrimVolume.h>
#include <GT/GT_Primitive.h>
#include <GT/GT_PrimitiveTypes.h>
#include <UT/UT_Vector3.h>
#include <UT/UT_VoxelArray.h>

#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>

#include <nanovdb/util/CreateNanoGrid.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace {

using SopGetFn = void *(*)(const char *path, const char *name, int index);

// USD_SopVol.dll's resolvers, from the Houdini the process runs (next to
// libUT.dll: $HFS/bin/../houdini/dso), else from $HFS.
struct SopVol {
    SopGetFn vdb = nullptr;
    SopGetFn houdini = nullptr;

    SopVol() {
        HMODULE m = GetModuleHandleW(L"USD_SopVol.dll");
        if (!m) {
            std::filesystem::path dso;
            if (HMODULE ut = GetModuleHandleW(L"libUT.dll")) {
                wchar_t path[MAX_PATH] = {};
                if (GetModuleFileNameW(ut, path, MAX_PATH))
                    dso = std::filesystem::path(path).parent_path().parent_path() / "houdini" / "dso";
            }
            if (dso.empty()) {
                char hfs[MAX_PATH] = {};
                if (GetEnvironmentVariableA("HFS", hfs, MAX_PATH))
                    dso = std::filesystem::path(hfs) / "houdini" / "dso";
            }
            if (!dso.empty()) // its dependencies (libHUSD.dll ...) come from PATH, as $HFS/bin is
                m = LoadLibraryExW((dso / "USD_SopVol.dll").wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (!m)
            return;
        vdb = reinterpret_cast<SopGetFn>(GetProcAddress(m, "SOPgetVDBVolumePrimitiveWithIndex"));
        houdini = reinterpret_cast<SopGetFn>(GetProcAddress(m, "SOPgetHoudiniVolumePrimitive"));
        if (!vdb || !houdini)
            vdb = houdini = nullptr;
    }
};

nova::VolumeGridPtr toNova(const openvdb::FloatGrid &grid) {
    const nanovdb::GridHandle<nanovdb::HostBuffer> handle = nanovdb::createNanoGrid(grid);
    return nova::VolumeGrid::copyOf(handle.data(), handle.size());
}

// A float grid with the values of `src` mapped by `f` (active voxels and
// tiles; inactive values become the background).
template <typename GridT, typename F>
openvdb::FloatGrid::Ptr floatGridOf(const GridT &src, float background, F &&f) {
    openvdb::FloatGrid::Ptr out = openvdb::FloatGrid::create(background);
    out->setTransform(src.transform().copy());
    out->setGridClass(src.getGridClass() == openvdb::GRID_LEVEL_SET ? openvdb::GRID_UNKNOWN : src.getGridClass());
    openvdb::FloatGrid::Accessor acc = out->getAccessor();
    for (auto it = src.cbeginValueOn(); it; ++it) {
        const float v = f(*it);
        if (it.isVoxelValue())
            acc.setValue(it.getCoord(), v);
        else
            out->tree().fill(it.getBoundingBox(), v, true);
    }
    return out;
}

// An OpenVDB grid as NanoVDB float grids: scalars as one, vectors as three.
bool convertVdb(const openvdb::GridBase &base, nova::VolumeFieldGrids &out, std::string *error) {
    if (const auto *g = dynamic_cast<const openvdb::FloatGrid *>(&base)) {
        out.grids.push_back(toNova(*g));
    } else if (const auto *g = dynamic_cast<const openvdb::DoubleGrid *>(&base)) {
        out.grids.push_back(toNova(*floatGridOf(*g, float(g->background()), [](double v) { return float(v); })));
    } else if (const auto *g = dynamic_cast<const openvdb::Int32Grid *>(&base)) {
        out.grids.push_back(toNova(*floatGridOf(*g, float(g->background()), [](int32_t v) { return float(v); })));
    } else if (const auto *g = dynamic_cast<const openvdb::BoolGrid *>(&base)) {
        out.grids.push_back(toNova(*floatGridOf(*g, 0.0f, [](bool v) { return v ? 1.0f : 0.0f; })));
    } else if (const auto *g = dynamic_cast<const openvdb::Vec3SGrid *>(&base)) {
        for (int c = 0; c < 3; ++c)
            out.grids.push_back(toNova(
                *floatGridOf(*g, float(g->background()[c]), [c](const openvdb::Vec3s &v) { return float(v[c]); })));
    } else if (const auto *g = dynamic_cast<const openvdb::Vec3DGrid *>(&base)) {
        for (int c = 0; c < 3; ++c)
            out.grids.push_back(toNova(
                *floatGridOf(*g, float(g->background()[c]), [c](const openvdb::Vec3d &v) { return float(v[c]); })));
    } else {
        if (error)
            *error = "unsupported VDB value type " + base.valueType();
        return false;
    }
    for (const nova::VolumeGridPtr &p : out.grids)
        if (!p) {
            if (error)
                *error = "NanoVDB conversion failed";
            out.grids.clear();
            return false;
        }
    return true;
}

// A Houdini native volume: its non-zero voxels, voxel centres at integer
// indices (the GEO_PrimVolume's index-to-position map).
bool convertNative(const GEO_PrimVolume &volume, nova::VolumeFieldGrids &out, std::string *error) {
    int rx = 0, ry = 0, rz = 0;
    volume.getRes(rx, ry, rz);
    UT_Vector3 p0, px, py, pz;
    volume.indexToPos(0, 0, 0, p0);
    volume.indexToPos(1, 0, 0, px);
    volume.indexToPos(0, 1, 0, py);
    volume.indexToPos(0, 0, 1, pz);
    openvdb::math::Mat4d m = openvdb::math::Mat4d::identity();
    const UT_Vector3 rows[4] = {px - p0, py - p0, pz - p0, p0};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            m(r, c) = rows[r][c];
    openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.0f);
    try {
        grid->setTransform(openvdb::math::Transform::createLinearTransform(m));
    } catch (const std::exception &e) {
        if (error)
            *error = std::string("degenerate volume transform: ") + e.what();
        return false;
    }
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    UT_VoxelArrayReadHandleF h = volume.getVoxelHandle();
    openvdb::FloatGrid::Accessor acc = grid->getAccessor();
    for (int k = 0; k < rz; ++k)
        for (int j = 0; j < ry; ++j)
            for (int i = 0; i < rx; ++i) {
                const float v = (*h)(i, j, k);
                if (v != 0.0f)
                    acc.setValue(openvdb::Coord(i, j, k), v);
            }
    out.grids.push_back(toNova(*grid));
    if (!out.grids.back()) {
        out.grids.clear();
        if (error)
            *error = "NanoVDB conversion failed";
        return false;
    }
    return true;
}

bool endsWith(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i])
            return false;
    return true;
}

// A .vdb file read with OpenVDB: the fieldIndex-th grid named fieldName
// (any grid of that name if the index is out of range).
bool loadVdbFile(const nova::VolumeFieldRequest &r, nova::VolumeFieldGrids &out, std::string *error) {
    try {
        openvdb::io::File file(r.filePath);
        file.open(false);
        openvdb::GridPtrVecPtr grids = file.getGrids();
        file.close();
        openvdb::GridBase::Ptr match;
        int seen = 0;
        for (const openvdb::GridBase::Ptr &g : *grids) {
            if (g->getName() != r.fieldName)
                continue;
            if (!match || seen == std::max(r.fieldIndex, 0))
                match = g;
            if (seen++ == std::max(r.fieldIndex, 0))
                break;
        }
        if (!match) {
            if (error)
                *error = "no grid '" + r.fieldName + "' in " + r.filePath;
            return false;
        }
        return convertVdb(*match, out, error);
    } catch (const std::exception &e) {
        if (error)
            *error = r.filePath + ": " + e.what();
        return false;
    }
}

class Bridge final : public nova::VolumeBridge {
public:
    Bridge() { openvdb::initialize(); }

    bool load(const nova::VolumeFieldRequest &r, nova::VolumeFieldGrids &out, std::string *error) override {
        std::lock_guard lock(m_mutex);
        out.grids.clear();
        void *p = nullptr;
        if (m_sopVol.vdb)
            p = r.houdiniField ? m_sopVol.houdini(r.filePath.c_str(), r.fieldName.c_str(), r.fieldIndex)
                               : m_sopVol.vdb(r.filePath.c_str(), r.fieldName.c_str(), r.fieldIndex);
        if (!p) {
            if (!r.houdiniField && endsWith(r.filePath, ".vdb"))
                return loadVdbFile(r, out, error);
            if (error)
                *error = m_sopVol.vdb ? "Houdini cannot resolve field '" + r.fieldName + "' of " + r.filePath
                                      : "Houdini's USD_SopVol.dll is not available to read " + r.filePath;
            return false;
        }
        const auto *gt = static_cast<const GT_Primitive *>(p);
        try {
            if (gt->getPrimitiveType() == GT_PRIM_VDB_VOLUME) {
                const openvdb::GridBase *grid = static_cast<const GT_PrimVDB *>(gt)->getGrid();
                if (!grid) {
                    if (error)
                        *error = "an empty VDB primitive for " + r.fieldName;
                    return false;
                }
                return convertVdb(*grid, out, error);
            }
            if (gt->getPrimitiveType() == GT_PRIM_VOXEL_VOLUME) {
                const auto *volume = dynamic_cast<const GEO_PrimVolume *>(
                    static_cast<const GT_PrimVolume *>(gt)->getGeoPrimitive());
                if (!volume) {
                    if (error)
                        *error = "a volume primitive without voxels for " + r.fieldName;
                    return false;
                }
                return convertNative(*volume, out, error);
            }
        } catch (const std::exception &e) {
            if (error)
                *error = r.fieldName + ": " + e.what();
            out.grids.clear();
            return false;
        }
        if (error)
            *error = std::string("unexpected primitive type ") + gt->className() + " for " + r.fieldName;
        return false;
    }

private:
    std::mutex m_mutex;
    SopVol m_sopVol;
};

} // namespace

extern "C" __declspec(dllexport) nova::VolumeBridge *novaVolumeBridge() {
    static Bridge bridge;
    return &bridge;
}

// For tests in hython (ctypes): loads a field and reports, over its first
// grid's active voxels, out[0] the grid count, out[1] the voxel count, out[2]
// the minimum, out[3] the maximum, out[4] the sum; out[5..16] the first
// grid's index-to-object map (x, y, z, t). Returns 1, or 0 with `error`.
extern "C" __declspec(dllexport) int novaVolumeFieldStats(const char *path, const char *name, int index,
                                                         int houdiniField, double *out, char *error, int errorSize) {
    nova::VolumeFieldRequest r;
    r.filePath = path ? path : "";
    r.fieldName = name ? name : "";
    r.fieldIndex = index;
    r.houdiniField = houdiniField != 0;
    nova::VolumeFieldGrids grids;
    std::string message;
    std::fill(out, out + 17, 0.0);
    if (!novaVolumeBridge()->load(r, grids, &message) || grids.grids.empty()) {
        if (error && errorSize > 0)
            std::snprintf(error, size_t(errorSize), "%s", message.c_str());
        return 0;
    }
    out[0] = double(grids.grids.size());
    const nanovdb::FloatGrid &g = *grids.grids[0]->grid();
    double lo = 1e300, hi = -1e300, sum = 0.0, count = 0.0;
    const auto &tree = g.tree();
    const auto *leaf = tree.getFirstLeaf();
    for (uint32_t i = 0; i < tree.nodeCount(0); ++i, ++leaf)
        for (uint32_t n = 0; n < 512; ++n)
            if (leaf->isActive(n)) {
                const double v = leaf->getValue(n);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += v;
                count += 1.0;
            }
    out[1] = count;
    out[2] = count > 0.0 ? lo : 0.0;
    out[3] = count > 0.0 ? hi : 0.0;
    out[4] = sum;
    const nova::Affine3f m = grids.grids[0]->indexToObject();
    const nova::Vec3f axes[4] = {m.x, m.y, m.z, m.t};
    for (int a = 0; a < 4; ++a) {
        out[5 + a * 3] = axes[a].x;
        out[6 + a * 3] = axes[a].y;
        out[7 + a * 3] = axes[a].z;
    }
    return 1;
}
