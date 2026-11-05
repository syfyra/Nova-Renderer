// M6.5: cryptomatte ids and coverage (docs/design/M6.md D10). The hash
// vectors are from Karma's files (docs/research/m6/cryptomatte.md 3).

#include "test_helpers.h"

#include "core/film/crypto.h"
#include "core/geometry/curves.h"
#include "core/integrator/renderer.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>
#include <memory>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("Cryptomatte ids are MurmurHash3 with the exponent fixup", "[crypto]") {
    // Layer keys: the raw hash (its first 7 hex digits name the layer).
    CHECK(murmurHash3("CryptoPrimitives", 16) == 0xb5b7a6bcu);
    // Names, as Karma's manifests store them.
    CHECK(cryptoHash("/World/mtl/red") == 0xf13a94bfu);
    CHECK(cryptoHash("/World/mtl/blue") == 0x9c9743fau);
    CHECK(cryptoHash("/World/geo/ballA") == 0xaabbd798u);
    CHECK(cryptoHash("alpha") == 0xa9c0133du);
    CHECK(cryptoHash("42") == 0xbc58a436u);
    CHECK(cryptoHash("-3") == 0x1826e92fu);
    // Exponent 0 before the fixup: bit 23 flipped.
    CHECK(murmurHash3("/World/inst/protos/rock", 23) == 0x8002046eu);
    CHECK(cryptoHash("/World/inst/protos/rock") == 0x8082046eu);
    // The preview colour's channels.
    const Vec3f c = cryptoColor(0x80000000u);
    CHECK(c.x == 0.5f);
    CHECK(c.y == 0.0f);
}

TEST_CASE("Cryptomatte coverage counts the first surface of every sample", "[crypto][render]") {
    // Two quads side by side (objects "left" and "right", materials "a" and
    // "b"), the right one with a per-face primvar; seen edge-on to the seam.
    Scene scene;
    MeshDesc left = makeQuad(2.0f), right = makeQuad(2.0f);
    left.transform = translation({-2.0f, 0.0f, 0.0f});
    right.transform = translation({2.0f, 0.0f, 0.0f});
    right.triangleFaces = {0, 1}; // two faces, one triangle each
    const MeshId l = scene.createMesh(), r = scene.createMesh();
    scene.updateMesh(l, left);
    scene.updateMesh(r, right);
    const MaterialId ma = scene.createMaterial(), mb = scene.createMaterial();
    MaterialDesc a = defaultMaterial(), b = defaultMaterial();
    a.cryptoId = cryptoHash("a");
    b.cryptoId = cryptoHash("b");
    scene.updateMaterial(ma, a);
    scene.updateMaterial(mb, b);
    scene.setMeshMaterials(l, {ma});
    scene.setMeshMaterials(r, {mb});
    scene.setMeshCrypto(l, cryptoHash("left"), {});
    scene.setMeshCrypto(r, cryptoHash("right"), {CryptoAttribute{"piece", true, {cryptoHash("p0"), cryptoHash("p1")}}});
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 32;
    settings.maxDepth = 1;
    settings.crypto = std::make_shared<const std::vector<CryptoRequest>>(std::vector<CryptoRequest>{
        {CryptoSource::Object, {}}, {CryptoSource::Material, {}}, {CryptoSource::Primvar, "piece"}});
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, 8, 8)); // [-2, 2]^2, the seam between pixels 3 and 4
    renderer.resize(8, 8);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    REQUIRE(film.crypto.size() == 3);
    const auto coverage = [&](int plane, int x, int y) {
        std::map<uint32_t, float> m;
        const size_t px = size_t(y) * 8 + size_t(x);
        for (int s = 0; s < kCryptoSlots; ++s)
            if (film.crypto[size_t(plane)].weights[px * kCryptoSlots + s] > 0.0f)
                m[film.crypto[size_t(plane)].ids[px * kCryptoSlots + s]] =
                    film.crypto[size_t(plane)].weights[px * kCryptoSlots + s] / float(film.samples);
        return m;
    };
    // Inside each quad (away from the image border): one id with full coverage.
    CHECK(coverage(0, 1, 4) == std::map<uint32_t, float>{{cryptoHash("left"), 1.0f}});
    CHECK(coverage(1, 6, 4) == std::map<uint32_t, float>{{cryptoHash("b"), 1.0f}});
    // At the seam (filter footprint): both, summing to the alpha.
    const auto seam = coverage(0, 4, 4);
    REQUIRE(seam.size() == 2);
    float sum = 0.0f;
    for (const auto &[id, w] : seam)
        sum += w;
    CHECK_THAT(sum, WithinAbs(film.alphaSum[4 * 8 + 4] / float(film.samples), 1e-6));
    // The per-face primvar: the lower-right triangle is face 0, the upper-left face 1.
    CHECK(coverage(2, 7, 0).count(cryptoHash("p0")) == 1);
    CHECK(coverage(2, 5, 7).count(cryptoHash("p1")) == 1);
    // The left quad has no such primvar: no coverage.
    CHECK(coverage(2, 1, 4).empty());
}

TEST_CASE("Cryptomatte of a per-curve primvar takes each curve's id", "[crypto][render][curves]") {
    // Two straight curves of 4 segments each along x, at y = -1 and y = 1,
    // with a uniform primvar: hits report segments, the ids are per curve
    // (M9 D3), so every segment of a curve shows that curve's id.
    Scene scene;
    MeshDesc d;
    d.shape = MeshShape::Curves;
    d.curveBasis = CurveBasis::Linear;
    for (const float y : {-1.0f, 1.0f}) {
        for (int i = 0; i < 5; ++i)
            d.points.push_back({-1.9f + 0.95f * float(i), y, 0.0f});
        d.curveVertexCounts.push_back(5);
    }
    d.curveRadius.assign(d.points.size(), 0.35f);
    REQUIRE(prepareCurves(d));
    REQUIRE(d.segmentCurve.size() == 8);
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, d);
    scene.setMeshCrypto(id, cryptoHash("strands"),
                        {CryptoAttribute{"strand", true, {cryptoHash("7"), cryptoHash("9")}}});
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 16;
    settings.maxDepth = 1;
    settings.crypto = std::make_shared<const std::vector<CryptoRequest>>(
        std::vector<CryptoRequest>{{CryptoSource::Primvar, "strand"}});
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, 8, 8)); // rows 1-2 and 5-6 over the curves
    renderer.resize(8, 8);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    REQUIRE(film.crypto.size() == 1);
    const auto ids = [&](int x, int y) {
        std::map<uint32_t, float> m;
        const size_t px = size_t(y) * 8 + size_t(x);
        for (int s = 0; s < kCryptoSlots; ++s)
            if (film.crypto[0].weights[px * kCryptoSlots + s] > 0.0f)
                m[film.crypto[0].ids[px * kCryptoSlots + s]] += film.crypto[0].weights[px * kCryptoSlots + s];
        return m;
    };
    const std::map<uint32_t, float> a = ids(1, 1), b = ids(1, 6);
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);
    const uint32_t idA = a.begin()->first, idB = b.begin()->first;
    CHECK(idA != idB);
    CHECK((idA == cryptoHash("7") || idA == cryptoHash("9")));
    CHECK((idB == cryptoHash("7") || idB == cryptoHash("9")));
    // Along each curve (all its segments), its id alone.
    for (int x = 0; x < 8; ++x) {
        INFO("column " << x);
        for (const int y : {1, 2}) {
            const std::map<uint32_t, float> m = ids(x, y);
            CHECK((m.size() == 1 && m.count(idA) == 1));
        }
        for (const int y : {5, 6}) {
            const std::map<uint32_t, float> m = ids(x, y);
            CHECK((m.size() == 1 && m.count(idB) == 1));
        }
    }
}
