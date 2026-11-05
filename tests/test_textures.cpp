// Texture lookups in the core (docs/design/M3.md, M3.8): the reference
// sampler's address modes (the wrap table of docs/research/m3/textures.md 8,
// which OIIO and Karma give as well), grey to RGB, and Tex2D through the
// compiler and every executor, with per-lane fallbacks.

#include "core/shading/compiler.h"
#include "core/shading/vm.h"
#include "core/texture/memory_sampler.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace nova;
using namespace nova::shading;

namespace {

float lookup1(const MemorySampler &ms, TextureId id, TexSlot slot, float s, float t, uint8_t *statusOut = nullptr) {
    TexSampler smp;
    smp.texture = id;
    smp.slot = slot;
    TexQuery q;
    q.count = 1;
    q.s = &s;
    q.t = &t;
    float out[4] = {};
    uint8_t status = 0;
    ms.sample2D(smp, q, out, &status);
    if (statusOut)
        *statusOut = status;
    return status ? -1.0f : out[0];
}

} // namespace

TEST_CASE("Address modes on a 4x1 ramp", "[texture]") {
    MemorySampler ms;
    const TextureId ramp = ms.add(4, 1, 1, {1.0f, 2.0f, 3.0f, 4.0f});
    struct Row {
        TexWrap wrap;
        float expect[3];
    };
    // s = -0.1, 1.1, 1.6 with closest filtering; -1 marks the fallback.
    const Row rows[] = {
        {TexWrap::Clamp, {1, 4, 4}},       {TexWrap::Periodic, {4, 1, 3}},  {TexWrap::Mirror, {1, 4, 2}},
        {TexWrap::Black, {0, 0, 0}},       {TexWrap::Constant, {-1, -1, -1}},
    };
    const float s[3] = {-0.1f, 1.1f, 1.6f};
    for (const Row &r : rows) {
        TexSlot slot;
        slot.wrapS = r.wrap;
        slot.wrapT = TexWrap::Clamp;
        slot.filter = TexFilter::Closest;
        slot.channels = 1;
        for (int i = 0; i < 3; ++i) {
            INFO("wrap " << int(r.wrap) << " s " << s[i]);
            CHECK(lookup1(ms, ramp, slot, s[i], 0.5f) == r.expect[i]);
        }
        CHECK(lookup1(ms, ramp, slot, 0.3f, 0.5f) == 2.0f); // inside: every mode reads the texel
    }
    // Bilinear between texel centers, and v up: row 0 is the top.
    TexSlot lin;
    lin.wrapS = lin.wrapT = TexWrap::Clamp;
    lin.channels = 1;
    CHECK(lookup1(ms, ramp, lin, 0.25f, 0.5f) == 1.5f);
    const TextureId column = ms.add(1, 2, 1, {10.0f, 20.0f}); // top 10, bottom 20
    lin.filter = TexFilter::Closest;
    CHECK(lookup1(ms, column, lin, 0.5f, 0.75f) == 10.0f);
    CHECK(lookup1(ms, column, lin, 0.5f, 0.25f) == 20.0f);
}

TEST_CASE("Grey textures read as RGB, a missing alpha is 1", "[texture]") {
    MemorySampler ms;
    const TextureId grey = ms.add(1, 1, 1, {0.25f});
    const TextureId rgb = ms.add(1, 1, 3, {0.1f, 0.2f, 0.3f});
    TexSampler smp;
    smp.slot.channels = 4;
    float s = 0.5f, t = 0.5f, out[4];
    uint8_t status;
    TexQuery q;
    q.count = 1;
    q.s = &s;
    q.t = &t;
    smp.texture = grey;
    ms.sample2D(smp, q, out, &status);
    CHECK(status == 0);
    CHECK((out[0] == 0.25f && out[1] == 0.25f && out[2] == 0.25f && out[3] == 1.0f));
    smp.texture = rgb;
    ms.sample2D(smp, q, out, &status);
    CHECK((out[0] == 0.1f && out[1] == 0.2f && out[2] == 0.3f && out[3] == 1.0f));
    smp.texture = 7; // unknown id
    ms.sample2D(smp, q, out, &status);
    CHECK(status == 1);
}

TEST_CASE("Tex2D runs in every executor with per-lane fallbacks", "[texture][vm]") {
    MemorySampler ms;
    std::vector<float> texels;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            for (int c = 0; c < 3; ++c)
                texels.push_back(float(x) + 10.0f * float(y) + 100.0f * float(c));
    const TextureId tex = ms.add(8, 8, 3, texels);

    // uv = P.xy; Constant addressing: lanes outside [0, 1] take the fallback.
    IrGraph g;
    IrBuilder b(g);
    TexSlot slot;
    slot.wrapS = slot.wrapT = TexWrap::Constant;
    slot.channels = 3;
    const uint32_t s0 = b.textureSlot(slot);
    const Ref P = b.global(Global::P);
    const Ref color = b.tex2D(s0, b.swizzle(P, 0, 1), b.constant(-1.0f, -2.0f, -3.0f));
    b.output(surface_field::baseColor, 3, color);
    const CompileResult r = compile(g);
    REQUIRE(r.program);
    const Program &p = *r.program;
    CHECK(!p.varyingFree());
    REQUIRE(p.textures.size() == 1);

    std::vector<float> U;
    p.initUniforms(nullptr, U);
    const TexSampler samplers[1] = {{tex, p.textures[0]}};
    const TextureContext ctx{&ms, samplers, 1};
    const int lanes = 37;
    std::vector<Vec3f> pos(lanes);
    for (int l = 0; l < lanes; ++l)
        pos[size_t(l)] = {float(l % 9) * 0.137f - 0.05f, float(l / 9) * 0.29f - 0.1f, 0.0f};
    auto run = [&](auto lanesTag, const TextureContext *context) {
        constexpr int N = decltype(lanesTag)::value;
        std::vector<float> out(size_t(lanes) * 3), R(size_t(p.rowCount) * N);
        for (int base = 0; base < lanes; base += N) {
            for (int l = 0; l < N; ++l) {
                const Vec3f q = pos[size_t(std::min(base + l, lanes - 1))];
                for (const GlobalBinding &gb : p.globals)
                    for (int k = 0; k < 3; ++k)
                        R[(gb.row + size_t(k)) * N + size_t(l)] = q[k];
            }
            execute<N>(p.code.data(), p.code.size(), R.data(), U.data(), context);
            const OutputBinding &o = p.outputs[0];
            for (int l = 0; l < N && base + l < lanes; ++l)
                for (int k = 0; k < 3; ++k)
                    out[size_t(base + l) * 3 + size_t(k)] = R[(o.index + size_t(k)) * N + size_t(l)];
        }
        return out;
    };
    const std::vector<float> a = run(std::integral_constant<int, 1>{}, &ctx);
    const std::vector<float> b8 = run(std::integral_constant<int, 8>{}, &ctx);
    const std::vector<float> b32 = run(std::integral_constant<int, 32>{}, &ctx);
    CHECK(std::memcmp(a.data(), b8.data(), a.size() * 4) == 0);
    CHECK(std::memcmp(a.data(), b32.data(), a.size() * 4) == 0);
    int inside = 0, outside = 0;
    for (int l = 0; l < lanes; ++l) {
        const Vec3f q = pos[size_t(l)];
        TexSampler smp{tex, p.textures[0]};
        float s = q.x, t = q.y, ref[3];
        uint8_t status;
        TexQuery query;
        query.count = 1;
        query.s = &s;
        query.t = &t;
        ms.sample2D(smp, query, ref, &status);
        for (int k = 0; k < 3; ++k)
            CHECK(a[size_t(l) * 3 + size_t(k)] == (status ? -1.0f - float(k) : ref[k]));
        (status ? outside : inside) += 1;
    }
    CHECK(inside > 10);
    CHECK(outside > 5);
    // No sampler, or no texture bound: the fallback everywhere.
    const std::vector<float> none = run(std::integral_constant<int, 8>{}, nullptr);
    const TexSampler unbound[1] = {{kNoTexture, p.textures[0]}};
    const TextureContext unboundCtx{&ms, unbound, 1};
    const std::vector<float> empty = run(std::integral_constant<int, 8>{}, &unboundCtx);
    for (int l = 0; l < lanes; ++l)
        for (int k = 0; k < 3; ++k) {
            CHECK(none[size_t(l) * 3 + size_t(k)] == -1.0f - float(k));
            CHECK(empty[size_t(l) * 3 + size_t(k)] == -1.0f - float(k));
        }
}

TEST_CASE("Texture slots are structure, the textures are values", "[texture][vm]") {
    auto build = [](TexWrap wrap, float fallback) {
        IrGraph g;
        IrBuilder b(g);
        TexSlot slot;
        slot.wrapS = wrap;
        const uint32_t s = b.textureSlot(slot);
        // A constant uv is still looked up per hit: Tex2D never folds.
        const float uv[2] = {0.5f, 0.5f};
        b.output(surface_field::baseColor, 3, b.tex2D(s, b.constant(uv, 2), b.newParam(3)));
        (void)fallback;
        return compile(g).program;
    };
    const ProgramPtr a = build(TexWrap::Periodic, 0.0f), b = build(TexWrap::Periodic, 1.0f), c = build(TexWrap::Clamp, 0.0f);
    REQUIRE((a && b && c));
    CHECK(a->code.size() == 1);
    CHECK(a->structureHash == b->structureHash);
    CHECK(a->structureHash != c->structureHash);
}

// Data replaced under an id (as Hydra prepares a changed file again) is what
// the sampler and textureData return, under a new generation that tells a
// device copy it is stale; other ids keep theirs.
TEST_CASE("Replaced texture data gets a new generation", "[texture]") {
    MemorySampler ms;
    const TextureId a = ms.add(1, 1, 1, {0.25f});
    const TextureId b = ms.add(1, 1, 1, {0.5f});
    const uint32_t before = ms.textureGeneration(a), other = ms.textureGeneration(b);
    TexSlot slot;
    slot.channels = 1;
    CHECK(lookup1(ms, a, slot, 0.5f, 0.5f) == 0.25f);
    ms.replace(a, 2, 1, 3, {0.75f, 0.0f, 0.0f, 0.75f, 0.0f, 0.0f});
    CHECK(ms.textureGeneration(a) != before);
    CHECK(ms.textureGeneration(b) == other);
    CHECK(lookup1(ms, a, slot, 0.5f, 0.5f) == 0.75f);
    TexData data;
    REQUIRE(ms.textureData(a, data));
    REQUIRE(data.tiles.size() == 1);
    REQUIRE(data.tiles[0].levels.size() == 1);
    CHECK(data.tiles[0].levels[0].width == 2);
    CHECK(data.tiles[0].levels[0].texels[0] == 0.75f);
    CHECK(ms.textureGeneration(7) == 0); // unknown id
}
