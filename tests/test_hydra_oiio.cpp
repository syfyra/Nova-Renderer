// OIIO TextureSystem inside hdNova (M3.0 spike S3, docs/design/M3.md K9):
// Nova's texture system is private (its options, e.g. flip_t, must not leak
// into Houdini's shared TextureSystem) but reads through the shared
// ImageCache, so husk's cache limits apply; lookups run concurrently with one
// Perthread record per thread.

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/texture.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A tiled, MIP-mapped half-float EXR: the top half of the image red, the
// bottom half blue (OIIO's image rows run top to bottom).
std::string makeTestTexture() {
    const fs::path dir = fs::temp_directory_path() / "nova_test_oiio";
    fs::create_directories(dir);
    const std::string path = (dir / "halves.exr").generic_string();
    OIIO::ImageBuf src(OIIO::ImageSpec(64, 64, 3, OIIO::TypeDesc::FLOAT));
    const float red[3] = {1.0f, 0.0f, 0.0f}, blue[3] = {0.0f, 0.0f, 1.0f};
    OIIO::ImageBufAlgo::fill(src, red, OIIO::ROI(0, 64, 0, 32, 0, 1, 0, 3));
    OIIO::ImageBufAlgo::fill(src, blue, OIIO::ROI(0, 64, 32, 64, 0, 1, 0, 3));
    OIIO::ImageSpec config;
    config.tile_width = config.tile_height = 16;
    config.set_format(OIIO::TypeDesc::HALF);
    config.attribute("compression", "zip");
    REQUIRE(OIIO::ImageBufAlgo::make_texture(OIIO::ImageBufAlgo::MakeTxTexture, src, path, config));
    return path;
}

} // namespace

TEST_CASE("A private TextureSystem on the shared ImageCache keeps its options", "[hydra][oiio]") {
    const std::string path = makeTestTexture();

    OIIO::ImageCache *shared = OIIO::ImageCache::create(true);
    OIIO::TextureSystem *ts = OIIO::TextureSystem::create(false, shared);
    REQUIRE(ts);
    CHECK(ts->imagecache() == shared);
    ts->attribute("flip_t", 1);      // t = 0 at the bottom, as USD/MaterialX texture coordinates
    ts->attribute("gray_to_rgb", 1);

    OIIO::TextureSystem *sharedTs = OIIO::TextureSystem::create(true);
    int sharedFlip = -1;
    CHECK(sharedTs->getattribute("flip_t", sharedFlip));
    CHECK(sharedFlip == 0);
    OIIO::TextureSystem::destroy(sharedTs);

    // The converted file is what the renderer will read: tiled and MIP-mapped.
    {
        OIIO::ImageCache::Perthread *pt = shared->get_perthread_info();
        OIIO::ImageCache::ImageHandle *h = shared->get_image_handle(OIIO::ustring(path), pt);
        REQUIRE(h);
        int subimages = 0, miplevels = 0;
        CHECK(shared->get_image_info(h, pt, 0, 0, OIIO::ustring("subimages"), OIIO::TypeInt, &subimages));
        CHECK(shared->get_image_info(h, pt, 0, 0, OIIO::ustring("miplevels"), OIIO::TypeInt, &miplevels));
        CHECK(subimages == 1);
        CHECK(miplevels == 7); // 64 .. 1
    }

    // Concurrent lookups, one Perthread record and handle per thread, as the
    // shade stage will do per TBB worker.
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int k = 0; k < 4; ++k)
        threads.emplace_back([&, k] {
            OIIO::TextureSystem::Perthread *pt = ts->get_perthread_info(nullptr);
            OIIO::TextureSystem::TextureHandle *h = ts->get_texture_handle(OIIO::ustring(path), pt);
            OIIO::TextureOpt opt;
            opt.swrap = opt.twrap = OIIO::TextureOpt::WrapClamp;
            for (int i = 0; i < 1000; ++i) {
                const float s = (i % 64 + 0.5f) / 64.0f;
                float lower[3] = {-1, -1, -1}, upper[3] = {-1, -1, -1};
                // Zero derivatives: the finest level, bilinear. Away from the
                // seam the halves are exact.
                if (!h || !ts->texture(h, pt, opt, s, 0.2f + 0.001f * k, 0, 0, 0, 0, 3, lower) ||
                    !ts->texture(h, pt, opt, s, 0.8f, 0, 0, 0, 0, 3, upper))
                    ++wrong;
                else if (!(lower[0] == 0 && lower[2] == 1 && upper[0] == 1 && upper[2] == 0))
                    ++wrong;
            }
        });
    for (std::thread &t : threads)
        t.join();
    CHECK(wrong == 0);

    OIIO::TextureSystem::destroy(ts);
    std::error_code ec;
    fs::remove(path, ec);
}
