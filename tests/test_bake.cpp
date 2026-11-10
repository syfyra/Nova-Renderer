// Texture baking (docs/design/M11-P8.md): MikkTSpace frames, the image
// filters, the texture-space lookup and bake renders against analytic
// values (Karma's conventions are checked by tests/husk/check_karma_bake.py).

#include "core/bake/bake.h"
#include "core/bake/image_filter.h"
#include "core/bake/mikktspace.h"
#include "core/integrator/renderer.h"
#include "test_helpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

using namespace nova;
using Catch::Approx;

namespace {

MikkMesh quadMesh(bool mirrorU, bool reversed) {
    MikkMesh m;
    Vec3f p[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    Vec2f uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    int order[4] = {0, 1, 2, 3};
    if (reversed)
        order[1] = 3, order[3] = 1;
    MikkFace f;
    f.count = 4;
    for (int k = 0; k < 4; ++k) {
        const int i = order[k];
        m.P.push_back(p[i]);
        m.N.push_back({0, 0, reversed ? -1.0f : 1.0f});
        m.uv.push_back(mirrorU ? Vec2f{1.0f - uv[i].x, uv[i].y} : uv[i]);
        f.corner[k] = uint32_t(k);
    }
    m.faces.push_back(f);
    return m;
}

// The film value of a utility plane at a pixel (sum / count).
Vec3f planeValue(const Film &film, UtilityKind kind, int x, int y) {
    for (const UtilityPlane &p : film.utility)
        if (p.request.kind == kind) {
            const size_t i = size_t(y) * size_t(film.width) + size_t(x);
            const float inv = 1.0f / float(film.count[i]);
            const float *v = p.data.data() + i * p.width;
            return {v[0] * inv, p.width > 1 ? v[1] * inv : 0.0f, p.width > 2 ? v[2] * inv : 0.0f};
        }
    return {};
}

} // namespace

TEST_CASE("MikkTSpace tangents of quads and mirrored islands", "[bake]") {
    // Standard layout: T along +x, bitangent +y (sign +1).
    auto t = mikkTangents(quadMesh(false, false));
    for (const MikkTangent &c : t) {
        CHECK(c.t.x == Approx(1.0f).margin(1e-5));
        CHECK(c.sign == 1.0f);
    }
    // Mirrored u: T along -x, sign -1 (B = -cross(N, T) = +y, Karma measured).
    t = mikkTangents(quadMesh(true, false));
    for (const MikkTangent &c : t) {
        CHECK(c.t.x == Approx(-1.0f).margin(1e-5));
        CHECK(c.sign == -1.0f);
    }
    // Reversed winding (a left-handed face read as authored): the same
    // tangent, the opposite sign.
    MikkMesh flipped = quadMesh(false, false);
    std::swap(flipped.faces[0].corner[1], flipped.faces[0].corner[3]);
    t = mikkTangents(flipped);
    for (const MikkTangent &c : t) {
        CHECK(c.t.x == Approx(1.0f).margin(1e-5));
        CHECK(c.sign == -1.0f);
    }
}

TEST_CASE("MikkTSpace averages over welded vertices by corner angle", "[bake]") {
    // A cylinder of 8 quads, u around, v up: every tangent is the circle's
    // direction at its vertex, perpendicular to the normal.
    MikkMesh m;
    const int n = 8;
    for (int i = 0; i < n; ++i) {
        MikkFace f;
        f.count = 4;
        const int a[4][2] = {{i, 0}, {i + 1, 0}, {i + 1, 1}, {i, 1}};
        for (int k = 0; k < 4; ++k) {
            const float phi = kTwoPi * float(a[k][0]) / float(n);
            m.P.push_back({std::cos(phi), float(a[k][1]), -std::sin(phi)});
            m.N.push_back({std::cos(phi), 0.0f, -std::sin(phi)});
            m.uv.push_back({float(a[k][0]) / float(n), float(a[k][1])});
            f.corner[k] = uint32_t(m.P.size() - 1);
        }
        m.faces.push_back(f);
    }
    const auto t = mikkTangents(m);
    for (size_t c = 0; c < t.size(); ++c) {
        const Vec3f expected{-m.N[c].z, 0.0f, m.N[c].x}; // d/dphi of (cos, 0, -sin) is (-sin, 0, -cos)
        CHECK(std::fabs(dot(t[c].t, m.N[c])) < 1e-5f);
        CHECK(dot(t[c].t, Vec3f{-expected.x, 0.0f, -expected.z}) == Approx(1.0f).margin(1e-4));
    }
}

TEST_CASE("Karma's curvature bias is Schlick's", "[bake]") {
    CHECK(schlickBias(0.3f, 0.5f) == Approx(0.3f));
    CHECK(schlickBias(0.6269f, 0.75f) == Approx(0.8344f).margin(2e-4)); // measured pair (Karma)
    CHECK(schlickBias(0.3131f, 0.75f) == Approx(0.5776f).margin(2e-4));
    CHECK(schlickBias(0.2f, 1.0f) == Approx(1.0f));
    CHECK(schlickBias(0.9f, 0.0f) == 0.0f);
}

TEST_CASE("Image filters: shift and island expansion", "[bake]") {
    // shift on premultiplied texels: a * offset.
    float px[6] = {-0.2f, 0.4f, 1.0f, 0.0f, 0.0f, 0.0f};
    const float alpha[2] = {0.5f, 0.0f};
    ShiftFilter f;
    for (int c = 0; c < 3; ++c)
        f.scale[c] = 0.5f, f.offset[c] = 0.5f;
    f.premultiplied = true;
    applyShift(px, 2, 3, alpha, f);
    CHECK(px[0] == Approx(0.15f));
    CHECK(px[2] == Approx(0.75f));
    CHECK(px[3] == 0.0f);
    // swizzle xzy with scale (1, 1, -1)
    float q[3] = {1.0f, 2.0f, 3.0f};
    ShiftFilter s;
    s.swizzle[1] = 2, s.swizzle[2] = 1;
    s.scale[2] = -1.0f;
    applyShift(q, 1, 3, nullptr, s);
    CHECK(q[0] == 1.0f);
    CHECK(q[1] == 3.0f);
    CHECK(q[2] == -2.0f);

    // A 2x2 island in a 6x6 image: covered texels divided by their
    // coverage, the first ring the mean of its filled neighbours, all filled.
    const int w = 6, h = 6;
    std::vector<float> img(w * h, 0.0f), a(w * h, 0.0f);
    const int island[4][2] = {{2, 2}, {3, 2}, {2, 3}, {3, 3}};
    const float vals[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    for (int k = 0; k < 4; ++k) {
        const int i = island[k][1] * w + island[k][0];
        a[i] = k == 0 ? 0.5f : 1.0f;
        img[i] = vals[k] * a[i];
    }
    enlargeIslands(img.data(), w, h, 1, a.data(), 4);
    CHECK(img[2 * w + 2] == Approx(1.0f));
    CHECK(img[3 * w + 3] == Approx(4.0f));
    CHECK(img[1 * w + 1] == Approx(1.0f));            // ring 1, corner: only (2, 2)
    CHECK(img[1 * w + 2] == Approx((1.0f + 2.0f) / 2)); // above (2, 2) and (3, 2)
    for (float v : img)
        CHECK(v > 0.0f);
}

TEST_CASE("Bake: quads split along their second and fourth corners", "[bake]") {
    // A non-affine quad (square UVs on a skewed trapezoid) as the host's fan
    // triangulates it: Karma maps texture space through the 1-3 split
    // (measured: P.x 0.5638 at uv (0.3125, 0.3125), the 0-2 split gives 0.375).
    Scene scene;
    MeshDesc low;
    low.points = {{0, 0, 0}, {2, 0, 0}, {1.2f, 1, 0}, {-0.2f, 1, 0}};
    low.indices = {0, 1, 2, 0, 2, 3};
    low.triangleFaces = {0, 0};
    low.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    low.attributes.push_back({"st", AttributeRate::Vertex, 2, {0, 0, 1, 0, 1, 1, 0, 1}});
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, low);
    scene.commit();
    BakeSettings s;
    s.low = {id};
    Baker baker;
    baker.build(scene, s);
    BakePoint p;
    REQUIRE(baker.lookup({0.3125f, 0.3125f}, p));
    CHECK(p.P.x == Approx(2.0f * 0.3125f - 0.2f * 0.3125f).margin(1e-5));
    REQUIRE(baker.lookup({0.6875f, 0.6875f}, p));
    CHECK(p.P.x == Approx(1.0125f).margin(1e-4));
}

TEST_CASE("Bake: a triangle with a corner far outside the tile is still found", "[bake]") {
    // The lookup grid's cell range of a triangle was converted to int before
    // it was clamped: a corner at u = 1e9 (grid coordinate 4e9, beyond int)
    // became cell 0, the range was empty and the triangle was in no cell.
    Scene scene;
    MeshDesc low;
    std::vector<float> st;
    const auto triangle = [&](Vec3f p0, Vec3f p1, Vec3f p2, Vec2f t0, Vec2f t1, Vec2f t2) {
        const uint32_t base = uint32_t(low.points.size());
        low.points.insert(low.points.end(), {p0, p1, p2});
        low.indices.insert(low.indices.end(), {base, base + 1, base + 2});
        low.normals.insert(low.normals.end(), 3, Vec3f{0, 0, 1});
        st.insert(st.end(), {t0.x, t0.y, t1.x, t1.y, t2.x, t2.y});
    };
    triangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0.6f, 0.1f}, {1e9f, 0.1f}, {0.6f, 0.9f});
    for (int i = 0; i < 16; ++i) { // 17 triangles: a 4 x 4 grid
        const float u = 0.02f + 0.03f * float(i);
        triangle({u, 0, 5}, {u + 0.01f, 0, 5}, {u, 0.01f, 5}, {u, 0.01f}, {u + 0.01f, 0.01f}, {u, 0.02f});
    }
    low.attributes.push_back({"st", AttributeRate::Vertex, 2, st});
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, low);
    scene.commit();
    BakeSettings s;
    s.low = {id};
    Baker baker;
    baker.build(scene, s);
    BakePoint p;
    REQUIRE(baker.lookup({0.8f, 0.3f}, p)); // inside the far-reaching triangle, in grid cell (3, 1)
    CHECK(p.P.y == Approx(0.25f).margin(1e-3));
    CHECK(p.P.z == Approx(0.0f).margin(1e-6));
    REQUIRE(baker.lookup({0.025f, 0.012f}, p)); // a small triangle
    CHECK(p.P.z == Approx(5.0f));
}

TEST_CASE("Bake: texture-space lookup, UDIM tiles and the trace maps", "[bake]") {
    Scene scene;
    // Low: the unit square at z = 0 in UDIM 1002 (u in [1, 2]), hidden.
    MeshDesc low;
    low.points = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    low.indices = {0, 1, 2, 0, 2, 3};
    low.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    low.attributes.push_back({"st", AttributeRate::Vertex, 2, {1, 0, 2, 0, 2, 1, 1, 1}});
    const MeshId lowId = scene.createMesh();
    scene.updateMesh(lowId, low);
    scene.setMeshVisibility(lowId, ObjectVisibility{0, -1});
    // High: the plane z = 0.1 + 0.2 x + 0.1 y over [-1, 2]^2.
    MeshDesc high;
    for (Vec2f c : {Vec2f{-1, -1}, Vec2f{2, -1}, Vec2f{2, 2}, Vec2f{-1, 2}})
        high.points.push_back({c.x, c.y, 0.1f + 0.2f * c.x + 0.1f * c.y});
    high.indices = {0, 1, 2, 0, 2, 3};
    const MeshId highId = scene.createMesh();
    scene.updateMesh(highId, high);
    scene.commit();

    auto bake = std::make_shared<BakeSettings>();
    bake->low = {lowId};
    bake->udim = 1002;
    {
        Baker baker;
        baker.build(scene, *bake);
        BakePoint p;
        REQUIRE(baker.lookup({0.25f, 0.75f}, p));
        CHECK(p.P.x == Approx(0.25f));
        CHECK(p.P.y == Approx(0.75f));
        CHECK(p.T.x == Approx(1.0f));
        CHECK(p.B.y == Approx(1.0f));
        CHECK_FALSE(baker.lookup({1.5f, 0.5f}, p));
        BakeSettings other = *bake;
        other.udim = 1001; // nothing in the first tile
        baker.build(scene, other);
        CHECK_FALSE(baker.lookup({0.5f, 0.5f}, p));
    }

    RenderSettings s;
    s.maxSamples = 16;
    s.bake = bake;
    s.utility = std::make_shared<std::vector<UtilityRequest>>(std::vector<UtilityRequest>{
        {UtilityKind::BakeNormal, {}},
        {UtilityKind::BakeAlpha, {}},
        {UtilityKind::BakeHeight, {}},
        {UtilityKind::BakeVector, {}},
        {UtilityKind::BakePosition, {}},
        {UtilityKind::BakeOcclusion, {}},
        {UtilityKind::BakeCurvature, {}},
        {UtilityKind::BakeExport, "base_color"}});
    Renderer r;
    r.setScene(&scene);
    Camera camera;
    camera.fitFilm(8, 8);
    r.setCamera(camera);
    r.setSettings(s);
    r.resize(8, 8);
    r.reset();
    while (!r.converged())
        REQUIRE(r.renderPass({}));
    const Film &film = r.film();
    const Vec3f n = normalize(Vec3f{-0.2f, -0.1f, 1.0f});
    const Vec3f nt = planeValue(film, UtilityKind::BakeNormal, 4, 4);
    CHECK(nt.x == Approx(n.x).margin(1e-4));
    CHECK(nt.y == Approx(n.y).margin(1e-4));
    CHECK(nt.z == Approx(n.z).margin(1e-4));
    CHECK(planeValue(film, UtilityKind::BakeAlpha, 4, 4).x == Approx(1.0f));
    // Rays go along the low normal: Vd = (0, 0, z(x, y)), Ds = |Vd|; at the
    // texel centre on average (the filter is symmetric).
    const Vec3f vd = planeValue(film, UtilityKind::BakeVector, 4, 4);
    CHECK(vd.x == Approx(0.0f).margin(1e-5));
    CHECK(vd.z == Approx(0.1f + 0.3f * 0.5625f).margin(0.01));
    CHECK(planeValue(film, UtilityKind::BakeHeight, 4, 4).y == Approx(vd.z).margin(1e-4));
    const Vec3f P = planeValue(film, UtilityKind::BakePosition, 4, 4);
    CHECK(P.z == Approx(vd.z).margin(1e-4));
    CHECK(planeValue(film, UtilityKind::BakeOcclusion, 4, 4).x == Approx(1.0f));
    CHECK(planeValue(film, UtilityKind::BakeCurvature, 4, 4).x == Approx(0.5f));
    CHECK(planeValue(film, UtilityKind::BakeExport, 4, 4).x == Approx(0.8f)); // unbound: Standard Surface's
    // Coverage at the corner texel: the Gaussian filter's samples outside
    // the island add nothing (Karma: 0.71 at many samples).
    const float corner = film.alphaSum[0] / float(film.count[0]);
    CHECK(corner > 0.3f);
    CHECK(corner < 1.0f);
    CHECK(film.colorSum[27].x == 0.0f); // no lighting outputs
}
