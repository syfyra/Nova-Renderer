// M8.1 volume shaders (docs/design/M8.md D4-D7): the compiler splits the
// volume outputs into Program::volume; interval runs of its bytecode bound
// every value the shader takes over given input ranges; a volume reads its
// fields through the shader's primvar reads, and its majorant cells bound
// the shader's extinction (with a rate floor where it emits), or take a
// sampled estimate where intervals give no bound.

#if defined(_MSC_VER)
#pragma warning(disable : 4702) // unreachable code in NanoVDB's grid builder templates
#endif

#include "core/integrator/volume_eval.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"
#include "core/shading/interval.h"
#include "core/shading/vm.h"

#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/GridBuilder.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace nova;
using namespace nova::shading;

namespace {

template <typename F>
VolumeGridPtr makeGrid(F f, int n, double voxel, nanovdb::Vec3d origin = nanovdb::Vec3d(0.0)) {
    nanovdb::build::Grid<float> g(0.0f, "field", nanovdb::GridClass::FogVolume);
    g.setTransform(voxel, origin);
    g([&](const nanovdb::Coord &ijk) { return f(ijk[0], ijk[1], ijk[2]); },
      nanovdb::CoordBBox(nanovdb::Coord(0), nanovdb::Coord(n - 1)));
    const nanovdb::GridHandle<nanovdb::HostBuffer> handle = nanovdb::createNanoGrid(g);
    return VolumeGrid::copyOf(handle.data(), handle.size());
}

// A ball of density with a soft edge, and a temperature that rises with y.
float ball(int x, int y, int z) {
    const float dx = float(x) - 20.0f, dy = float(y) - 20.0f, dz = float(z) - 20.0f;
    const float r = std::sqrt(dx * dx + dy * dy + dz * dz);
    return r < 14.0f ? std::min(1.0f, (14.0f - r) / 4.0f) * (1.0f + 0.2f * float((x * 7 + y * 3 + z) % 5)) : 0.0f;
}
float warm(int x, int y, int z) {
    const float v = ball(x, y, z) > 0.0f ? 0.05f * float(y) : 0.0f;
    return v;
}

Ref vout(IrBuilder &b, uint16_t field, int width, Ref value) {
    b.output(uint16_t(volume_field::kBase + field), width, value);
    return value;
}

} // namespace

TEST_CASE("The compiler splits a material's volume outputs into Program::volume", "[volume][shader]") {
    IrGraph g;
    IrBuilder b(g);
    b.output(surface_field::baseColor, 3, b.constant(0.5f, 0.5f, 0.5f));
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    vout(b, volume_field::scattering, 3, b.convert(b.mul(d, b.constant(2.0f)), 3));
    vout(b, volume_field::anisotropy, 1, b.constant(0.4f));
    const CompileResult r = compile(g);
    REQUIRE(r.program);
    REQUIRE(r.program->volume);
    const Program &v = *r.program->volume;
    CHECK(v.primvars == std::vector<std::string>{"density"});
    bool scattering = false, anisotropy = false;
    for (const OutputBinding &o : v.outputs) {
        scattering |= o.field == volume_field::scattering && !o.uniform;
        anisotropy |= o.field == volume_field::anisotropy && o.uniform;
    }
    CHECK(scattering);
    CHECK(anisotropy);
    // The surface program never reads the volume's primvars.
    CHECK(r.program->primvarBindings.empty());
    for (const OutputBinding &o : r.program->outputs)
        CHECK(o.field < surface_field::kBlockSize);
}

TEST_CASE("Interval runs bound every value of a program", "[volume][shader][interval]") {
    IrGraph g;
    IrBuilder b(g);
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    const Ref t = b.primvar("temperature", 1, b.constant(0.0f));
    const Ref ray = b.global(Global::RayType);
    const Ref x = b.ternary(Op::Clamp, b.sub(b.mul(d, b.constant(2.0f)), b.constant(0.3f)), b.constant(0.0f),
                            b.constant(1.5f));
    const Ref y = b.ternary(Op::Mix, x, b.binary(Op::Pow, b.unary(Op::Abs, t), b.constant(1.7f)),
                            b.ternary(Op::Smoothstep, b.constant(0.2f), b.constant(0.8f), d));
    const Ref shadow = b.binary(Op::Greater, ray, b.constant(5.5f));
    const Ref z = b.add(b.ternary(Op::Select, shadow, b.mul(y, b.constant(0.5f)), y),
                        b.mul(b.unary(Op::Abs, b.unary(Op::Sin, t)), b.binary(Op::Max, d, b.constant(0.1f))));
    const Ref w = b.binary(Op::Div, b.unary(Op::Exp, b.unary(Op::Neg, d)), b.add(b.unary(Op::Abs, t), b.constant(1.0f)));
    vout(b, volume_field::scattering, 3, b.concat(b.concat(z, w), b.binary(Op::Min, z, w)));
    vout(b, volume_field::absorption, 1, b.unary(Op::Sqrt, b.unary(Op::Log, b.add(d, b.constant(1.0f)))));
    const CompileResult r = compile(g);
    REQUIRE(r.program);
    REQUIRE(r.program->volume);
    const Program &p = *r.program->volume;
    std::vector<float> U;
    p.initUniforms(nullptr, U);

    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    int outside = 0, checked = 0;
    for (int trial = 0; trial < 200; ++trial) {
        // Random input ranges, some of them single points.
        float d0 = 2.0f * u(rng) - 0.5f, d1 = d0 + (trial % 5 == 0 ? 0.0f : 2.0f * u(rng));
        float t0 = 4.0f * u(rng) - 2.0f, t1 = t0 + (trial % 7 == 0 ? 0.0f : 3.0f * u(rng));
        const float rt0 = trial % 3 == 0 ? 6.0f : 0.0f, rt1 = trial % 3 == 1 ? 0.0f : 6.0f;
        std::vector<Interval> R(p.rowCount);
        for (const GlobalBinding &gb : p.globals)
            R[gb.row] = {std::min(rt0, rt1), std::max(rt0, rt1)};
        for (const PrimvarBinding &pb : p.primvarBindings)
            R[pb.row] = p.primvars[pb.name] == "density" ? Interval{d0, d1} : Interval{t0, t1};
        executeInterval(p.code.data(), p.code.size(), R.data(), U.data());
        std::vector<Interval> bound(volume_field::kCount, Interval::point(0.0f));
        for (const OutputBinding &o : p.outputs)
            for (int k = 0; k < o.width; ++k)
                bound[o.field + k] = o.uniform ? Interval::point(U[o.index + k]) : R[o.index + k];
        for (int s = 0; s < 64; ++s) {
            std::vector<float> rows(p.rowCount);
            const float dv = d0 + (d1 - d0) * u(rng), tv = t0 + (t1 - t0) * u(rng);
            const float rv = s % 2 ? std::min(rt0, rt1) : std::max(rt0, rt1);
            for (const GlobalBinding &gb : p.globals)
                rows[gb.row] = rv;
            for (const PrimvarBinding &pb : p.primvarBindings)
                rows[pb.row] = p.primvars[pb.name] == "density" ? dv : tv;
            execute<1>(p.code.data(), p.code.size(), rows.data(), U.data());
            for (const OutputBinding &o : p.outputs) {
                if (o.uniform)
                    continue;
                for (int k = 0; k < o.width; ++k) {
                    const float v = rows[o.index + k];
                    const Interval &iv = bound[o.field + k];
                    const float tol = 1e-5f * std::max(1.0f, std::fabs(v));
                    outside += !(v >= iv.lo - tol && v <= iv.hi + tol);
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 10000);
    CHECK(outside == 0);

    // No rule: unbounded; an unbounded operand stays unbounded, never NaN.
    IrGraph n;
    IrBuilder nb(n);
    const Ref P = nb.global(Global::P);
    vout(nb, volume_field::scattering, 1, nb.mul(nb.noise(P, 1), nb.constant(0.0f)));
    vout(nb, volume_field::absorption, 1, nb.add(nb.noise(P, 1), nb.constant(1.0f)));
    const CompileResult nr = compile(n);
    REQUIRE(nr.program);
    REQUIRE(nr.program->volume);
    const Program &np = *nr.program->volume;
    std::vector<float> nu;
    np.initUniforms(nullptr, nu);
    std::vector<Interval> R(np.rowCount);
    for (const GlobalBinding &gb : np.globals)
        for (int k = 0; k < 3; ++k)
            R[gb.row + k] = {0.0f, 1.0f};
    executeInterval(np.code.data(), np.code.size(), R.data(), nu.data());
    for (const OutputBinding &o : np.outputs) {
        if (o.uniform || o.field != volume_field::absorption)
            continue;
        CHECK_FALSE(R[o.index].bounded());
        CHECK_FALSE(std::isnan(R[o.index].lo));
    }
}

TEST_CASE("Volumes read their fields through the shader and bound its extinction", "[volume][shader]") {
    Scene scene;
    const VolumeGridPtr density = makeGrid(ball, 40, 0.05);
    const VolumeGridPtr temperature = makeGrid(warm, 20, 0.1, nanovdb::Vec3d(0.025, 0.025, 0.025)); // coarser, shifted
    IrGraph g;
    IrBuilder b(g);
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    const Ref t = b.primvar("temperature", 1, b.constant(0.0f));
    vout(b, volume_field::scattering, 3, b.mul(b.convert(d, 3), b.constant(1.6f, 1.8f, 2.0f)));
    vout(b, volume_field::absorption, 3, b.convert(b.mul(d, b.constant(0.5f)), 3));
    vout(b, volume_field::emission, 3, b.mul(b.convert(t, 3), b.constant(1.0f, 0.5f, 0.1f)));
    vout(b, volume_field::anisotropy, 1, b.constant(0.3f));
    const CompileResult compiled = compile(g);
    REQUIRE(compiled.program);
    MaterialDesc material;
    material.program = compiled.program;
    const MaterialId mid = scene.createMaterial();
    scene.updateMaterial(mid, material);

    VolumeDesc desc;
    desc.fields.push_back({"density", "density", density, Affine3f{}});
    desc.fields.push_back({"temperature", "temperature", temperature, Affine3f{}});
    desc.material = mid;
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    scene.commit();
    REQUIRE(scene.activeVolumes().size() == 1);
    const VolumeRecord &rec = scene.volumeRecord(id);
    REQUIRE(rec.program);
    CHECK(rec.reference == 0);
    CHECK(rec.estimatedCells == 0);

    VolumeEvaluator eval(scene, id);
    VolumeSampler sd(*density->grid()), st(*temperature->grid());
    const Affine3f dIndex = inverseAffine(density->indexToObject()), tIndex = inverseAffine(temperature->indexToObject());
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-0.1f, 2.1f);
    int violations = 0, emissive = 0;
    for (int i = 0; i < 100000; ++i) {
        const Vec3f p{u(rng), u(rng), u(rng)};
        const VolumePoint v = eval(p, p, {0.0f, 0.0f, 1.0f}, 0.5f, 0, 0);
        const float dv = std::max(sd(dIndex.point(p)), 0.0f), tv = st(tIndex.point(p));
        if (i < 2000) {
            CHECK_THAT(v.scattering.y, WithinAbs(1.8 * dv, 1e-5));
            CHECK_THAT(v.absorption.x, WithinAbs(0.5 * dv, 1e-5));
            CHECK_THAT(v.emission.y, WithinAbs(0.5 * tv, 1e-5));
            CHECK_THAT(v.g, WithinAbs(0.3, 1e-7));
        }
        // The majorant: every channel's extinction below its cell's bound.
        const Vec3f q = rec.fieldObjectToIndex[size_t(rec.reference)].point(p);
        int32_t c[3];
        const float sigma = maxComponent(v.extinction());
        if (!rec.layout.cellOf(q, c)) {
            violations += sigma > 0.0f || maxComponent(absolute(v.emission)) > 0.0f;
            continue;
        }
        const float m = rec.majorant[rec.layout.index(c[0], c[1], c[2])];
        violations += sigma > m * (1.0f + 1e-5f) + 1e-6f;
        if (maxComponent(absolute(v.emission)) > 0.0f) {
            ++emissive;
            violations += m < 1.0f / 0.05f - 1e-3f; // the rate floor: one collision per voxel
        }
    }
    CHECK(violations == 0);
    CHECK(emissive > 1000);

    SECTION("a material edit rebuilds the majorants") {
        // With emission the floor of one collision per voxel (20) dominates;
        // denser and without emission, the density's maximum (1.8) times 8.5.
        CHECK_THAT(*std::max_element(rec.majorant.begin(), rec.majorant.end()), WithinAbs(20.0, 1e-3));
        IrGraph g2;
        IrBuilder b2(g2);
        const Ref d2 = b2.primvar("density", 1, b2.constant(0.0f));
        vout(b2, volume_field::scattering, 3, b2.convert(b2.mul(d2, b2.constant(8.0f)), 3));
        vout(b2, volume_field::absorption, 3, b2.convert(b2.mul(d2, b2.constant(0.5f)), 3));
        material.program = compile(g2).program;
        scene.updateMaterial(mid, material);
        scene.commit();
        const float after = *std::max_element(rec.majorant.begin(), rec.majorant.end());
        CHECK(after > 8.5f * 1.5f);
        CHECK(after < 8.5f * 1.8f + 1e-3f);
    }
    SECTION("without the material the volume is the default one") {
        desc.material = kDefaultMaterialId;
        scene.updateVolume(id, desc);
        scene.commit();
        CHECK_FALSE(scene.volumeRecord(id).program);
        VolumeEvaluator plain(scene, id);
        const Vec3f p{1.0f, 1.0f, 1.0f};
        const VolumePoint v = plain(p, p, {0.0f, 0.0f, 1.0f}, 0.5f, 0, 0);
        const float dv = sd(dIndex.point(p));
        CHECK_THAT(v.scattering.x, WithinRel(dv, 1e-6f));
        CHECK(v.absorption.x == 0.0f);
        CHECK(v.emission.x == 0.0f);
    }
}

TEST_CASE("Cells without an interval bound take a sampled estimate", "[volume][shader]") {
    Scene scene;
    const VolumeGridPtr density = makeGrid(ball, 40, 0.05);
    IrGraph g;
    IrBuilder b(g);
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    // Noise has no interval rule.
    const Ref n = b.add(b.mul(b.noise(b.global(Global::P), 1), b.constant(0.25f)), b.constant(1.0f));
    vout(b, volume_field::scattering, 3, b.convert(b.mul(d, n), 3));
    MaterialDesc material;
    material.program = compile(g).program;
    REQUIRE(material.program);
    const MaterialId mid = scene.createMaterial();
    scene.updateMaterial(mid, material);
    VolumeDesc desc;
    desc.fields.push_back({"density", "density", density, Affine3f{}});
    desc.material = mid;
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    scene.commit();
    const VolumeRecord &rec = scene.volumeRecord(id);
    CHECK(rec.estimatedCells > 0);
    for (float m : rec.majorant)
        CHECK(std::isfinite(m));
    CHECK(*std::max_element(rec.majorant.begin(), rec.majorant.end()) > 1.0f);
}

TEST_CASE("Shaders that use a field twice are bounded by their transfer table", "[volume][shader]") {
    // Houdini's complement colour divides XYZ by X+Y+Z, both from density:
    // intervals see an unbounded quotient, the tabulated shader does not.
    Scene scene;
    const VolumeGridPtr density = makeGrid(ball, 40, 0.05);
    IrGraph g;
    IrBuilder b(g);
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    const Ref sum = b.add(b.mul(d, b.constant(3.0f)), b.mul(d, b.constant(1.0f)));
    const Ref share = b.ternary(Op::Select, b.binary(Op::Equal, sum, b.constant(0.0f)), b.constant(0.0f),
                                b.div(b.mul(d, b.constant(3.0f)), sum)); // 0.75 wherever d > 0
    vout(b, volume_field::scattering, 3, b.convert(b.mul(share, b.mul(d, b.constant(4.0f))), 3));
    MaterialDesc material;
    material.program = compile(g).program;
    REQUIRE(material.program);
    const MaterialId mid = scene.createMaterial();
    scene.updateMaterial(mid, material);
    VolumeDesc desc;
    desc.fields.push_back({"density", "density", density, Affine3f{}});
    desc.material = mid;
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    scene.commit();
    const VolumeRecord &rec = scene.volumeRecord(id);
    CHECK(rec.estimatedCells == 0);
    VolumeEvaluator eval(scene, id);
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> u(0.0f, 2.0f);
    int violations = 0;
    float largest = 0.0f;
    for (int i = 0; i < 50000; ++i) {
        const Vec3f p{u(rng), u(rng), u(rng)};
        const float sigma = maxComponent(eval(p, p, {0.0f, 0.0f, 1.0f}, 0.5f, 0, 0).extinction());
        int32_t c[3];
        if (!rec.layout.cellOf(rec.fieldObjectToIndex[0].point(p), c))
            continue;
        const float m = rec.majorant[rec.layout.index(c[0], c[1], c[2])];
        violations += sigma > m;
        largest = std::max(largest, sigma);
    }
    CHECK(violations == 0);
    // Tight: about 3 times the density's maximum (1.8), 5 % more.
    CHECK(*std::max_element(rec.majorant.begin(), rec.majorant.end()) < 1.06f * 3.0f * 1.8f + 1e-3f);
    CHECK(largest > 2.0f);
}

TEST_CASE("Time-driven volume shaders are bounded at the render's frame", "[volume][shader]") {
    // Scattering = density * Time: the intervals take the render's time
    // (Scene::setShadeFrame), so the majorants cover what the evaluator
    // sees at that frame (a table at time 0 bounded it by 0: an invisible
    // volume); another frame rebuilds them.
    Scene scene;
    const VolumeGridPtr density = makeGrid(ball, 40, 0.05);
    IrGraph g;
    IrBuilder b(g);
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    vout(b, volume_field::scattering, 3, b.convert(b.mul(d, b.global(Global::Time)), 3));
    MaterialDesc material;
    material.program = compile(g).program;
    REQUIRE(material.program);
    const MaterialId mid = scene.createMaterial();
    scene.updateMaterial(mid, material);
    VolumeDesc desc;
    desc.fields.push_back({"density", "density", density, Affine3f{}});
    desc.material = mid;
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    for (const float frame : {49.0f, 97.0f}) {
        INFO("frame " << frame);
        scene.setShadeFrame(frame, 24.0f);
        scene.commit();
        const VolumeRecord &rec = scene.volumeRecord(id);
        REQUIRE(rec.frameDependent);
        CHECK(rec.estimatedCells == 0);
        const float time = (frame - 1.0f) / 24.0f;
        VolumeEvaluator eval(scene, id, ShadeGlobals{frame, 24.0f});
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> u(0.0f, 2.0f);
        int violations = 0;
        float largest = 0.0f;
        for (int i = 0; i < 20000; ++i) {
            const Vec3f p{u(rng), u(rng), u(rng)};
            const float sigma = maxComponent(eval(p, p, {0.0f, 0.0f, 1.0f}, 0.5f, 0, 0).extinction());
            int32_t c[3];
            if (!rec.layout.cellOf(rec.fieldObjectToIndex[0].point(p), c))
                continue;
            violations += sigma > rec.majorant[rec.layout.index(c[0], c[1], c[2])];
            largest = std::max(largest, sigma);
        }
        CHECK(violations == 0);
        CHECK(largest > time);
        // Tight: the time times the density's maximum (1.8).
        CHECK(*std::max_element(rec.majorant.begin(), rec.majorant.end()) < 1.01f * time * 1.8f);
    }
}
