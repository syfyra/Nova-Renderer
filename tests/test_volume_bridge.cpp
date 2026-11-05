// The volume bridge (docs/design/M8.md D1): nova_volume.dll, loaded at run
// time as hdNova loads it, reads the fields of a pyro .vdb (density,
// temperature, flame; fp16 on disk) into NanoVDB float grids with the
// file's voxel counts and transform, and reports what it cannot read.

#include "core/volume/bridge.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <windows.h>

#include <string>

using Catch::Matchers::WithinAbs;

namespace {

nova::VolumeBridge *bridge() {
    static nova::VolumeBridge *b = [] {
        HMODULE dll = LoadLibraryA(NOVA_VOLUME_DLL);
        REQUIRE(dll);
        auto factory = reinterpret_cast<nova::VolumeBridgeFactory>(GetProcAddress(dll, nova::kVolumeBridgeEntry));
        REQUIRE(factory);
        return factory();
    }();
    return b;
}

const std::string kPyro = std::string(NOVA_TEST_DIR) + "/data/volumes/pyro.vdb";

uint64_t activeVoxels(const nanovdb::FloatGrid &g) {
    uint64_t n = 0;
    const auto &tree = g.tree();
    const auto *leaf = tree.getFirstLeaf();
    for (uint32_t i = 0; i < tree.nodeCount(0); ++i, ++leaf)
        for (uint32_t k = 0; k < 512; ++k)
            n += leaf->isActive(k);
    return n;
}

} // namespace

TEST_CASE("The volume bridge reads the fields of a .vdb file", "[volume][bridge]") {
    // Active voxels per grid (research H 3.4, measured with OpenVDB).
    const struct {
        const char *name;
        uint64_t active;
    } expected[] = {{"density", 40641}, {"flame", 35716}, {"temperature", 53038}};
    for (const auto &e : expected) {
        nova::VolumeFieldRequest r;
        r.filePath = kPyro;
        r.fieldName = e.name;
        nova::VolumeFieldGrids out;
        std::string error;
        INFO(e.name);
        REQUIRE(bridge()->load(r, out, &error));
        INFO(error);
        REQUIRE(out.grids.size() == 1);
        const nanovdb::FloatGrid *g = out.grids[0]->grid();
        REQUIRE(g);
        CHECK(activeVoxels(*g) == e.active);
        // 0.05 per voxel, axis-aligned.
        const nova::Affine3f m = out.grids[0]->indexToObject();
        CHECK_THAT(m.x.x, WithinAbs(0.05, 1e-6));
        CHECK_THAT(m.y.y, WithinAbs(0.05, 1e-6));
        CHECK_THAT(m.z.z, WithinAbs(0.05, 1e-6));
        CHECK_THAT(m.x.y, WithinAbs(0.0, 1e-9));
        CHECK(g->tree().background() == 0.0f);
    }
}

TEST_CASE("The volume bridge reports fields it cannot read", "[volume][bridge]") {
    nova::VolumeFieldGrids out;
    std::string error;
    nova::VolumeFieldRequest r;
    r.filePath = kPyro;
    r.fieldName = "nosuchgrid";
    CHECK_FALSE(bridge()->load(r, out, &error));
    CHECK_FALSE(error.empty());
    CHECK(out.grids.empty());
    error.clear();
    r.filePath = std::string(NOVA_TEST_DIR) + "/data/volumes/missing.vdb";
    r.fieldName = "density";
    CHECK_FALSE(bridge()->load(r, out, &error));
    CHECK_FALSE(error.empty());
}
