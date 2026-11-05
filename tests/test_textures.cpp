// Texture lookups in the core (docs/design/M3.md, M3.8): the reference
// sampler's address modes (the wrap table of docs/research/m3/textures.md 8,
// which OIIO and Karma give as well), grey to RGB, and Tex2D through the
// compiler and every executor, with per-lane fallbacks.

#include "core/shading/compiler.h"
#include "core/shading/vm.h"
#include "core/texture/memory_sampler.h"
#include "core/texture/texel_stream.h"
#include "core/texture/texture_budget.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <map>
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

namespace {

// Keeps what textureLevels hands over: per level its RGBA float texels (rows
// from the top) and the largest band.
struct LevelCollector final : TexRowSink {
    std::map<uint32_t, std::vector<float>> levels;
    std::map<uint32_t, int> widths, heights;
    int largestBand = 0;
    bool put(const TexRows &r) override {
        std::vector<float> &l = levels[r.level];
        l.resize(size_t(r.width) * size_t(r.height) * 4);
        std::memcpy(&l[size_t(r.y) * size_t(r.width) * 4], r.texels, size_t(r.width) * size_t(r.rows) * 16);
        widths[r.level] = r.width;
        heights[r.level] = r.height;
        largestBand = std::max(largestBand, r.rows);
        return true;
    }
};

// A MIP chain below a w x h level (levels 1 ... 1 x 1), level l constant
// `value(l)` in each of `channels` channels.
template <class F> std::vector<std::vector<float>> constantMips(int w, int h, int channels, F value) {
    std::vector<std::vector<float>> mips;
    for (uint32_t l = 1; std::max(w >> (l - 1), h >> (l - 1)) > 1; ++l)
        mips.emplace_back(size_t(std::max(1, w >> l)) * size_t(std::max(1, h >> l)) * size_t(channels), value(l));
    return mips;
}

} // namespace

// Data replaced under an id (as Hydra prepares a changed file again) is what
// the sampler and textureLevels return, under a new generation that tells a
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
    TexInfo info;
    REQUIRE(ms.textureInfo(a, info));
    REQUIRE(info.tiles.size() == 1);
    CHECK(info.tiles[0].width == 2);
    CHECK(info.tiles[0].stored == 1);
    TexLevelRequest request;
    request.half = false;
    LevelCollector data;
    REQUIRE(ms.textureLevels(a, request, data));
    REQUIRE(data.levels.size() == 1);
    CHECK(data.widths[0] == 2);
    CHECK(data.levels[0][0] == 0.75f);
    CHECK(data.levels[0][3] == 1.0f); // a missing alpha
    CHECK(ms.textureGeneration(7) == 0); // unknown id
}

// R1-48: the device texture budget picks each tile's levels from the
// textures' shapes before anything is read, and textureLevels reads only
// those, a band of rows at a time; a texture without stored MIP levels gets a
// coarser level made while its finest is read once.
TEST_CASE("The texture budget reads only the levels it keeps", "[texture][budget]") {
    MemorySampler ms;
    // a: 256 x 256 RGB, level l constant l; b: 64 x 64 grey, level l
    // constant 10 + l; c: 128 x 128 RGBA without MIP levels, a 1-texel
    // checker in red (green its complement), blue 0.25, alpha 0.5.
    const auto constant = [](int w, int h, int channels, float v) {
        return std::vector<float>(size_t(w) * size_t(h) * size_t(channels), v);
    };
    const TextureId a = ms.add(256, 256, 3, constant(256, 256, 3, 0.0f),
                               constantMips(256, 256, 3, [](uint32_t l) { return float(l); }));
    const TextureId b = ms.add(64, 64, 1, constant(64, 64, 1, 10.0f),
                               constantMips(64, 64, 1, [](uint32_t l) { return 10.0f + float(l); }));
    std::vector<float> checker;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const float v = float((x + y) & 1);
            checker.insert(checker.end(), {v, 1.0f - v, 0.25f, 0.5f});
        }
    const TextureId c = ms.add(128, 128, 4, checker);
    TexInfo info[3];
    REQUIRE(ms.textureInfo(a, info[0]));
    REQUIRE(ms.textureInfo(b, info[1]));
    REQUIRE(ms.textureInfo(c, info[2]));
    CHECK(info[0].tiles[0].stored == 9);
    CHECK(info[1].tiles[0].stored == 7);
    CHECK(info[2].tiles[0].stored == 1);
    const std::vector<const TexInfo *> textures = {&info[0], &info[1], &info[2]};

    // Everything fits: all stored levels (16 bytes per texel, test data).
    const TexBudgetPlan all = planTextureBudget(textures, size_t(1) << 30);
    CHECK(!all.reduced);
    CHECK(all.bytes == 16 * (87381 + 5461 + 16384));
    for (int i = 0; i < 3; ++i) {
        CHECK(all.textures[size_t(i)][0].first == 0);
        CHECK(all.textures[size_t(i)][0].count == info[i].tiles[0].stored);
    }

    // 200 000 bytes: capped at 32 texels (60 064 bytes), then a and b get a
    // level more in order of first use (c's would not fit any more).
    const size_t budget = 200000;
    const TexBudgetPlan plan = planTextureBudget(textures, budget);
    CHECK(plan.reduced);
    CHECK(plan.cap == 5);
    CHECK(plan.bytes <= budget);
    const TexTilePlan pa = plan.textures[0][0], pb = plan.textures[1][0], pc = plan.textures[2][0];
    CHECK((pa.first == 2 && pa.count == 7));
    CHECK((pb.first == 0 && pb.count == 7));
    CHECK((pc.first == 2 && pc.count == 1)); // a 32 x 32 level made from level 0
    CHECK(plan.bytes == pa.bytes + pb.bytes + pc.bytes);

    // Reading the plan reads only those levels: a's levels 2 to 8, b's
    // levels 0 to 6 and c's level 0 once (for its 32 x 32 level).
    const TextureId ids[3] = {a, b, c};
    LevelCollector got[3];
    for (int i = 0; i < 3; ++i) {
        const TexTilePlan &p = plan.textures[size_t(i)][0];
        TexLevelRequest request;
        request.first = p.first;
        request.count = p.count;
        request.half = false;
        REQUIRE(ms.textureLevels(ids[i], request, got[i]));
        CHECK(got[i].levels.size() == p.count);
        CHECK(got[i].largestBand <= LevelWriter::kBandRows);
    }
    CHECK(ms.texelsRead() == 5461 + 5461 + 128 * 128);
    CHECK(got[0].levels.count(0) == 0);
    CHECK(got[0].levels.count(1) == 0);
    REQUIRE(got[0].widths[2] == 64);
    CHECK((got[0].levels[2][0] == 2.0f && got[0].levels[2][3] == 1.0f));
    CHECK(got[0].levels[8][0] == 8.0f);
    CHECK((got[1].levels[0][2] == 10.0f && got[1].levels[6][0] == 16.0f)); // grey to RGB
    REQUIRE(got[2].widths[2] == 32);
    const std::vector<float> &made = got[2].levels[2];
    bool means = true;
    for (size_t i = 0; i < made.size(); i += 4)
        means = means && made[i] == 0.5f && made[i + 1] == 0.5f && made[i + 2] == 0.25f && made[i + 3] == 0.5f;
    CHECK(means); // box means of 4 x 4 checker texels

    // Half floats: the same texels as IEEE halfs.
    struct Halves final : TexRowSink {
        std::vector<uint16_t> texels;
        bool put(const TexRows &r) override {
            const uint16_t *p = static_cast<const uint16_t *>(r.texels);
            texels.insert(texels.end(), p, p + size_t(r.width) * size_t(r.rows) * 4);
            return true;
        }
    } halves;
    TexLevelRequest request;
    request.first = 8;
    REQUIRE(ms.textureLevels(a, request, halves));
    REQUIRE(halves.texels.size() == 4);
    CHECK(halves.texels[0] == floatToHalf(8.0f));
    CHECK(halves.texels[3] == floatToHalf(1.0f));

    // A single texture over the budget keeps the first chain that fits, as
    // the per-texture policy before (a: 100 000 bytes, levels 2 on).
    const TexBudgetPlan one = planTextureBudget({&info[0]}, 100000);
    CHECK(one.textures[0][0].first == 2);
    // Nothing fits: every tile keeps its coarsest level.
    const TexBudgetPlan none = planTextureBudget(textures, 0);
    CHECK((none.textures[0][0].first == 8 && none.textures[0][0].count == 1));
    CHECK((none.textures[2][0].first == 7 && none.textures[2][0].bytes == 16));
}

// The plan over UDIM sets and textures of different sizes: a set's tiles keep
// the same levels, a texture under the cap keeps everything, the cap keeps
// the largest power-of-two size that fits.
TEST_CASE("The texture budget caps the largest textures first", "[texture][budget]") {
    const auto tile = [](uint32_t udim, int size) {
        TexTileInfo t;
        t.udim = udim;
        t.width = t.height = size;
        t.stored = 1;
        while ((size >> (t.stored - 1)) > 1)
            ++t.stored;
        return t;
    };
    TexInfo set; // four 1024 x 1024 tiles, half floats
    set.udim = true;
    for (uint32_t u : {1001u, 1002u, 1011u, 1012u})
        set.tiles.push_back(tile(u, 1024));
    TexInfo big, small;
    big.tiles.push_back(tile(0, 2048));
    small.tiles.push_back(tile(0, 128));
    const std::vector<const TexInfo *> textures = {&set, &big, nullptr, &small};
    const TexBudgetPlan full = planTextureBudget(textures, size_t(1) << 30);
    for (size_t budgetMb : {64, 24, 8, 6, 2, 1}) {
        INFO("budget " << budgetMb << " MB");
        const size_t budget = budgetMb << 20;
        const TexBudgetPlan plan = planTextureBudget(textures, budget);
        REQUIRE(plan.textures.size() == 4);
        CHECK(plan.textures[2].empty());
        CHECK(plan.bytes <= budget);
        CHECK(plan.reduced == (full.bytes > budget));
        for (const TexTilePlan &p : plan.textures[0])
            CHECK(p == plan.textures[0][0]); // uniform across the set
        // The kept sizes: at most the cap, or one level more.
        const int cap = plan.reduced ? 1 << plan.cap : 1 << 30;
        CHECK((1024 >> plan.textures[0][0].first) <= 2 * cap);
        CHECK((2048 >> plan.textures[1][0].first) <= 2 * cap);
        // A smaller texture never loses more than a larger one keeps.
        CHECK((2048 >> plan.textures[1][0].first) >= (1024 >> plan.textures[0][0].first) / 2);
        if (128 <= cap)
            CHECK(plan.textures[3][0].first == 0);
    }
    // 8 MB: the cap of 256 texels (3.5 MB) leaves 4.5 MB; a level more takes
    // 8 MB for the set (no), then 2 MB for the 2048 texture (yes).
    const TexBudgetPlan plan = planTextureBudget(textures, size_t(8) << 20);
    CHECK(plan.cap == 8);
    CHECK(plan.textures[0][0].first == 2);
    CHECK(plan.textures[1][0].first == 2);
    CHECK(plan.textures[3][0].first == 0);
}
