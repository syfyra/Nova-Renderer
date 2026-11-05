// M8.0 volume data (docs/design/M8.md D2, D6, D7): NanoVDB grids sample
// trilinearly with voxel centres at integers, majorant cells bound every
// trilinear value of a field (also of a field in another index space), and
// the scene keeps volumes with their cells, bounds and motion.

#if defined(_MSC_VER)
#pragma warning(disable : 4702) // unreachable code in NanoVDB's grid builder templates
#endif

#include "core/scene/scene.h"
#include "core/volume/majorant.h"

#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/GridBuilder.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>

using Catch::Matchers::WithinAbs;
using namespace nova;

namespace {

// A fog grid of f(ijk) over [0, n)^3 (values equal to the background are not
// stored), voxel size `voxel`, voxel (0,0,0) at `origin`.
template <typename F>
VolumeGridPtr makeGrid(F f, int n, double voxel, nanovdb::Vec3d origin = nanovdb::Vec3d(0.0)) {
    nanovdb::build::Grid<float> g(0.0f, "density", nanovdb::GridClass::FogVolume);
    g.setTransform(voxel, origin);
    g([&](const nanovdb::Coord &ijk) { return f(ijk[0], ijk[1], ijk[2]); },
      nanovdb::CoordBBox(nanovdb::Coord(0), nanovdb::Coord(n - 1)));
    const nanovdb::GridHandle<nanovdb::HostBuffer> handle = nanovdb::createNanoGrid(g);
    return VolumeGrid::copyOf(handle.data(), handle.size());
}

// Smoke-like: three soft blobs with a hashed per-voxel variation, zero
// outside them (sparse).
float smoke(int x, int y, int z) {
    const float blobs[3][4] = {{30, 30, 30, 12}, {60, 40, 50, 9}, {45, 70, 35, 14}};
    float d = 0.0f;
    for (const auto &b : blobs) {
        const float dx = float(x) - b[0], dy = float(y) - b[1], dz = float(z) - b[2];
        d += 2.0f * std::exp(-(dx * dx + dy * dy + dz * dz) / (2.0f * b[3] * b[3]));
    }
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    const float noise = float(h >> 8) / 16777216.0f;
    const float v = d * (0.6f + 0.8f * noise);
    return v > 0.05f ? v : 0.0f;
}

Mat4 translation(float x, float y, float z) {
    Mat4 m = Mat4::identity();
    m.m[3][0] = x;
    m.m[3][1] = y;
    m.m[3][2] = z;
    return m;
}

} // namespace

TEST_CASE("Volume grids sample trilinearly with voxel centres at integers", "[volume]") {
    const VolumeGridPtr grid = makeGrid([](int x, int y, int z) { return 1.0f + 0.25f * x + 0.5f * y + 0.75f * z; },
                                        16, 0.1, nanovdb::Vec3d(1.0, 2.0, 3.0));
    REQUIRE(grid);
    REQUIRE(grid->grid());
    const Affine3f m = grid->indexToObject();
    CHECK_THAT(m.x.x, WithinAbs(0.1, 1e-7));
    CHECK_THAT(m.y.y, WithinAbs(0.1, 1e-7));
    CHECK_THAT(m.t.x, WithinAbs(1.0, 1e-6));
    CHECK_THAT(m.t.z, WithinAbs(3.0, 1e-6));
    CHECK_THAT(m.x.y, WithinAbs(0.0, 1e-9));

    // Trilinear interpolation reproduces a linear field exactly.
    VolumeSampler sample(*grid->grid());
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.0f, 15.0f);
    for (int i = 0; i < 1000; ++i) {
        const Vec3f p{u(rng), u(rng), u(rng)};
        CHECK_THAT(sample(p), WithinAbs(1.0 + 0.25 * p.x + 0.5 * p.y + 0.75 * p.z, 2e-4));
    }
    CHECK_THAT(sample({4.0f, 5.0f, 6.0f}), WithinAbs(1.0 + 1.0 + 2.5 + 4.5, 1e-5)); // a voxel centre
    // Outside the stored values: the background, blending in over one voxel.
    CHECK(sample({-2.0f, 5.0f, 5.0f}) == 0.0f);
    CHECK_THAT(sample({-0.5f, 5.0f, 5.0f}), WithinAbs(0.5 * (1.0 + 2.5 + 3.75), 1e-4));

    // An invalid buffer is no grid.
    const unsigned char junk[64] = {};
    CHECK_FALSE(VolumeGrid::copyOf(junk, sizeof(junk)));
}

TEST_CASE("Majorant cells bound every trilinear value of a field", "[volume][majorant]") {
    const VolumeGridPtr ref = makeGrid(smoke, 96, 0.02);
    REQUIRE(ref);
    const nanovdb::FloatGrid &grid = *ref->grid();
    Vec3f lower, upper;
    REQUIRE(fieldSupport(grid, Affine3f{}, lower, upper));
    const MajorantLayout layout = majorantLayout(lower, upper);
    REQUIRE_FALSE(layout.empty());
    CHECK(layout.k == MajorantLayout::kBaseCellSize);
    CellRanges ranges;
    fieldCellRanges(grid, Affine3f{}, layout, ranges);
    REQUIRE(ranges.hi.size() == layout.cellCount());

    // A second field at twice the voxel size, offset by a third of a voxel:
    // its ranges in the reference's cells.
    const VolumeGridPtr coarse =
        makeGrid([](int x, int y, int z) { return smoke(2 * x + 1, 2 * y, 2 * z + 1); }, 48, 0.04,
                 nanovdb::Vec3d(0.013, -0.007, 0.0));
    REQUIRE(coarse);
    const Affine3f coarseToRef = compose(coarse->indexToObject(), inverseAffine(ref->indexToObject()));
    const Affine3f refToCoarse = inverseAffine(coarseToRef);
    CellRanges coarseRanges;
    fieldCellRanges(*coarse->grid(), coarseToRef, layout, coarseRanges);

    VolumeSampler sampleRef(grid), sampleCoarse(*coarse->grid());
    std::mt19937 rng(11);
    const Vec3f a = layout.lower(), b = layout.upper();
    std::uniform_real_distribution<float> ux(a.x, b.x), uy(a.y, b.y), uz(a.z, b.z);
    int violations = 0, coarseViolations = 0, nonZero = 0;
    double sumValue = 0.0, sumBound = 0.0;
    for (int i = 0; i < 400000; ++i) {
        const Vec3f p{ux(rng), uy(rng), uz(rng)};
        int32_t c[3];
        if (!layout.cellOf(p, c))
            continue;
        const size_t cell = layout.index(c[0], c[1], c[2]);
        const float v = sampleRef(p);
        violations += v > ranges.hi[cell] + 1e-6f || v < ranges.lo[cell] - 1e-6f;
        const float w = sampleCoarse(refToCoarse.point(p));
        coarseViolations += w > coarseRanges.hi[cell] + 1e-6f || w < coarseRanges.lo[cell] - 1e-6f;
        nonZero += v > 0.0f;
        sumValue += v;
        sumBound += ranges.hi[cell];
    }
    CHECK(violations == 0);
    CHECK(coarseViolations == 0);
    CHECK(nonZero > 10000);
    // Tight enough to be worth it: the mean bound within a few times the mean.
    CHECK(sumBound < 6.0 * sumValue);

    // Outside the support every value is the background.
    CHECK(sampleRef({lower.x - 0.5f, 40.0f, 40.0f}) == 0.0f);

    // Large domains grow the cells to at most 128^3 of them.
    const MajorantLayout big = majorantLayout({0.0f, 0.0f, 0.0f}, {4096.0f, 4096.0f, 2048.0f});
    CHECK(big.cellCount() <= MajorantLayout::kMaxCells);
    CHECK(big.k == 32);
    CHECK(majorantLayout({1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}).empty());
}

TEST_CASE("Scene volumes have majorants, bounds and motion", "[volume]") {
    Scene scene;
    const VolumeGridPtr grid = makeGrid(smoke, 96, 0.02);
    VolumeDesc desc;
    desc.fields.push_back({"density", "density", grid, Affine3f{}});
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    scene.updateVolumeMotion(id, {translation(5.0f, 0.0f, 0.0f)});
    scene.commit();

    REQUIRE(scene.activeVolumes() == std::vector<VolumeId>{id});
    const VolumeRecord &rec = scene.volumeRecord(id);
    CHECK(rec.reference == 0);
    CHECK(rec.majorant.size() == rec.layout.cellCount());
    float maxMajorant = 0.0f;
    for (float m : rec.majorant)
        maxMajorant = std::max(maxMajorant, m);
    CHECK(maxMajorant > 1.0f);
    // The cells' box, 0.02 per voxel, moved by 5 in x.
    const Vec3f lo = rec.layout.lower() * 0.02f, hi = rec.layout.upper() * 0.02f;
    CHECK_THAT(rec.lower.x, WithinAbs(lo.x + 5.0f, 1e-4));
    CHECK_THAT(rec.upper.x, WithinAbs(hi.x + 5.0f, 1e-4));
    CHECK_THAT(rec.lower.y, WithinAbs(lo.y, 1e-4));
    CHECK(scene.sceneRadius() > 0.5f * length(hi - lo) - 1e-3f); // volumes count for the scene size
    CHECK_THAT(scene.volumeToWorld(id, 0.5f).t.x, WithinAbs(5.0, 1e-6));

    SECTION("moving volumes cover the shutter") {
        scene.updateVolumeMotion(id, {translation(0.0f, 0.0f, 0.0f), translation(10.0f, 0.0f, 0.0f)});
        scene.commit();
        const VolumeRecord &moved = scene.volumeRecord(id);
        CHECK_THAT(moved.lower.x, WithinAbs(lo.x, 1e-3));
        CHECK_THAT(moved.upper.x, WithinAbs(hi.x + 10.0f, 1e-3));
        CHECK_THAT(scene.volumeToWorld(id, 0.25f).t.x, WithinAbs(2.5, 1e-5));
    }
    SECTION("hidden and removed volumes are not active") {
        const uint64_t version = scene.version();
        scene.setVolumeVisible(id, false);
        CHECK(scene.version() > version);
        scene.commit();
        CHECK(scene.activeVolumes().empty());
        scene.setVolumeVisible(id, true);
        scene.commit();
        CHECK(scene.activeVolumes().size() == 1);
        scene.removeVolume(id);
        scene.commit();
        CHECK(scene.activeVolumes().empty());
        CHECK(scene.createVolume() == id); // ids are reused
    }
    SECTION("a volume without density renders nothing") {
        VolumeDesc other;
        other.fields.push_back({"temperature", "temperature", grid, Affine3f{}});
        scene.updateVolume(id, other);
        scene.commit();
        CHECK(scene.activeVolumes().empty());
        CHECK(scene.volumeRecord(id).empty());
    }
}
