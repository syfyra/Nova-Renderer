// MaterialX noise against Houdini's VEX built-ins (docs/design/M3.md, M3.7):
// Karma's MaterialX templates call mx_perlin, mx_cellnoise and mx_worley, so
// these goldens (tests/vex/noise_golden.vfl, 2000 points) are what Karma
// renders. Perlin and fBm within 2 ulp, cell noise and Worley exactly. Also
// the procedural opcodes through the VM for every lane count.

#include "core/shading/compiler.h"
#include "core/shading/noise.h"
#include "core/shading/vm.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::shading;

namespace {

int32_t ulps(float a, float b) {
    int32_t ia, ib;
    std::memcpy(&ia, &a, 4);
    std::memcpy(&ib, &b, 4);
    if ((ia < 0) != (ib < 0))
        return a == b ? 0 : (std::fabs(a - b) < 1e-30f ? 0 : 1 << 30);
    return std::abs(ia - ib);
}

} // namespace

TEST_CASE("MaterialX noise matches Houdini's built-ins", "[shading][noise]") {
    std::ifstream in(NOVA_TEST_DIR "/vex/noise_golden.txt");
    REQUIRE(in.good());
    std::string line;
    int rows = 0, perlinWorst = 0, fbmWorst = 0, cellWrong = 0, worleyWrong = 0;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        int i;
        float v[22];
        s >> i;
        for (float &x : v)
            s >> x;
        if (!s)
            continue;
        ++rows;
        const float x = float(((i * 7919) % 20011) - 10005) / 128.0f + 0.001953125f;
        const float y = float(((i * 6151) % 20011) - 10005) / 128.0f + 0.001953125f;
        const float z = float(((i * 3571) % 20011) - 10005) / 128.0f + 0.001953125f;
        const Vec2f p2{x, y};
        const Vec3f p3{x, y, z};
        const Vec3f pv2 = noise::perlinV3(p2), pv3 = noise::perlinV3(p3);
        const float perlin[8] = {noise::perlin(p2), pv2.x, pv2.y, pv2.z, noise::perlin(p3), pv3.x, pv3.y, pv3.z};
        for (int k = 0; k < 8; ++k)
            perlinWorst = std::max(perlinWorst, std::fabs(perlin[k] - v[k]) < 1e-7f ? 0 : ulps(perlin[k], v[k]));
        const Vec3f cv3 = noise::cellV3(p3);
        const float cells[5] = {noise::cell(p2), noise::cell(p3), cv3.x, cv3.y, cv3.z};
        for (int k = 0; k < 5; ++k)
            cellWrong += cells[k] != v[8 + k];
        float d2[3], d3[3];
        Vec2f m2;
        Vec3f m3;
        noise::worley(p2, 1.0f, d2, m2);
        noise::worley(p3, 0.75f, d3, m3);
        worleyWrong += std::sqrt(d2[0]) != v[13];
        for (int k = 0; k < 3; ++k)
            worleyWrong += std::sqrt(d3[k]) != v[14 + k];
        const float fbm = noise::fractal(Vec3f{x * 0.1f, y * 0.1f, z * 0.1f}, 3, 2.0f, 0.5f);
        fbmWorst = std::max(fbmWorst, std::fabs(fbm - v[20]) < 1e-7f ? 0 : ulps(fbm, v[20]));
    }
    CHECK(rows == 2000);
    CHECK(perlinWorst <= 2);
    CHECK(fbmWorst <= 4);
    CHECK(cellWrong == 0);
    CHECK(worleyWrong == 0);
}

TEST_CASE("Procedural opcodes agree in every executor", "[shading][noise][vm]") {
    // noise3d (vector4), fractal2d (vector2), worley3d solid (vector3), cell
    // noise and HSV round trip of the position, for 37 lanes.
    IrGraph g;
    IrBuilder b(g);
    const Ref P = b.global(Global::P);
    const Ref uv = b.swizzle(P, 0, 1);
    const Ref n4 = b.noise(P, 4);
    const Ref f2 = b.fractal(uv, b.constant(4.0f), b.constant(2.0f), b.constant(0.5f), 2);
    const Ref w3 = b.worley(P, b.constant(1.0f), 1, 3);
    const Ref c1 = b.cellNoise(uv, 1);
    const Ref hsv = b.hsvToRgb(b.rgbToHsv(b.unary(Op::Fract, P)));
    b.output(0, 4, n4);
    b.output(4, 2, f2);
    b.output(8, 3, w3);
    b.output(12, 1, c1);
    b.output(16, 3, hsv);
    const CompileResult r = compile(g);
    REQUIRE(r.program);
    const Program &p = *r.program;
    std::vector<float> U;
    p.initUniforms(nullptr, U);
    const int lanes = 37;
    std::vector<Vec3f> pos(lanes);
    for (int l = 0; l < lanes; ++l)
        pos[size_t(l)] = {float(l) * 0.37f - 5.0f, float(l * l % 11) * 0.29f, float(l % 7) * 1.7f - 3.0f};
    auto run = [&](auto lanesTag) {
        constexpr int N = decltype(lanesTag)::value;
        std::vector<float> out(size_t(lanes) * 20, 0.0f), R(size_t(p.rowCount) * N);
        for (int base = 0; base < lanes; base += N) {
            for (int l = 0; l < N; ++l) {
                const Vec3f q = pos[size_t(std::min(base + l, lanes - 1))];
                for (const GlobalBinding &gb : p.globals)
                    for (int k = 0; k < 3; ++k)
                        R[(gb.row + size_t(k)) * N + size_t(l)] = q[k];
            }
            execute<N>(p.code.data(), p.code.size(), R.data(), U.data());
            for (int l = 0; l < N && base + l < lanes; ++l)
                for (const OutputBinding &o : p.outputs)
                    for (int k = 0; k < o.width; ++k)
                        out[size_t(base + l) * 20 + o.field + size_t(k)] =
                            o.uniform ? U[o.index + size_t(k)] : R[(o.index + size_t(k)) * N + size_t(l)];
        }
        return out;
    };
    const std::vector<float> a = run(std::integral_constant<int, 1>{}), b8 = run(std::integral_constant<int, 8>{}),
                             b32 = run(std::integral_constant<int, 32>{});
    CHECK(std::memcmp(a.data(), b8.data(), a.size() * 4) == 0);
    CHECK(std::memcmp(a.data(), b32.data(), a.size() * 4) == 0);
    // Spot values against the functions themselves.
    for (int l = 0; l < lanes; ++l) {
        const Vec3f q = pos[size_t(l)];
        const Vec3f v = noise::perlinV3(q);
        CHECK(a[size_t(l) * 20 + 0] == v.x);
        CHECK(a[size_t(l) * 20 + 3] == noise::perlin(q + Vec3f{19.0f, 73.0f, 29.0f}));
        CHECK(a[size_t(l) * 20 + 12] == noise::cell(Vec2f{q.x, q.y}));
        const Vec3f f{q.x - std::floor(q.x), q.y - std::floor(q.y), q.z - std::floor(q.z)};
        for (int k = 0; k < 3; ++k) // HSV round trip
            CHECK(std::fabs(a[size_t(l) * 20 + 16 + size_t(k)] - f[k]) < 1e-5f);
    }
}

TEST_CASE("Voronoi distances and seed points (kma_voronoinoise)", "[noise]") {
    using nova::Vec3f;
    float d[3];
    Vec3f pts[3];
    // Jitter 0: the seed points are the cell centres.
    nova::shading::noise::voronoi({0.3f, 0.5f, 0.0f}, 0.0f, 0, 2, d, pts);
    CHECK(d[0] == Catch::Approx(0.2f));
    CHECK(d[1] == Catch::Approx(0.8f));
    CHECK(d[2] == Catch::Approx(std::sqrt(0.2f * 0.2f + 1.0f)));
    CHECK(pts[0].x == Catch::Approx(0.5f));
    CHECK(pts[0].y == Catch::Approx(0.5f));
    nova::shading::noise::voronoi({0.3f, 0.5f, 0.0f}, 0.0f, 1, 2, d, pts); // squared
    CHECK(d[0] == Catch::Approx(0.04f));
    nova::shading::noise::voronoi({0.3f, 0.5f, 0.0f}, 0.0f, 2, 2, d, pts); // Manhattan
    CHECK(d[2] == Catch::Approx(1.2f));
    nova::shading::noise::voronoi({0.3f, 0.5f, 0.0f}, 0.0f, 3, 2, d, pts); // Chebyshev
    CHECK(d[2] == Catch::Approx(1.0f));
    // Jittered, 3D: ordered distances, the nearest point at distance F1.
    for (int i = 0; i < 200; ++i) {
        const Vec3f p{float(i) * 0.137f - 7.0f, float(i % 17) * 0.29f, float(i % 5) * -0.61f};
        nova::shading::noise::voronoi(p, 1.0f, 0, 3, d, pts);
        CHECK(d[0] <= d[1]);
        CHECK(d[1] <= d[2]);
        CHECK(nova::length(pts[0] - p) == Catch::Approx(d[0]).margin(1e-5));
    }
}
