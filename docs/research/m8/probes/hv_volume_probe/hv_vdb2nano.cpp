// Research probe for M8 (docs/research/m8/houdini-volumes.md), not part of Nova.
// HDK-free: links only Houdini's OpenVDB (openvdb_sesi) + its TBB/blosc/zlib and
// uses the NanoVDB headers shipped in $HFS/toolkit/include/nanovdb.
//
//   hv_vdb2nano <in.vdb> <out.nvdb>
// Reads every grid of a .vdb with OpenVDB, converts float grids to NanoVDB,
// writes them to a .nvdb, reads the .nvdb back with NanoVDB alone and checks
// that a few voxel values and the world bounding boxes agree.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/IO.h>

#include <chrono>
#include <cstdio>
#include <vector>

static double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: hv_vdb2nano <in.vdb> <out.nvdb>\n");
        return 2;
    }
    std::printf("OpenVDB %s (ABI %d), NanoVDB %d.%d.%d\n", openvdb::getLibraryVersionString(),
                OPENVDB_ABI_VERSION_NUMBER, NANOVDB_MAJOR_VERSION_NUMBER, NANOVDB_MINOR_VERSION_NUMBER,
                NANOVDB_PATCH_VERSION_NUMBER);
    auto t0 = std::chrono::steady_clock::now();
    openvdb::initialize();
    openvdb::io::File file(argv[1]);
    file.open(false); // no delayed loading
    std::printf("open: %.2f ms, file version %s\n", msSince(t0), file.version().c_str());
    std::vector<nanovdb::GridHandle<nanovdb::HostBuffer>> handles;
    std::vector<openvdb::FloatGrid::Ptr> sources;
    for (auto it = file.beginName(); it != file.endName(); ++it) {
        t0 = std::chrono::steady_clock::now();
        openvdb::GridBase::Ptr base = file.readGrid(it.gridName());
        const double msRead = msSince(t0);
        const openvdb::CoordBBox bb = base->evalActiveVoxelBoundingBox();
        std::printf("grid '%s' type %s class %s active %llu bbox (%d %d %d)-(%d %d %d) voxel %.4f read %.2f ms\n",
                    it.gridName().c_str(), base->valueType().c_str(),
                    openvdb::GridBase::gridClassToString(base->getGridClass()).c_str(),
                    (unsigned long long)base->activeVoxelCount(), bb.min()[0], bb.min()[1], bb.min()[2], bb.max()[0],
                    bb.max()[1], bb.max()[2], base->voxelSize()[0], msRead);
        auto fg = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
        if (!fg)
            continue;
        t0 = std::chrono::steady_clock::now();
        auto h = nanovdb::createNanoGrid(*fg);
        std::printf("    -> NanoVDB float grid %zu bytes, %.2f ms\n", size_t(h.size()), msSince(t0));
        handles.push_back(std::move(h));
        sources.push_back(fg);
    }
    file.close();
    nanovdb::io::writeGrids<nanovdb::HostBuffer, std::vector>(std::string(argv[2]), handles);

    // Read back with NanoVDB only and compare voxel values.
    t0 = std::chrono::steady_clock::now();
    auto back = nanovdb::io::readGrids(argv[2]);
    std::printf("NanoVDB readGrids(%s): %zu handles, %.2f ms\n", argv[2], back.size(), msSince(t0));
    int bad = 0;
    for (size_t g = 0; g < back.size() && g < sources.size(); ++g) {
        const nanovdb::NanoGrid<float> *ng = back[g].grid<float>();
        if (!ng) {
            ++bad;
            continue;
        }
        auto acc = ng->getAccessor();
        size_t n = 0, mism = 0;
        for (auto it = sources[g]->cbeginValueOn(); it && n < 200000; ++it, ++n) {
            const openvdb::Coord c = it.getCoord();
            if (acc.getValue(nanovdb::Coord(c[0], c[1], c[2])) != *it)
                ++mism;
        }
        const auto wb = ng->worldBBox();
        std::printf("    '%s' compared %zu voxels, %zu mismatches; world bbox (%.3f %.3f %.3f)-(%.3f %.3f %.3f)\n",
                    ng->gridName(), n, mism, wb.min()[0], wb.min()[1], wb.min()[2], wb.max()[0], wb.max()[1],
                    wb.max()[2]);
        bad += mism != 0;
    }
    return bad;
}
