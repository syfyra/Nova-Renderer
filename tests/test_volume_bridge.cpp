// The volume bridge (docs/design/M8.md D1): nova_volume.dll, loaded at run
// time as hdNova loads it, reads the fields of a pyro .vdb (density,
// temperature, flame; fp16 on disk) into NanoVDB float grids with the
// file's voxel counts and transform, and reports what it cannot read.

#include "core/volume/bridge.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// A .vdb cut off inside its header (a cache whose write was interrupted), or
// of a file version newer than the library reads, is refused before OpenVDB
// sees it: OpenVDB reads the header's lengths without checking its stream
// and then allocates and loops for minutes (a file of 64 bytes did not come
// back within a minute, one of 1 KB after 22 s).
TEST_CASE("The volume bridge refuses .vdb files with an incomplete header", "[volume][bridge][robust]") {
    std::ifstream in(kPyro, std::ios::binary);
    const std::string whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(whole.size() > 4096);
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("nova_vdb_header_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const auto write = [&](const char *name, const std::string &bytes) {
        const std::filesystem::path path = dir / name;
        std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
        return path.string();
    };
    nova::VolumeFieldRequest r;
    r.fieldName = "density";
    nova::VolumeFieldGrids out;
    std::string error;
    const auto seconds = [&] {
        const auto start = std::chrono::steady_clock::now();
        const bool loaded = bridge()->load(r, out, &error);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        CHECK_FALSE(loaded);
        return s;
    };

    for (const size_t size : {size_t(12), size_t(40), size_t(64), size_t(200), size_t(1024)}) {
        INFO(size << " bytes");
        r.filePath = write("cut.vdb", whole.substr(0, size));
        error.clear();
        CHECK(seconds() < 2.0);
        CHECK(error.find("ends inside its header") != std::string::npos);
        CHECK(out.grids.empty());
    }

    // A file version beyond the library's.
    std::string newer = whole;
    const uint32_t version = 100000;
    std::memcpy(newer.data() + 8, &version, 4);
    r.filePath = write("newer.vdb", newer);
    error.clear();
    CHECK(seconds() < 2.0);
    CHECK(error.find("newer") != std::string::npos);

    // Files cut off behind the header are the readers' to report, and the
    // whole file still loads.
    r.filePath = write("half.vdb", whole.substr(0, whole.size() / 2));
    error.clear();
    CHECK(seconds() < 10.0);
    CHECK(error.find("ends inside its header") == std::string::npos);
    r.filePath = write("whole.vdb", whole);
    CHECK(bridge()->load(r, out, &error));
    CHECK(out.grids.size() == 1);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
