#pragma once

// Volume fields (docs/design/M8.md D2): every field is a NanoVDB float grid
// in one pointer-free buffer, shared and immutable once filled. The same
// bytes upload unchanged to the GPU (M10). Lookups are trilinear with voxel
// centres at integer index coordinates and the grid's background (0 for fog
// volumes) outside its stored values, as Karma's (research H 4.2).
//
// Header-only, so the HDK-linked volume bridge (houdini/volume_bridge) can
// fill grids without linking nova_core.

#include "core/scene/motion.h"

#include <nanovdb/NanoVDB.h>
#include <nanovdb/util/SampleFromVoxels.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>

namespace nova {

class VolumeGrid {
public:
    // A buffer of `size` bytes, aligned for NanoVDB, to fill with a grid.
    static std::shared_ptr<VolumeGrid> allocate(size_t size) {
        void *p = ::operator new(size, std::align_val_t(NANOVDB_DATA_ALIGNMENT));
        return std::shared_ptr<VolumeGrid>(new VolumeGrid(p, size));
    }
    // A copy of a NanoVDB grid buffer; null unless it holds a float grid.
    static std::shared_ptr<const VolumeGrid> copyOf(const void *data, size_t size) {
        if (!data || size < sizeof(nanovdb::GridData))
            return nullptr;
        std::shared_ptr<VolumeGrid> g = allocate(size);
        std::memcpy(g->data(), data, size);
        if (!g->grid())
            return nullptr;
        return g;
    }

    VolumeGrid(const VolumeGrid &) = delete;
    VolumeGrid &operator=(const VolumeGrid &) = delete;
    ~VolumeGrid() { ::operator delete(m_data, std::align_val_t(NANOVDB_DATA_ALIGNMENT)); }

    void *data() { return m_data; }
    const void *data() const { return m_data; }
    size_t size() const { return m_size; }

    // The float grid, or null if the buffer holds none.
    const nanovdb::FloatGrid *grid() const {
        const auto *g = static_cast<const nanovdb::GridData *>(m_data);
        if (m_size < sizeof(nanovdb::GridData) || !g->isValid() || g->mGridType != nanovdb::GridType::Float ||
            g->mGridSize > m_size)
            return nullptr;
        return reinterpret_cast<const nanovdb::FloatGrid *>(m_data);
    }

    // The grid's index-to-object map (its NanoVDB map, double precision).
    Affine3f indexToObject() const {
        Affine3f a;
        const nanovdb::FloatGrid *g = grid();
        if (!g)
            return a;
        const nanovdb::Map &m = g->map();
        const nanovdb::Vec3d o = m.applyMap(nanovdb::Vec3d(0.0));
        const nanovdb::Vec3d x = m.applyJacobian(nanovdb::Vec3d(1.0, 0.0, 0.0));
        const nanovdb::Vec3d y = m.applyJacobian(nanovdb::Vec3d(0.0, 1.0, 0.0));
        const nanovdb::Vec3d z = m.applyJacobian(nanovdb::Vec3d(0.0, 0.0, 1.0));
        a.x = {float(x[0]), float(x[1]), float(x[2])};
        a.y = {float(y[0]), float(y[1]), float(y[2])};
        a.z = {float(z[0]), float(z[1]), float(z[2])};
        a.t = {float(o[0]), float(o[1]), float(o[2])};
        return a;
    }

private:
    VolumeGrid(void *data, size_t size) : m_data(data), m_size(size) {}
    void *m_data;
    size_t m_size;
};

using VolumeGridPtr = std::shared_ptr<const VolumeGrid>;

// Trilinear lookups in a grid's index space. NanoVDB's accessor caches the
// path to the last node: one sampler per thread, never shared.
class VolumeSampler {
public:
    explicit VolumeSampler(const nanovdb::FloatGrid &grid) : m_acc(grid.getAccessor()), m_sample(m_acc) {}
    VolumeSampler(const VolumeSampler &) = delete;
    VolumeSampler &operator=(const VolumeSampler &) = delete;

    // The value at index-space point p (voxel centres at integers).
    float operator()(Vec3f p) const { return float(m_sample(nanovdb::Vec3f(p.x, p.y, p.z))); }
    // Whether a lookup at p reaches a voxel of the grid: one of the eight
    // voxels it interpolates is active (VolumeRecord::maskFields).
    bool reaches(Vec3f p) const {
        const int x = int(std::floor(p.x)), y = int(std::floor(p.y)), z = int(std::floor(p.z));
        for (int k = 0; k < 8; ++k)
            if (m_acc.isActive(nanovdb::Coord(x + (k & 1), y + ((k >> 1) & 1), z + (k >> 2))))
                return true;
        return false;
    }
    float voxel(int x, int y, int z) const { return m_acc.getValue(nanovdb::Coord(x, y, z)); }

private:
    using Accessor = nanovdb::DefaultReadAccessor<float>;
    Accessor m_acc;
    nanovdb::SampleFromVoxels<Accessor, 1, true> m_sample;
};

} // namespace nova
