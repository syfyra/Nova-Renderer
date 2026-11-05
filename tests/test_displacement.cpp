// M4.6 displacement in the core (docs/design/M4.md 3.6, docs/research/m4/
// displacement.md 7): constant and varying offsets on subdivision and
// polygon meshes, the dicing rate rule, normals of a height field (O(h^2)),
// motion parity with Karma (the first step's offset on every step), split
// copies at sharp creases, zero detection and watertightness.

#include "core/geometry/dicing.h"
#include "core/geometry/displace.h"
#include "core/geometry/subdiv.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <random>
#include <vector>

using namespace nova;
namespace sh = nova::shading;
using Catch::Matchers::WithinAbs;

namespace {

SubdivMeshDesc unitCube() {
    SubdivMeshDesc d;
    d.points = {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f},
                {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}};
    d.faceVertexCounts = {4, 4, 4, 4, 4, 4};
    d.faceVertexIndices = {0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4};
    return d;
}

// A unit quad in z = 0 with st = (x, y), as a bilinear control mesh.
SubdivMeshDesc unitQuad() {
    SubdivMeshDesc d;
    d.scheme = SubdivScheme::Bilinear;
    d.points = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    d.faceVertexCounts = {4};
    d.faceVertexIndices = {0, 1, 2, 3};
    SubdivPrimvar st;
    st.name = "st";
    st.interp = PrimvarInterp::Vertex;
    st.width = 2;
    st.values = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    d.primvars.push_back(st);
    return d;
}

// A latitude-longitude polygon sphere (quads, triangles at the poles) with
// its radial normals as a varying primvar "dir".
SubdivMeshDesc polygonSphere(int segments, int rings) {
    SubdivMeshDesc d;
    d.scheme = SubdivScheme::Bilinear;
    d.points.push_back({0.0f, 1.0f, 0.0f});
    for (int r = 1; r < rings; ++r) {
        const float theta = kPi * float(r) / float(rings);
        for (int s = 0; s < segments; ++s) {
            const float phi = kTwoPi * float(s) / float(segments);
            d.points.push_back({std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)});
        }
    }
    d.points.push_back({0.0f, -1.0f, 0.0f});
    const int south = int(d.points.size()) - 1;
    auto ring = [&](int r, int s) { return 1 + (r - 1) * segments + (s % segments); };
    for (int s = 0; s < segments; ++s) {
        d.faceVertexCounts.push_back(3);
        d.faceVertexIndices.insert(d.faceVertexIndices.end(), {0, ring(1, s + 1), ring(1, s)});
    }
    for (int r = 1; r + 1 < rings; ++r)
        for (int s = 0; s < segments; ++s) {
            d.faceVertexCounts.push_back(4);
            d.faceVertexIndices.insert(d.faceVertexIndices.end(),
                                       {ring(r, s), ring(r, s + 1), ring(r + 1, s + 1), ring(r + 1, s)});
        }
    for (int s = 0; s < segments; ++s) {
        d.faceVertexCounts.push_back(3);
        d.faceVertexIndices.insert(d.faceVertexIndices.end(), {south, ring(rings - 1, s), ring(rings - 1, s + 1)});
    }
    SubdivPrimvar dir;
    dir.name = "dir";
    dir.interp = PrimvarInterp::Varying;
    dir.width = 3;
    for (const Vec3f &p : d.points)
        dir.values.insert(dir.values.end(), {p.x, p.y, p.z});
    d.primvars.push_back(dir);
    return d;
}

// A displacement shader from `offset(builder)` (object-space offset).
struct Shader {
    sh::ProgramPtr surface;
    DisplacementShader displacement;
};
Shader makeShader(const std::function<sh::Ref(sh::IrBuilder &)> &offset) {
    sh::IrGraph g;
    sh::IrBuilder b(g);
    b.output(surface_field::kDisplacement, 3, offset(b));
    b.output(surface_field::baseColor, 3, b.constant(0.5f, 0.5f, 0.5f));
    Shader s;
    const sh::CompileResult r = sh::compile(g);
    REQUIRE(r.program);
    REQUIRE(r.program->displacement);
    s.surface = r.program;
    s.displacement.program = r.program->displacement;
    s.displacement.program->initUniforms(nullptr, s.displacement.uniforms);
    return s;
}

DisplacementInput inputFor(const MeshDesc &mesh, const DisplacementShader &shader) {
    DisplacementInput in;
    in.triangleShader.assign(mesh.triangleCount(), 0);
    in.shaders = {&shader};
    return in;
}

std::vector<Vec3f> directionsOf(const MeshDesc &mesh, const char *name) {
    const MeshAttribute *a = mesh.findAttribute(name);
    REQUIRE(a);
    std::vector<Vec3f> d(mesh.pointCount());
    for (size_t i = 0; i < d.size(); ++i)
        d[i] = {a->data[i * 3], a->data[i * 3 + 1], a->data[i * 3 + 2]};
    return d;
}

} // namespace

TEST_CASE("A constant displacement moves the limit surface along its normals", "[displacement]") {
    const Shader s = makeShader([](sh::IrBuilder &b) { return b.mul(b.global(sh::Global::Nobject), b.constant(0.1f)); });
    MeshDesc mesh;
    REQUIRE(tessellateSubdiv(unitCube(), 8, mesh));
    const MeshDesc before = mesh;
    const size_t moved = displaceMesh(mesh, inputFor(mesh, s.displacement), DisplacementContext{});
    CHECK(moved == mesh.pointCount());
    for (size_t i = 0; i < mesh.pointCount(); ++i) {
        const Vec3f d = mesh.points[i] - before.points[i];
        CHECK_THAT(length(d), WithinAbs(0.1, 1e-5));
        CHECK_THAT(dot(normalize(d), before.normals[i]), WithinAbs(1.0, 1e-5));
    }
    // A zero offset does not displace (Karma's default builder; the
    // translator drops unconnected zero amounts before they get here).
    const Shader zero = makeShader([](sh::IrBuilder &b) { return b.constant(0.0f, 0.0f, 0.0f); });
    CHECK(displacementIsZero(zero.displacement));
    CHECK_FALSE(displacementIsZero(s.displacement));
}

TEST_CASE("Polygon spheres displace along their interpolated normals", "[displacement]") {
    // 0.1 along the radial (vertex-normal) directions: control points reach
    // radius 1.1 exactly, every point moves by 0.1.
    const Shader s = makeShader([](sh::IrBuilder &b) { return b.mul(b.global(sh::Global::Nobject), b.constant(0.1f)); });
    MeshDesc mesh;
    REQUIRE(tessellateSubdiv(polygonSphere(32, 16), 4, mesh));
    const MeshDesc before = mesh;
    DisplacementInput in = inputFor(mesh, s.displacement);
    in.directions = directionsOf(mesh, "dir");
    displaceMesh(mesh, in, DisplacementContext{});
    int onSphere = 0;
    for (size_t i = 0; i < mesh.pointCount(); ++i) {
        CHECK_THAT(length(mesh.points[i] - before.points[i]), WithinAbs(0.1, 1e-5));
        if (std::fabs(length(before.points[i]) - 1.0f) < 1e-6f) {
            ++onSphere;
            CHECK_THAT(length(mesh.points[i]), WithinAbs(1.1, 1e-5));
        }
    }
    CHECK(onSphere == 32 * 15 + 2);
}

TEST_CASE("The dicing rate follows ceil(pixels x quality)", "[displacement]") {
    // Karma: a 200-pixel quad edge at quality 0.5 gives 100 segments; 12.5
    // pixels give 7.
    DicingView ortho;
    ortho.valid = true;
    ortho.perspective = false;
    ortho.pixelsPerUnit = 200.0f;
    CHECK(dicingRate(edgePixels(ortho, {0, 0, 0}, {1, 0, 0}), 0.5f) == 100);
    CHECK(dicingRate(12.5f, 0.5f) == 7);
    CHECK(dicingRate(0.0f, 0.5f) == 1);
    CHECK(dicingRate(1e9f, 0.5f) == 1024);
    // Perspective: the pixel length falls with the distance to the eye.
    DicingView persp;
    persp.valid = true;
    persp.pixelsPerUnit = 500.0f;
    persp.eye = {0, 0, 10};
    CHECK_THAT(edgePixels(persp, {-0.5f, 0, 0}, {0.5f, 0, 0}), WithinAbs(50.0, 1e-3));

    // Per-edge rates through the tessellation: 100 segments per edge.
    MeshDesc mesh;
    TessellateOptions opts;
    const SubdivMeshDesc quad = unitQuad();
    opts.edgeRate = [&](int32_t a, int32_t b) {
        return dicingRate(edgePixels(ortho, quad.points[size_t(a)], quad.points[size_t(b)]), 0.5f);
    };
    REQUIRE(tessellateSubdiv(quad, 1, mesh, opts));
    CHECK(mesh.pointCount() == 101 * 101);
    CHECK(mesh.triangleCount() == 2 * 100 * 100);
}

TEST_CASE("A displaced height field has O(h^2) normals", "[displacement]") {
    // z = 0.05 sin(8 pi s), from the st primvar.
    const Shader s = makeShader([](sh::IrBuilder &b) {
        const sh::Ref st = b.primvar("st", 2, b.constant(0.0f));
        const sh::Ref z = b.mul(b.unary(sh::Op::Sin, b.mul(b.component(st, 0), b.constant(8.0f * kPi))), b.constant(0.05f));
        return b.mul(b.global(sh::Global::Nobject), z);
    });
    double previous = 0.0;
    for (const int rate : {50, 100, 200}) {
        MeshDesc mesh;
        TessellateOptions opts;
        opts.edgeRate = [rate](int32_t, int32_t) { return rate; };
        REQUIRE(tessellateSubdiv(unitQuad(), 1, mesh, opts));
        displaceMesh(mesh, inputFor(mesh, s.displacement), DisplacementContext{});
        double zErr = 0.0, nErr = 0.0;
        int count = 0;
        for (size_t i = 0; i < mesh.pointCount(); ++i) {
            const Vec3f p = mesh.points[i];
            zErr = std::max(zErr, std::fabs(double(p.z) - 0.05 * std::sin(8.0 * 3.14159265358979 * p.x)));
            if (p.x < 0.02f || p.x > 0.98f || p.y < 0.02f || p.y > 0.98f)
                continue; // borders: one-sided
            const double dz = 0.05 * 8.0 * 3.14159265358979 * std::cos(8.0 * 3.14159265358979 * p.x);
            const Vec3f n = normalize(Vec3f{float(-dz), 0.0f, 1.0f});
            nErr += std::acos(std::min(1.0, double(dot(n, mesh.normals[i]))));
            ++count;
        }
        nErr /= count;
        CAPTURE(rate, nErr);
        CHECK(zErr < 1e-6);
        if (previous > 0.0)
            CHECK(previous / nErr > 3.0); // about 4 per halving
        previous = nErr;
    }
}

TEST_CASE("Motion steps take the first step's offset", "[displacement]") {
    // Karma: a sphere translated +2 over the shutter with displacement
    // 0.1 (P.x + 1) along the normal reaches x = 3.2 at shutter close.
    const Shader s = makeShader([](sh::IrBuilder &b) {
        const sh::Ref px = b.component(b.global(sh::Global::Pobject), 0);
        return b.mul(b.global(sh::Global::Nobject), b.mul(b.add(px, b.constant(1.0f)), b.constant(0.1f)));
    });
    SubdivMeshDesc sphere = polygonSphere(32, 16);
    const size_t n = sphere.points.size();
    sphere.pointSteps = 2;
    for (size_t i = 0; i < n; ++i)
        sphere.points.push_back(sphere.points[i] + Vec3f{2.0f, 0.0f, 0.0f});
    MeshDesc mesh;
    REQUIRE(tessellateSubdiv(sphere, 1, mesh));
    DisplacementInput in = inputFor(mesh, s.displacement);
    in.directions = directionsOf(mesh, "dir");
    displaceMesh(mesh, in, DisplacementContext{});
    float maxX0 = -1e9f, maxX1 = -1e9f;
    const size_t np = mesh.pointCount();
    for (size_t i = 0; i < np; ++i) {
        maxX0 = std::max(maxX0, mesh.points[i].x);
        maxX1 = std::max(maxX1, mesh.points[np + i].x);
    }
    CHECK_THAT(maxX0, WithinAbs(1.2, 1e-5));
    CHECK_THAT(maxX1, WithinAbs(3.2, 1e-5));
    // Normals per step are unit length.
    for (const Vec3f &nn : mesh.normals)
        CHECK_THAT(length(nn), WithinAbs(1.0, 1e-5));
}

TEST_CASE("Split copies at sharp creases keep their source's offset", "[displacement]") {
    const Shader s = makeShader([](sh::IrBuilder &b) {
        const sh::Ref y = b.component(b.global(sh::Global::Pobject), 1);
        return b.mul(b.global(sh::Global::Nobject), b.add(b.constant(0.05f), b.mul(y, b.constant(0.1f))));
    });
    SubdivMeshDesc cube = unitCube();
    cube.creaseEdges = {0, 1};
    cube.creaseSharpness = {10.0f};
    MeshDesc mesh;
    TessellateOptions opts;
    std::vector<std::pair<int32_t, int32_t>> copies;
    opts.copies = &copies;
    REQUIRE(tessellateSubdiv(cube, 8, mesh, opts));
    REQUIRE(!copies.empty());
    DisplacementInput in = inputFor(mesh, s.displacement);
    in.copies = copies;
    displaceMesh(mesh, in, DisplacementContext{});
    for (const auto &[copy, source] : copies)
        CHECK(maxComponent(absolute(mesh.points[size_t(copy)] - mesh.points[size_t(source)])) == 0.0f);
}

TEST_CASE("Displaced closed meshes stay watertight", "[displacement]") {
    // A varying offset on a Catmull-Clark cube and on a bilinear polygon
    // cube; rays from the centre all hit.
    const Shader s = makeShader([](sh::IrBuilder &b) {
        const sh::Ref p = b.global(sh::Global::Pobject);
        const sh::Ref d = b.add(b.constant(0.08f), b.mul(b.unary(sh::Op::Sin, b.mul(b.component(p, 1), b.constant(20.0f))),
                                                         b.constant(0.04f)));
        return b.mul(b.global(sh::Global::Nobject), d);
    });
    for (const bool polygon : {false, true}) {
        CAPTURE(polygon);
        SubdivMeshDesc cage = unitCube();
        if (polygon) {
            cage.scheme = SubdivScheme::Bilinear;
            SubdivPrimvar dir;
            dir.name = "dir";
            dir.interp = PrimvarInterp::Varying;
            dir.width = 3;
            for (const Vec3f &p : cage.points) {
                const Vec3f n = normalize(p);
                dir.values.insert(dir.values.end(), {n.x, n.y, n.z});
            }
            cage.primvars.push_back(dir);
        }
        MeshDesc mesh;
        TessellateOptions opts;
        opts.edgeRate = [](int32_t a, int32_t b) { return 7 + (a + b) % 5; }; // uneven rates
        REQUIRE(tessellateSubdiv(cage, 1, mesh, opts));
        DisplacementInput in = inputFor(mesh, s.displacement);
        if (polygon)
            in.directions = directionsOf(mesh, "dir");
        displaceMesh(mesh, in, DisplacementContext{});
        Scene scene;
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, std::move(mesh));
        scene.commit();
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        int misses = 0;
        const int rays = 10000;
        std::vector<Vec3f> org(rays, Vec3f{0.0f, 0.0f, 0.0f}), dir(rays);
        std::vector<float> tMax(rays, 1e30f), t(rays);
        std::vector<uint32_t> meshIds(rays), prims(rays);
        std::vector<Vec2f> uv(rays);
        for (int i = 0; i < rays; ++i) {
            Vec3f d;
            do {
                d = {u(rng), u(rng), u(rng)};
            } while (dot(d, d) > 1.0f || dot(d, d) < 1e-4f);
            dir[size_t(i)] = normalize(d);
        }
        RayBatch batch{org.data(), dir.data(), tMax.data(), uint32_t(rays)};
        HitBatch hits{t.data(), meshIds.data(), prims.data(), uv.data()};
        scene.accel().intersect(batch, hits);
        for (int i = 0; i < rays; ++i)
            misses += meshIds[size_t(i)] == kInvalidId ? 1 : 0;
        CHECK(misses == 0);
    }
}

namespace {

// Returns the MIP level OIIO would read for a 1024-texel texture (log2 of
// the footprint in texels), so the displacement reports it.
struct LevelProbe final : ITextureSampler {
    void sample2D(const TexSampler &, const TexQuery &q, float *out, uint8_t *status) const override {
        for (uint32_t l = 0; l < q.count; ++l) {
            const float dx = q.dsdx ? std::hypot(q.dsdx[l], q.dtdx[l]) : 0.0f;
            const float dy = q.dsdy ? std::hypot(q.dsdy[l], q.dtdy[l]) : 0.0f;
            out[l] = std::log2(std::max(1024.0f * std::max(dx, dy), 1e-6f));
            status[l] = 0;
        }
    }
};

} // namespace

TEST_CASE("Displacement textures read at half a micropolygon edge", "[displacement]") {
    // A 1024-texel map over a quad diced into 16 segments: 64 texels per
    // edge, level log2(64) - 1 = 5 (Karma measured -1.08).
    sh::IrGraph g;
    sh::IrBuilder b(g);
    TexSlot slot;
    slot.channels = 1;
    const uint32_t tex = b.textureSlot(slot);
    const sh::Ref level = b.tex2D(tex, b.primvar("st", 2, b.constant(0.0f)), b.constant(0.0f));
    b.output(surface_field::kDisplacement, 3, b.mul(b.global(sh::Global::Nobject), level));
    const sh::CompileResult r = sh::compile(g);
    REQUIRE(r.program);
    REQUIRE(r.program->displacement);
    DisplacementShader shader;
    shader.program = r.program->displacement;
    shader.program->initUniforms(nullptr, shader.uniforms);
    shader.samplers = {TexSampler{0, slot}};
    LevelProbe probe;
    DisplacementContext ctx;
    ctx.textures = &probe;
    MeshDesc mesh;
    TessellateOptions opts;
    opts.edgeRate = [](int32_t, int32_t) { return 16; };
    REQUIRE(tessellateSubdiv(unitQuad(), 1, mesh, opts));
    displaceMesh(mesh, inputFor(mesh, shader), ctx);
    for (size_t i = 0; i < mesh.pointCount(); ++i)
        CHECK_THAT(mesh.points[i].z, WithinAbs(5.0, 0.05));
}

namespace {

// A VEX displacement as the bridge runs it (docs/design/M5.md D8):
// P += N * amp, in world space.
class FakeDisplacer final : public ExternalDisplacer {
public:
    FakeDisplacer() {
        ExternalParam amp;
        amp.name = "amp";
        amp.width = 1;
        amp.defaults = {0.0f};
        _params = {amp};
    }
    const std::vector<ExternalParam> &params() const override { return _params; }
    bool run(int count, const ExternalLane *lanes, const ExternalInput *inputs, const std::string *, Vec3f *P,
             std::string *) const override {
        for (int l = 0; l < count; ++l) {
            const float amp = inputs[0].data[inputs[0].varying ? l : 0];
            P[l] = lanes[l].P + lanes[l].N * amp;
        }
        return true;
    }

private:
    std::vector<ExternalParam> _params;
};

} // namespace

TEST_CASE("VEX displacement runs in world space and reads primvars of its parameters' names", "[displacement]") {
    DisplacementShader shader;
    shader.external = std::make_shared<FakeDisplacer>();
    shader.externalValues = {{0.2f}};
    shader.externalStrings = {""};
    CHECK_FALSE(displacementIsZero(shader));
    // Scaled by 2 and moved: 0.2 in world space is 0.1 in object space.
    DisplacementContext ctx;
    ctx.objectToWorld = Mat4::identity();
    for (int k = 0; k < 3; ++k)
        ctx.objectToWorld.m[k][k] = 2.0;
    ctx.objectToWorld.m[3][0] = 5.0;
    MeshDesc mesh;
    REQUIRE(tessellateSubdiv(unitCube(), 4, mesh));
    const MeshDesc before = mesh;
    CHECK(displaceMesh(mesh, inputFor(mesh, shader), ctx) == mesh.pointCount());
    for (size_t i = 0; i < mesh.pointCount(); ++i) {
        const Vec3f d = mesh.points[i] - before.points[i];
        CHECK_THAT(length(d), WithinAbs(0.1, 1e-5));
        CHECK_THAT(dot(normalize(d), before.normals[i]), WithinAbs(1.0, 1e-5));
    }
    // A vertex primvar "amp" overrides the value per point.
    mesh = before;
    MeshAttribute amp;
    amp.name = "amp";
    amp.rate = AttributeRate::Vertex;
    for (size_t i = 0; i < mesh.pointCount(); ++i)
        amp.data.push_back(i % 2 ? 0.4f : 0.0f);
    mesh.attributes.push_back(amp);
    displaceMesh(mesh, inputFor(mesh, shader), ctx);
    for (size_t i = 0; i < mesh.pointCount(); ++i)
        CHECK_THAT(length(mesh.points[i] - before.points[i]), WithinAbs(i % 2 ? 0.2 : 0.0, 1e-5));
}
