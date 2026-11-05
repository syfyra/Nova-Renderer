// Shading IR, compiler and VM (docs/design/M3.md, M3.5): compiler counts
// (folding, CSE, DCE, row allocation, hoisting, structure hash), random DAGs
// evaluated bit-identically by the reference tree walker, the lane executor
// and the batch executors with and without optimization, and VEX's safe
// math (goldens measured with vexexec, tests/vex/safe_math.vfl).

#include "bsdf_stats.h"
#include "core/shading/compiler.h"
#include "core/shading/vm.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::shading;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

ProgramPtr compileOk(const IrGraph &g, bool optimize = true) {
    CompileOptions o;
    o.optimize = optimize;
    CompileResult r = compile(g, o);
    INFO(r.error);
    REQUIRE(r.program);
    return r.program;
}

// Evaluates a one-output program for one shading point (lane executor).
float runScalar(const IrGraph &g, const float *globals3 = nullptr) {
    const ProgramPtr p = compileOk(g);
    std::vector<float> U;
    p->initUniforms(nullptr, U);
    std::vector<float> R(std::max<uint32_t>(p->rowCount, 1));
    for (const GlobalBinding &b : p->globals)
        for (int k = 0; k < globalWidth(b.global); ++k)
            R[b.row + size_t(k)] = globals3 ? globals3[k] : 0.0f;
    execute<1>(p->code.data(), p->code.size(), R.data(), U.data());
    const OutputBinding &o = p->outputs.at(0);
    return o.uniform ? U[o.index] : R[o.index];
}

float unaryValue(Op op, float x) {
    IrGraph g;
    IrBuilder b(g);
    b.output(0, 1, b.unary(op, b.constant(x)));
    return runScalar(g);
}
float binaryValue(Op op, float x, float y) {
    IrGraph g;
    IrBuilder b(g);
    b.output(0, 1, b.binary(op, b.constant(x), b.constant(y)));
    return runScalar(g);
}
float ternaryValue(Op op, float x, float y, float z) {
    IrGraph g;
    IrBuilder b(g);
    b.output(0, 1, b.ternary(op, b.constant(x), b.constant(y), b.constant(z)));
    return runScalar(g);
}

} // namespace

TEST_CASE("Safe math follows VEX", "[shading][vm]") {
    // vexexec tests/vex/safe_math.vex (Houdini 22.0.466).
    CHECK(binaryValue(Op::Div, 1, 0) == 0.0f);
    CHECK(std::signbit(binaryValue(Op::Div, -1, 0)));
    CHECK(binaryValue(Op::Div, 0, 0) == 0.0f);
    CHECK(binaryValue(Op::Mod, 5.5f, 0) == 0.0f);
    CHECK(binaryValue(Op::Mod, -5.5f, 2) == 0.5f);
    CHECK(binaryValue(Op::Mod, 5.5f, -2) == -0.5f);
    CHECK(binaryValue(Op::Mod, -0.5f, 2) == 1.5f);
    CHECK(unaryValue(Op::Sqrt, -4) == 0.0f);
    CHECK(unaryValue(Op::Log, -4) == 0.0f);
    CHECK(unaryValue(Op::Log, 0) == 0.0f);
    CHECK(binaryValue(Op::Pow, -4, 0.5f) == -4.0f); // negative base: rint(exponent)
    CHECK(binaryValue(Op::Pow, -4, 2) == 16.0f);
    CHECK(binaryValue(Op::Pow, 0, -1) == 0.0f);
    CHECK(binaryValue(Op::Pow, -4, 0.25f) == 1.0f);
    CHECK(binaryValue(Op::Pow, -8, 1.0f / 3.0f) == 1.0f);
    CHECK(binaryValue(Op::Pow, -2, 1.5f) == 4.0f);
    CHECK(binaryValue(Op::Pow, -2, -2) == 0.25f);
    CHECK(binaryValue(Op::Pow, -2, 3) == -8.0f);
    CHECK(binaryValue(Op::Pow, -0.5f, 0.5f) == -0.5f);
    CHECK(binaryValue(Op::Pow, 0, 0) == 1.0f);
    CHECK(binaryValue(Op::Pow, 0, 0.5f) == 0.0f);
    CHECK_THAT(binaryValue(Op::Pow, 2, -0.5f), WithinRel(0.707107, 1e-6));
    CHECK_THAT(unaryValue(Op::Asin, 2), WithinAbs(1.5708, 1e-4));
    CHECK_THAT(unaryValue(Op::Acos, -2), WithinAbs(3.14159, 1e-5));
    CHECK(binaryValue(Op::Atan2, 0, 0) == 0.0f);
    CHECK_THAT(binaryValue(Op::Atan2, 1, -1), WithinAbs(2.35619, 1e-5));
    CHECK(std::isinf(unaryValue(Op::Exp, 100)));
    CHECK(unaryValue(Op::Floor, -1.5f) == -2.0f);
    CHECK(unaryValue(Op::Ceil, -1.5f) == -1.0f);
    CHECK(unaryValue(Op::Round, -1.5f) == -1.0f); // VEX rint: floor(x + 0.5)
    CHECK(unaryValue(Op::Round, 2.5f) == 3.0f);
    CHECK(unaryValue(Op::Round, 0.5f) == 1.0f);
    CHECK(unaryValue(Op::Trunc, -1.7f) == -1.0f);
    CHECK(unaryValue(Op::Sign, -3) == -1.0f);
    CHECK(unaryValue(Op::Sign, 0) == 0.0f);
    CHECK(unaryValue(Op::Sign, 2) == 1.0f);
    CHECK(unaryValue(Op::Fract, -1.25f) == 0.75f);
    CHECK(unaryValue(Op::Fract, 1.25f) == 0.25f);
    CHECK(ternaryValue(Op::Smoothstep, 0, 1, 0.25f) == 0.15625f);
    CHECK(ternaryValue(Op::Smoothstep, 1, 0, 0.25f) == 0.84375f); // Karma: reversed edges blend reversed
    CHECK(ternaryValue(Op::Smoothstep, 0.5f, 0.5f, 0.5f) == 0.0f);
    CHECK(ternaryValue(Op::Clamp, 5, 2, 1) == 1.0f);
    CHECK(ternaryValue(Op::Clamp, -5, 2, 1) == 2.0f);
    {
        IrGraph g;
        IrBuilder b(g);
        b.output(0, 3, b.normalize(b.constant(0.0f, 0.0f, 0.0f)));
        CHECK(runScalar(g) == 0.0f);
    }
}

TEST_CASE("The compiler folds, shares and removes", "[shading][compiler]") {
    SECTION("2 * 3 + P.x is two instructions") {
        IrGraph g;
        IrBuilder b(g);
        b.output(0, 1, b.add(b.mul(b.constant(2.0f), b.constant(3.0f)), b.component(b.global(Global::P), 0)));
        const ProgramPtr p = compileOk(g);
        CHECK(p->code.size() == 2);
        const float P[3] = {0.5f, 7.0f, 9.0f};
        CHECK(runScalar(g, P) == 6.5f);
    }
    SECTION("common subexpressions and dead code") {
        IrGraph g;
        IrBuilder b(g);
        const Ref x = b.component(b.global(Global::P), 0);
        const Ref s1 = b.unary(Op::Sin, x), s2 = b.unary(Op::Sin, b.component(b.global(Global::P), 0));
        b.unary(Op::Cos, x); // dead
        b.output(0, 1, b.add(s1, s2));
        const ProgramPtr p = compileOk(g);
        CHECK(p->code.size() == 3); // swizzle, sin, add
        CHECK(compileOk(g, false)->code.size() == 5); // unoptimized: both swizzles and sines
    }
    SECTION("CSE ignores IrNode's padding byte") {
        // Byte 3 (after op and width) is padding nothing initializes: two
        // nodes built at different stack depths may differ only there.
        static_assert(offsetof(IrNode, width) == 2 && offsetof(IrNode, a) == 4, "padding byte at offset 3");
        IrGraph g;
        IrBuilder b(g);
        const Ref n1 = b.global(Global::N), n2 = b.global(Global::N);
        std::memset(reinterpret_cast<unsigned char *>(&g.nodes[n1]) + 3, 0x00, 1);
        std::memset(reinterpret_cast<unsigned char *>(&g.nodes[n2]) + 3, 0xA5, 1);
        b.output(0, 3, b.add(n1, n2));
        const ProgramPtr p = compileOk(g);
        CHECK(p->globals.size() == 1);
        CHECK(p->code.size() == 1);
    }
    SECTION("a 400-op chain needs the globals and 3 rows") {
        IrGraph g;
        IrBuilder b(g);
        Ref x = b.component(b.global(Global::P), 0);
        const Ref n = b.global(Global::N);
        for (int i = 0; i < 400; ++i)
            x = b.add(b.mul(x, b.constant(0.5f + float(i % 7))), b.component(n, i % 3));
        b.output(0, 1, x);
        const ProgramPtr p = compileOk(g);
        CHECK(p->rowCount <= 6 + 3);
    }
    SECTION("a balanced depth-7 tree needs at most 9 rows besides the globals") {
        IrGraph g;
        IrBuilder b(g);
        const Ref P = b.global(Global::P);
        std::vector<Ref> level;
        for (int i = 0; i < 128; ++i) // distinct leaves, one instruction each
            level.push_back(b.dot(P, b.constant(float(i + 1), 1.0f, float(i % 5))));
        while (level.size() > 1) {
            std::vector<Ref> next;
            for (size_t i = 0; i < level.size(); i += 2)
                next.push_back(b.add(level[i], level[i + 1]));
            level = next;
        }
        b.output(0, 1, level[0]);
        const ProgramPtr p = compileOk(g);
        CHECK(p->rowCount <= 3 + 9);
    }
    SECTION("parameter-only subgraphs run in the prologue") {
        IrGraph g;
        IrBuilder b(g);
        uint32_t offset = 0;
        const Ref k = b.newParam(1, &offset);
        b.output(surface_field::baseColor, 3,
                 b.add(b.mul(b.unary(Op::Sin, k), b.param(offset, 1)), b.constant(1.0f, 2.0f, 3.0f)));
        const ProgramPtr p = compileOk(g);
        CHECK(p->code.empty());
        CHECK(p->varyingFree());
        CHECK_FALSE(p->prologue.empty());
        std::vector<float> U;
        const float param = 0.5f;
        p->initUniforms(&param, U);
        REQUIRE(p->outputs.size() == 1);
        CHECK(p->outputs[0].uniform);
        const float expected = std::sin(0.5f) * 0.5f;
        CHECK(U[p->outputs[0].index] == expected + 1.0f);
        CHECK(U[p->outputs[0].index + 2] == expected + 3.0f);
    }
    SECTION("the structure hash ignores parameter values and dead code") {
        auto build = [](bool dead, float constant, const char *primvar) {
            IrGraph g;
            IrBuilder b(g);
            const Ref c = b.primvar(primvar, 3, b.constant(0.18f));
            if (dead)
                b.unary(Op::Exp, b.component(c, 1));
            b.output(surface_field::baseColor, 3, b.mul(c, b.add(b.newParam(1), b.constant(constant))));
            return compileOk(g)->structureHash;
        };
        CHECK(build(false, 1.0f, "displayColor") == build(true, 1.0f, "displayColor"));
        CHECK(build(false, 1.0f, "displayColor") != build(false, 2.0f, "displayColor"));
        CHECK(build(false, 1.0f, "displayColor") != build(false, 1.0f, "Cd"));
    }
}

// ---------------------------------------------------------------------------
// Differential tests
// ---------------------------------------------------------------------------

namespace {

using nova::test::Pcg32;

uint32_t randInt(Pcg32 &rng, uint32_t n) { return rng.next() % n; }

struct RandomGraph {
    IrGraph g;
    int primvarWidths[4] = {0, 0, 0, 0};
};

float randomValue(Pcg32 &rng) {
    static const float special[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, -2.5f, 1e-3f, 100.0f, 3.14159f};
    if (rng.uniform() < 0.3)
        return special[randInt(rng, 10)];
    return float(rng.uniform() * 8.0 - 4.0);
}

RandomGraph randomGraph(Pcg32 &rng) {
    RandomGraph r;
    IrBuilder b(r.g);
    std::vector<Ref> nodes;
    // Leaves.
    nodes.push_back(b.global(Global::P));
    nodes.push_back(b.global(Global::N));
    nodes.push_back(b.global(Global::Time));
    for (int i = 0; i < 4; ++i) {
        const int w = 1 + int(randInt(rng, 4));
        float v[4];
        for (float &x : v)
            x = randomValue(rng);
        nodes.push_back(b.constant(v, w));
    }
    for (int i = 0; i < 3; ++i)
        nodes.push_back(b.newParam(1 + int(randInt(rng, 4))));
    for (int i = 0; i < 2; ++i) {
        const int w = 1 + int(randInt(rng, 4));
        r.primvarWidths[i] = w;
        float v[4];
        for (float &x : v)
            x = randomValue(rng);
        nodes.push_back(b.primvar("pv" + std::to_string(i), w, b.constant(v, w)));
    }
    static const Op unaryOps[] = {Op::Copy, Op::Neg,  Op::Abs, Op::Floor, Op::Ceil, Op::Round, Op::Trunc,
                                  Op::Sign, Op::Fract, Op::Sqrt, Op::Exp, Op::Log, Op::Sin,  Op::Cos,
                                  Op::Tan,  Op::Asin, Op::Acos, Op::Atan, Op::Not};
    static const Op binaryOps[] = {Op::Add, Op::Sub, Op::Mul, Op::Div,   Op::Mod,       Op::Min,   Op::Max,
                                   Op::Pow, Op::Atan2, Op::Greater, Op::GreaterEq, Op::Equal, Op::And, Op::Or};
    static const Op ternaryOps[] = {Op::Clamp, Op::Mix, Op::Smoothstep, Op::Select};
    auto pick = [&]() { return nodes[randInt(rng, uint32_t(nodes.size()))]; };
    auto pickWidth = [&](int w) { // a node of width w or 1, else converted
        const Ref n = pick();
        const int nw = b.width(n);
        return nw == w || nw == 1 ? n : b.convert(n, w);
    };
    const int count = 8 + int(randInt(rng, 40));
    for (int i = 0; i < count; ++i) {
        const uint32_t kind = randInt(rng, 10);
        Ref n;
        if (kind < 2) {
            n = b.unary(unaryOps[randInt(rng, 19)], pick());
        } else if (kind < 4) {
            const int w = 1 + int(randInt(rng, 4));
            n = b.binary(binaryOps[randInt(rng, 14)], pickWidth(w), pickWidth(w));
        } else if (kind < 5) {
            const int w = 1 + int(randInt(rng, 4));
            n = b.ternary(ternaryOps[randInt(rng, 4)], pickWidth(w), pickWidth(w), pickWidth(w));
        } else if (kind < 6) {
            const Ref a = pick();
            const int w = b.width(a);
            switch (randInt(rng, 4)) {
            case 0:
                n = b.dot(a, b.convert(pick(), w));
                break;
            case 1:
                n = b.length(a);
                break;
            case 2:
                n = b.distance(a, b.convert(pick(), w));
                break;
            default:
                n = b.normalize(a);
                break;
            }
        } else if (kind < 7) {
            n = b.cross(b.convert(pick(), 3), b.convert(pick(), 3));
        } else if (kind < 8) {
            const Ref a = pick();
            const int w = b.width(a);
            n = b.swizzle(a, int(randInt(rng, uint32_t(w))), int(randInt(rng, uint32_t(w))),
                          rng.uniform() < 0.5 ? int(randInt(rng, uint32_t(w))) : -1);
        } else if (kind < 9) {
            const Ref a = pick();
            const int wa = b.width(a);
            n = wa < 4 ? b.concat(a, b.convert(pick(), 1 + int(randInt(rng, uint32_t(4 - wa))))) : b.normalize(a);
        } else { // procedurals (M3.7)
            const Ref p = b.convert(pick(), 2 + int(randInt(rng, 2)));
            const int w = 1 + int(randInt(rng, 4));
            switch (randInt(rng, 6)) {
            case 0:
                n = b.noise(p, w);
                break;
            case 1:
                n = b.fractal(p, b.convert(pick(), 1), b.convert(pick(), 1), b.convert(pick(), 1), w);
                break;
            case 2:
                n = b.cellNoise(p, w == 1 ? 1 : 3);
                break;
            case 3:
                n = b.worley(p, b.convert(pick(), 1), int(randInt(rng, 2)), w == 4 ? 3 : w);
                break;
            case 4:
                n = b.rgbToHsv(pick());
                break;
            default:
                n = b.hsvToRgb(pick());
                break;
            }
        }
        nodes.push_back(n);
    }
    // A few outputs at synthetic offsets (4 floats apart).
    for (uint16_t o = 0; o < 4; ++o) {
        const Ref n = nodes[nodes.size() - 1 - randInt(rng, std::min<uint32_t>(10, uint32_t(nodes.size())))];
        b.output(uint16_t(o * 4), b.width(n), n);
    }
    return r;
}

struct Lane {
    float globals[int(Global::Count)][3];
    float primvars[4][4];
    bool hasPrimvar[4];
};

bool sameBits(float a, float b) {
    if (std::isnan(a) && std::isnan(b))
        return true;
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// Runs `program` for `lanes` with an N-lane executor (chunks of N, padded).
template <int N>
std::vector<float> runProgram(const Program &p, const std::vector<float> &U, const std::vector<Lane> &lanes) {
    std::vector<float> out(lanes.size() * 16, 0.0f);
    std::vector<float> R(size_t(std::max<uint32_t>(p.rowCount, 1)) * N);
    for (size_t base = 0; base < lanes.size(); base += N) {
        for (int l = 0; l < N; ++l) {
            const Lane &lane = lanes[std::min(base + size_t(l), lanes.size() - 1)];
            for (const GlobalBinding &g : p.globals)
                for (int k = 0; k < globalWidth(g.global); ++k)
                    R[(g.row + size_t(k)) * N + size_t(l)] = lane.globals[int(g.global)][k];
            for (const PrimvarBinding &b : p.primvarBindings) {
                const int idx = p.primvars[b.name][2] - '0';
                for (int k = 0; k < b.width; ++k)
                    R[(b.row + size_t(k)) * N + size_t(l)] =
                        lane.hasPrimvar[idx] ? lane.primvars[idx][k] : U[b.fallback + size_t(k)];
            }
        }
        execute<N>(p.code.data(), p.code.size(), R.data(), U.data());
        for (int l = 0; l < N && base + size_t(l) < lanes.size(); ++l)
            for (const OutputBinding &o : p.outputs)
                for (int k = 0; k < o.width; ++k)
                    out[(base + size_t(l)) * 16 + o.field + size_t(k)] =
                        o.uniform ? U[o.index + size_t(k)] : R[(o.index + size_t(k)) * N + size_t(l)];
    }
    return out;
}

} // namespace

TEST_CASE("Random programs agree bit for bit in every executor", "[shading][vm][differential]") {
    Pcg32 rng(2025);
    int mismatches = 0, compiled = 0;
    std::string first;
    for (int graph = 0; graph < 3000; ++graph) {
        const RandomGraph rg = randomGraph(rng);
        std::vector<float> params(rg.g.paramCount);
        for (float &x : params)
            x = randomValue(rng);
        std::vector<Lane> lanes(45);
        for (Lane &lane : lanes) {
            for (auto &g : lane.globals)
                for (float &x : g)
                    x = randomValue(rng);
            for (int i = 0; i < 4; ++i) {
                lane.hasPrimvar[i] = rng.uniform() < 0.7;
                for (float &x : lane.primvars[i])
                    x = randomValue(rng);
            }
        }
        // Reference: the tree walker over the unoptimized graph.
        std::vector<float> reference(lanes.size() * 16, 0.0f), values;
        for (size_t l = 0; l < lanes.size(); ++l) {
            const float *globals[int(Global::Count)];
            for (int gi = 0; gi < int(Global::Count); ++gi)
                globals[gi] = lanes[l].globals[gi];
            const float *primvars[2] = {lanes[l].hasPrimvar[0] ? lanes[l].primvars[0] : nullptr,
                                        lanes[l].hasPrimvar[1] ? lanes[l].primvars[1] : nullptr};
            evaluateGraph(rg.g, params.data(), globals, primvars, values);
            for (const IrOutput &o : rg.g.outputs)
                for (int k = 0; k < o.width; ++k)
                    reference[l * 16 + o.field + size_t(k)] = values[size_t(o.value) * 4 + size_t(k)];
        }
        for (const bool optimize : {false, true}) {
            CompileOptions options;
            options.optimize = optimize;
            const CompileResult r = compile(rg.g, options);
            if (!r.program) {
                FAIL("graph " << graph << ": " << r.error);
                continue;
            }
            ++compiled;
            std::vector<float> U;
            r.program->initUniforms(params.data(), U);
            const std::vector<float> results[4] = {runProgram<1>(*r.program, U, lanes), runProgram<8>(*r.program, U, lanes),
                                                   runProgram<16>(*r.program, U, lanes),
                                                   runProgram<32>(*r.program, U, lanes)};
            for (const std::vector<float> &res : results)
                for (size_t i = 0; i < res.size(); ++i)
                    if (!sameBits(res[i], reference[i])) {
                        if (mismatches++ == 0)
                            first = "graph " + std::to_string(graph) + (optimize ? " (optimized)" : "") + " value " +
                                    std::to_string(i) + ": " + std::to_string(res[i]) + " vs " +
                                    std::to_string(reference[i]);
                    }
        }
    }
    INFO(first);
    CHECK(compiled == 6000);
    CHECK(mismatches == 0);
}
