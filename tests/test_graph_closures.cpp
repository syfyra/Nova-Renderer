// M4.5 generic BSDF graphs (docs/design/M4.md 3.4, docs/research/m4/
// bsdf-graphs.md 7.1): closed-form furnaces of layer, mix, add, multiply and
// shared subgraphs, the transmission-only leaf completing the native R+T
// lobe, inside views, the Burley and Hoffman leaves, emission, segments of
// native models, lobe overflow, and the sampling of random trees (chi^2).

#include "bsdf_stats.h"

#include "core/bsdf/surface.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

using namespace nova;
using nova::test::Chi2Result;
using nova::test::LobeUnderTest;
using nova::test::Pcg32;
using nova::test::V3;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace sf = nova::surface_field;

namespace {

Vec3f toF(V3 v) { return {float(v.x), float(v.y), float(v.z)}; }
V3 toD(Vec3f v) { return {v.x, v.y, v.z}; }
Vec3f direction(float mu, float phi = 0.3f) {
    const float s = std::sqrt(std::fmax(0.0f, 1.0f - mu * mu));
    return {s * std::cos(phi), s * std::sin(phi), mu};
}
ClosureContext context() { return {&bsdfTables(), kXyzToRec709Grey}; }
const ShadingGeometry kFlat{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, true};

// A graph and its block, assembled by hand (the translator's job).
struct Graph {
    ClosureGraph g;
    SurfaceInputs in{};
    uint16_t next = sf::kCount;

    Graph() {
        in.model = SurfaceModel::Graph;
        g.frameCount = 1;
        g.frame[0] = {kNoSlot, kNoSlot};
    }
    Graph(const Graph &o) : g(o.g), in(o.in), next(o.next) { in.graph = &g; }
    uint16_t block(std::initializer_list<float> values) {
        const uint16_t o = next;
        for (float v : values)
            in.v[next++] = v;
        return o;
    }
    int node(GraphOp op, LeafKind kind, uint8_t flags, int a, int b, uint16_t param) {
        GraphNode &n = g.node[g.nodeCount];
        n = GraphNode{op, kind, flags, 0, uint16_t(a), uint16_t(b), param};
        return g.nodeCount++;
    }
    int lambert(float c, float w = 1.0f) { return node(GraphOp::Leaf, LeafKind::Diffuse, 0, 0, 0, block({w, c, c, c, 0.0f})); }
    int diffuse(LeafKind k, Spectrum c, float r, uint8_t flags = 0) {
        return node(GraphOp::Leaf, k, flags, 0, 0, block({1.0f, c.x, c.y, c.z, r}));
    }
    int dielectric(uint8_t mode, float ior, float alpha, float w = 1.0f, Spectrum tint = spectrum(1.0f)) {
        return node(GraphOp::Leaf, LeafKind::Dielectric, mode, 0, 0,
                    block({w, tint.x, tint.y, tint.z, ior, alpha, alpha, 0.0f, 1.5f}));
    }
    int conductor(Spectrum n, Spectrum k, float alpha) {
        return node(GraphOp::Leaf, LeafKind::Conductor, 0, 0, 0,
                    block({1.0f, n.x, n.y, n.z, k.x, k.y, k.z, alpha, alpha, 0.0f, 1.5f}));
    }
    int schlick(Spectrum c0, Spectrum c82, Spectrum c90, float alpha) {
        return node(GraphOp::Leaf, LeafKind::Schlick, kLeafReflect, 0, 0,
                    block({1.0f, c0.x, c0.y, c0.z, c82.x, c82.y, c82.z, c90.x, c90.y, c90.z, 5.0f, alpha, alpha, 0.0f,
                           1.5f}));
    }
    int sheen(float r, bool zeltner) {
        return node(GraphOp::Leaf, LeafKind::Sheen, zeltner ? kLeafZeltner : 0, 0, 0, block({1.0f, 1.0f, 1.0f, 1.0f, r}));
    }
    int mix(int fg, int bg, float m) { return node(GraphOp::Mix, LeafKind::Diffuse, 0, fg, bg, block({m})); }
    int multiply(int a, Spectrum c) { return node(GraphOp::Multiply, LeafKind::Diffuse, 0, a, 0, block({c.x, c.y, c.z})); }
    int add(int a, int b) { return node(GraphOp::Add, LeafKind::Diffuse, 0, a, b, 0); }
    int layer(int top, int base) { return node(GraphOp::Layer, LeafKind::Diffuse, 0, top, base, 0); }
    void finish(int root) {
        finalizeClosureGraph(g);
        g.segmentCount = 1;
        g.segment[0] = GraphSegment{SurfaceModel::Graph, uint8_t(root), 0, uint8_t(g.emissionCount), 0, kNoSlot, kNoSlot};
        in.graph = &g;
        in.size = next;
    }
};

ClosureList build(const SurfaceInputs &in, Vec3f wo, const ShadingGeometry &geo = kFlat) {
    ClosureList cl;
    buildClosure(in, geo, wo, context(), cl);
    return cl;
}

// Directional albedo at wo (all events; refracted samples in energy units).
Spectrum albedo(const SurfaceInputs &in, Vec3f wo, uint32_t n = 1u << 18, uint64_t seed = 7) {
    const ClosureList cl = build(in, wo);
    Pcg32 rng(seed);
    Spectrum sum = spectrum(0.0f);
    for (uint32_t i = 0; i < n; ++i) {
        ClosureSample s;
        if (sampleClosure(cl, wo, {float(rng.uniform()), float(rng.uniform()), float(rng.uniform())}, s))
            sum += s.weight * (s.refract ? s.eta * s.eta : 1.0f);
    }
    return sum * (1.0f / float(n));
}

LobeUnderTest adapt(const SurfaceInputs &in) {
    LobeUnderTest l;
    l.sample = [in](V3 wo, V3 u, V3 &wi, double &pdf, double &w, bool &delta) {
        const ClosureList cl = build(in, toF(wo));
        ClosureSample s;
        if (!sampleClosure(cl, toF(wo), toF(u), s))
            return false;
        wi = toD(s.wi);
        pdf = s.pdf;
        delta = s.delta;
        w = s.weight[0];
        return true;
    };
    l.pdf = [in](V3 wo, V3 wi) { return double(evalClosure(build(in, toF(wo)), toF(wo), toF(wi)).pdf); };
    l.evalCos = [in](V3 wo, V3 wi) { return double(evalClosure(build(in, toF(wo)), toF(wo), toF(wi)).fcos[0]); };
    return l;
}

// The compensated albedo of a reflection-only dielectric interface (the
// layering oracle shared with the native models).
float interfaceAlbedo(float ior, float alpha, float mu) {
    return closure_detail::dielectricReflectionEnergy(bsdfTables(), GgxDistribution::make(alpha, alpha), ior, mu).albedo;
}

} // namespace

TEST_CASE("Layers scale their base by one minus the top's albedo", "[closure][graph]") {
    for (const float mu : {0.2f, 0.6f, 0.95f}) {
        CAPTURE(mu);
        const Vec3f wo = direction(mu);
        // White furnace: layer(dielectric, white) conserves energy.
        Graph white;
        white.finish(white.layer(white.dielectric(kLeafReflect, 1.5f, 0.25f), white.lambert(1.0f)));
        CHECK_THAT(albedo(white.in, wo).x, WithinAbs(1.0, 5e-3));
        // layer(d, 0.5) = A + (1 - A) 0.5.
        const float A = interfaceAlbedo(1.5f, 0.25f, mu);
        Graph half;
        half.finish(half.layer(half.dielectric(kLeafReflect, 1.5f, 0.25f), half.lambert(0.5f)));
        CHECK_THAT(albedo(half.in, wo).x, WithinAbs(A + (1.0f - A) * 0.5f, 5e-3));
        CHECK_THAT(build(half.in, wo).albedo.x, WithinAbs(A + (1.0f - A) * 0.5f, 1e-5));
        // Nested layers multiply their throughputs.
        const float Ac = interfaceAlbedo(1.6f, 0.05f, mu);
        Graph nested;
        nested.finish(nested.layer(nested.dielectric(kLeafReflect, 1.6f, 0.05f),
                                   nested.layer(nested.dielectric(kLeafReflect, 1.5f, 0.25f), nested.lambert(0.5f))));
        CHECK_THAT(albedo(nested.in, wo).x, WithinAbs(Ac + (1.0f - Ac) * (A + (1.0f - A) * 0.5f), 6e-3));
    }
}

TEST_CASE("Mix, add and multiply are linear", "[closure][graph]") {
    const Vec3f wo = direction(0.5f);
    const Spectrum gold_n{0.183f, 0.421f, 1.373f}, gold_k{3.424f, 2.346f, 1.770f};
    Graph metal;
    metal.finish(metal.conductor(gold_n, gold_k, 0.1f));
    const Spectrum Em = albedo(metal.in, wo);
    // mix(white, gold, 0.3) = 0.3 + 0.7 E_gold.
    Graph mixed;
    mixed.finish(mixed.mix(mixed.lambert(1.0f), mixed.conductor(gold_n, gold_k, 0.1f), 0.3f));
    const Spectrum Emix = albedo(mixed.in, wo);
    for (int c = 0; c < 3; ++c)
        CHECK_THAT(Emix[c], WithinAbs(0.3f + 0.7f * Em[c], 6e-3));
    // add sums (no energy guarantee): 0.3 + 0.4.
    Graph sum;
    sum.finish(sum.add(sum.lambert(0.3f), sum.lambert(0.4f)));
    CHECK_THAT(albedo(sum.in, wo).x, WithinAbs(0.7, 3e-3));
    // multiply scales: 0.5 x white.
    Graph scaled;
    scaled.finish(scaled.multiply(scaled.lambert(1.0f), {0.5f, 0.25f, 1.0f}));
    const Spectrum Es = albedo(scaled.in, wo);
    CHECK_THAT(Es.x, WithinAbs(0.5, 3e-3));
    CHECK_THAT(Es.y, WithinAbs(0.25, 3e-3));
    // Inside a top, spec-linear (D5): layer(multiply(d, 0.5), white) stays
    // a white furnace, layer(multiply(d, 0), 0.5) is the base alone.
    Graph lama;
    lama.finish(lama.layer(lama.multiply(lama.dielectric(kLeafReflect, 1.5f, 0.25f), spectrum(0.5f)), lama.lambert(1.0f)));
    CHECK_THAT(albedo(lama.in, wo).x, WithinAbs(1.0, 5e-3));
    Graph none;
    none.finish(none.layer(none.multiply(none.dielectric(kLeafReflect, 1.5f, 0.25f), spectrum(0.0f)), none.lambert(0.5f)));
    CHECK_THAT(albedo(none.in, wo).x, WithinAbs(0.5, 3e-3));
    // A weighted top: w d + (1 - w A) base.
    const float A = interfaceAlbedo(1.5f, 0.25f, 0.5f);
    Graph weighted;
    weighted.finish(weighted.layer(weighted.dielectric(kLeafReflect, 1.5f, 0.25f, 0.5f), weighted.lambert(0.5f)));
    CHECK_THAT(albedo(weighted.in, wo).x, WithinAbs(0.5f * A + (1.0f - 0.5f * A) * 0.5f, 5e-3));
}

TEST_CASE("Shared subgraphs sum over their paths", "[closure][graph]") {
    // UsdPreviewSurface's skeleton: one base under two layers, mixed.
    const Vec3f wo = direction(0.7f);
    Graph dag;
    const int base = dag.lambert(0.5f);
    const int a = dag.layer(dag.dielectric(kLeafReflect, 1.5f, 0.2f), base);
    const int b = dag.layer(dag.dielectric(kLeafReflect, 2.0f, 0.4f), base);
    dag.finish(dag.mix(a, b, 0.25f));
    const float Aa = interfaceAlbedo(1.5f, 0.2f, 0.7f), Ab = interfaceAlbedo(2.0f, 0.4f, 0.7f);
    const float expected = 0.25f * (Aa + (1.0f - Aa) * 0.5f) + 0.75f * (Ab + (1.0f - Ab) * 0.5f);
    CHECK_THAT(albedo(dag.in, wo).x, WithinAbs(expected, 5e-3));
    CHECK_THAT(build(dag.in, wo).albedo.x, WithinAbs(expected, 1e-5));
}

TEST_CASE("A transmission leaf under its reflection is the native R+T interface", "[closure][graph]") {
    for (const float alpha : {0.0f, 0.3f}) {
        for (const bool inside : {false, true}) {
            CAPTURE(alpha, inside);
            Graph glass;
            glass.finish(glass.layer(glass.dielectric(kLeafReflect, 1.5f, alpha), glass.dielectric(kLeafTransmit, 1.5f, alpha)));
            Graph rt;
            rt.finish(rt.dielectric(kLeafReflect | kLeafTransmit, 1.5f, alpha));
            CHECK(glass.g.refractive);
            const Vec3f wo = inside ? -direction(0.6f) : direction(0.6f);
            if (alpha > 0.0f) {
                const ClosureList a = build(glass.in, wo), b = build(rt.in, wo);
                Pcg32 rng(5);
                for (int i = 0; i < 64; ++i) {
                    const double z = 1 - 2 * rng.uniform(), r = std::sqrt(std::max(0.0, 1 - z * z)),
                                 ph = 6.283185307 * rng.uniform();
                    const Vec3f wi{float(r * std::cos(ph)), float(r * std::sin(ph)), float(z)};
                    const float fa = evalClosure(a, wo, wi).fcos.x, fb = evalClosure(b, wo, wi).fcos.x;
                    CHECK_THAT(fa, WithinAbs(fb, 1e-4 * std::max(1.0f, fb)));
                }
            }
            // Both conserve energy (the smooth interface: total 1).
            CHECK_THAT(albedo(glass.in, wo).x, WithinAbs(albedo(rt.in, wo).x, 6e-3));
        }
    }
    // Alone, a smooth transmission passes everything (Karma 1.000).
    Graph t;
    t.finish(t.dielectric(kLeafTransmit, 1.5f, 0.0f));
    CHECK_THAT(albedo(t.in, direction(0.4f)).x, WithinAbs(1.0, 1e-3));
}

TEST_CASE("Reflections over refraction drop out from inside", "[closure][graph]") {
    // A coat over glass: from inside only the interface remains.
    Graph coated;
    coated.finish(coated.layer(coated.dielectric(kLeafReflect, 1.6f, 0.05f),
                               coated.layer(coated.dielectric(kLeafReflect, 1.5f, 0.2f),
                                            coated.dielectric(kLeafTransmit, 1.5f, 0.2f))));
    const Vec3f wo = -direction(0.6f);
    const ClosureList cl = build(coated.in, wo);
    REQUIRE(cl.count == 1);
    CHECK(cl.lobe[0].type == LobeType::Dielectric);
    CHECK(cl.lobe[0].dielectric.mode == DielectricLobe::kBoth);
    CHECK_THAT(cl.lobe[0].dielectric.eta, WithinAbs(1.0f / 1.5f, 1e-6));
}

TEST_CASE("Burley and Hoffman leaves follow MaterialX", "[closure][graph]") {
    // Burley: the MaterialX fit of its albedo (within the fit's accuracy).
    for (const float r : {0.0f, 0.5f, 1.0f})
        for (const float mu : {0.2f, 0.7f}) {
            CAPTURE(r, mu);
            Graph b;
            b.finish(b.diffuse(LeafKind::Burley, spectrum(1.0f), r));
            CHECK_THAT(albedo(b.in, direction(mu)).x, WithinRel(double(BurleyLobe::albedoFit(mu, r)), 0.05));
        }
    // Generalized Schlick with color82 0.5, smooth: MaterialX's
    // mx_fresnel_hoffman_schlick, written out here (Karma's bin means over
    // mu 0.1-0.2: 0.4508; MaterialX 0.4520).
    auto hoffman = [](double mu) {
        const double cmax = 1.0 / 7.0, factor = 1.0 / (cmax * std::pow(1.0 - cmax, 6.0));
        const double F0 = 0.8, F82 = 0.5, F90 = 1.0;
        const double a = (F0 + (F90 - F0) * std::pow(1.0 - cmax, 5.0)) * (1.0 - F82) * factor;
        return F0 + (F90 - F0) * std::pow(1.0 - mu, 5.0) - a * mu * std::pow(1.0 - mu, 6.0);
    };
    Graph s;
    s.finish(s.schlick(spectrum(0.8f), spectrum(0.5f), spectrum(1.0f), 0.0f));
    for (const float mu : {0.1f, 0.15f, 0.2f, 0.5f, 0.999f}) {
        CAPTURE(mu);
        CHECK_THAT(albedo(s.in, direction(mu)).x, WithinAbs(hoffman(mu), 2e-3));
    }
    // color82 1 is the plain Schlick curve, rough: the same as the native
    // UsdPreviewSurface layer helper.
    Graph plain;
    plain.finish(plain.layer(plain.schlick({0.9f, 0.5f, 0.1f}, spectrum(1.0f), spectrum(1.0f), 0.2f), plain.lambert(1.0f)));
    const Spectrum E = albedo(plain.in, direction(0.5f));
    for (int c = 0; c < 3; ++c)
        CHECK_THAT(E[c], WithinAbs(1.0, 6e-3)); // the spectral layering conserves energy
}

TEST_CASE("Sheen leaves layer like the native models", "[closure][graph]") {
    for (const bool zeltner : {false, true}) {
        Graph g;
        g.finish(g.layer(g.sheen(0.5f, zeltner), g.lambert(1.0f)));
        CHECK_THAT(albedo(g.in, direction(0.5f)).x, WithinAbs(1.0, 6e-3));
    }
}

TEST_CASE("Graph emission: uniform and generalized Schlick", "[closure][graph]") {
    Graph g;
    const int root = g.lambert(0.5f);
    g.g.emission[0] = {g.block({0.3f, 0.6f, 0.9f}), kNoSlot};
    g.g.emission[1] = {g.block({1.0f, 1.0f, 1.0f}), g.block({1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 5.0f})};
    g.g.emissionCount = 2;
    g.finish(root);
    const float mu = 0.4f;
    const ClosureList cl = build(g.in, direction(mu));
    const float schlick = 1.0f - std::pow(1.0f - mu, 5.0f);
    CHECK_THAT(cl.emission.x, WithinAbs(0.3f + schlick, 1e-5));
    CHECK_THAT(cl.emission.z, WithinAbs(0.9f + schlick, 1e-5));
}

TEST_CASE("Surface mixes blend native segments linearly", "[closure][graph]") {
    // mix_surfaceshader(Standard Surface copper, OpenPBR defaults, 0.25):
    // 0.25 SS + 0.75 OpenPBR for every direction (Karma: per pixel).
    SurfaceInputs ss = standardSurfaceDefaults();
    ss.set(sf::metalness, 1.0f);
    ss.set(sf::baseColor, {0.95f, 0.64f, 0.54f});
    ss.set(sf::specularRoughness, 0.3f);
    ss.set(sf::coatWeight, 0.5f);
    const SurfaceInputs op = openPbrDefaults();
    Graph mixed;
    const uint16_t a = mixed.next, b = uint16_t(a + sf::kCount);
    std::copy(ss.v, ss.v + sf::kCount, mixed.in.v + a);
    std::copy(op.v, op.v + sf::kCount, mixed.in.v + b);
    mixed.next = uint16_t(b + sf::kCount);
    const uint16_t wa = mixed.block({0.25f}), wb = mixed.block({0.75f});
    mixed.g.segmentCount = 2;
    mixed.g.segment[0] = GraphSegment{SurfaceModel::StandardSurface, ClosureGraph::kNoNode, 0, 0, a, wa, kNoSlot};
    mixed.g.segment[1] = GraphSegment{SurfaceModel::OpenPBR, ClosureGraph::kNoNode, 0, 0, b, wb, kNoSlot};
    mixed.in.graph = &mixed.g;
    mixed.in.size = mixed.next;
    const Vec3f wo = direction(0.55f);
    const ClosureList m = build(mixed.in, wo), A = build(ss, wo), B = build(op, wo);
    Pcg32 rng(9);
    for (int i = 0; i < 64; ++i) {
        const double z = rng.uniform(), r = std::sqrt(std::max(0.0, 1 - z * z)), ph = 6.283185307 * rng.uniform();
        const Vec3f wi{float(r * std::cos(ph)), float(r * std::sin(ph)), float(z)};
        const Spectrum fm = evalClosure(m, wo, wi).fcos;
        const Spectrum fe = evalClosure(A, wo, wi).fcos * 0.25f + evalClosure(B, wo, wi).fcos * 0.75f;
        for (int c = 0; c < 3; ++c)
            CHECK_THAT(fm[c], WithinAbs(fe[c], 1e-5 + 1e-4 * fe[c]));
    }
    for (int c = 0; c < 3; ++c)
        CHECK_THAT(m.albedo[c], WithinAbs(0.25f * A.albedo[c] + 0.75f * B.albedo[c], 1e-5));
    CHECK_THAT(albedo(mixed.in, wo).x, WithinAbs(0.25f * albedo(ss, wo).x + 0.75f * albedo(op, wo).x, 6e-3));
}

TEST_CASE("Surface mixes pick segments by their estimates, not their albedo", "[closure][graph]") {
    // mix_surfaceshader(unlit pass-through, diffuse 0.8, 0.5): the unlit lobe
    // has no albedo, but carries 0.5 of the 0.9 total; its pick must not fall
    // to the 1e-3 floor (fireflies of weight ~500).
    SurfaceInputs unlit = unlitDefaults();
    unlit.set(sf::emissionWeight, 0.0f);
    unlit.set(sf::transmissionWeight, 1.0f);
    SurfaceInputs ss = standardSurfaceDefaults();
    ss.set(sf::specularWeight, 0.0f);
    Graph mixed;
    const uint16_t a = mixed.next, b = uint16_t(a + sf::kCount);
    std::copy(unlit.v, unlit.v + sf::kCount, mixed.in.v + a);
    std::copy(ss.v, ss.v + sf::kCount, mixed.in.v + b);
    mixed.next = uint16_t(b + sf::kCount);
    const uint16_t wa = mixed.block({0.5f}), wb = mixed.block({0.5f});
    mixed.g.segmentCount = 2;
    mixed.g.segment[0] = GraphSegment{SurfaceModel::Unlit, ClosureGraph::kNoNode, 0, 0, a, wa, kNoSlot};
    mixed.g.segment[1] = GraphSegment{SurfaceModel::StandardSurface, ClosureGraph::kNoNode, 0, 0, b, wb, kNoSlot};
    mixed.in.graph = &mixed.g;
    mixed.in.size = mixed.next;
    const ClosureList cl = build(mixed.in, direction(0.7f));
    REQUIRE(cl.count == 2);
    int through = -1;
    for (int i = 0; i < cl.count; ++i)
        if (cl.lobe[i].type == LobeType::Dielectric)
            through = i;
    REQUIRE(through >= 0);
    CHECK_THAT(cl.lobe[through].pick, WithinAbs(0.5 / (0.5 + 0.5 * 0.8), 1e-3));
    // The pass-through's sample weight is then about its share, not ~500.
    ClosureSample s;
    REQUIRE(sampleClosure(cl, direction(0.7f), {0.5f, 0.5f, through == 0 ? 0.1f : 0.9f}, s));
    CHECK(s.delta);
    CHECK_THAT(s.weight.x, WithinAbs(0.9, 2e-3));
}

TEST_CASE("A full lobe list keeps the largest lobes", "[closure][graph]") {
    // 20 Lambert leaves of increasing weight, summed.
    Graph g;
    int root = g.lambert(1.0f, 0.01f);
    for (int i = 1; i < 20; ++i)
        root = g.add(root, g.lambert(1.0f, 0.01f * float(i + 1)));
    g.finish(root);
    const ClosureList cl = build(g.in, direction(0.5f));
    CHECK(cl.count == ClosureList::kMaxLobes);
    CHECK(cl.overflow);
    float smallest = 1.0f;
    for (int i = 0; i < cl.count; ++i)
        smallest = std::min(smallest, cl.lobe[i].weight.x);
    CHECK_THAT(smallest, WithinAbs(0.05, 1e-6)); // weights 0.05 .. 0.20 kept
}

TEST_CASE("Random graphs sample their mixture (chi-square)", "[closure][graph][chi2]") {
    Pcg32 params(77);
    for (int set = 0; set < 6; ++set) {
        Graph g;
        // Up to 8 leaves of every kind, combined at random.
        std::vector<int> pool;
        const int leaves = 2 + int(params.uniform() * 6.0);
        for (int i = 0; i < leaves; ++i) {
            const float c = float(0.2 + 0.7 * params.uniform()), r = float(0.1 + 0.8 * params.uniform());
            switch (int(params.uniform() * 7.0)) {
            case 0:
                pool.push_back(g.diffuse(LeafKind::Diffuse, spectrum(c), r));
                break;
            case 1:
                pool.push_back(g.diffuse(LeafKind::Diffuse, spectrum(c), r, kLeafEnergyComp));
                break;
            case 2:
                pool.push_back(g.diffuse(LeafKind::Burley, spectrum(c), r));
                break;
            case 3:
                pool.push_back(g.dielectric(kLeafReflect, 1.3f + c, 0.1f + 0.5f * r));
                break;
            case 4:
                pool.push_back(g.conductor({0.2f, 0.4f, 1.3f}, {3.4f, 2.3f, 1.8f}, 0.1f + 0.5f * r));
                break;
            case 5:
                pool.push_back(g.schlick(spectrum(c), spectrum(0.7f), spectrum(1.0f), 0.1f + 0.5f * r));
                break;
            default:
                pool.push_back(g.sheen(r, params.uniform() < 0.5));
                break;
            }
        }
        while (pool.size() > 1) {
            const int b = pool.back();
            pool.pop_back();
            const int a = pool.back();
            pool.pop_back();
            const double k = params.uniform();
            pool.push_back(k < 0.4 ? g.layer(a, b) : k < 0.7 ? g.mix(a, b, float(params.uniform())) : g.add(a, b));
        }
        g.finish(pool[0]);
        const LobeUnderTest lobe = adapt(g.in);
        Pcg32 rng(2000 + set);
        for (const float mu : {0.35f, 0.85f}) {
            INFO("graph " << set << ", cos(wo) " << mu);
            const Chi2Result r = nova::test::chi2Test(lobe, toD(direction(mu)), rng, 1 << 16, 32, 5.0, 0.01, 12);
            INFO(r.message << " (p " << r.pValue << ")");
            CHECK(r.pass);
        }
    }
}
