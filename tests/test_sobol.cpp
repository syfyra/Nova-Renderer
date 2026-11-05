#include "core/math/rng.h"
#include "core/math/sobol.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace nova;
using Catch::Matchers::WithinAbs;

// Natural (not Gray-code) index order: point i is the XOR of the direction
// numbers of the set bits of i.
TEST_CASE("Sobol dimensions 0 and 1 match the reference sequence", "[sobol]") {
    const float d0[8] = {0.0f, 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f};
    const float d1[8] = {0.0f, 0.5f, 0.75f, 0.25f, 0.625f, 0.125f, 0.375f, 0.875f};
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(unitFloat(sobolDim0(i)) == d0[i]);
        CHECK(unitFloat(sobolDim1(i)) == d1[i]);
    }
}

// Generator matrix columns (Joe-Kuo / pbrt-v4 SobolMatrices32) and golden
// values of the scramble, so an accidental change of the sequence is caught.
TEST_CASE("Sobol generator columns and golden values", "[sobol]") {
    const uint32_t dim1[8] = {0x80000000u, 0xc0000000u, 0xa0000000u, 0xf0000000u,
                              0x88000000u, 0xcc000000u, 0xaa000000u, 0xff000000u};
    const uint32_t dim2[8] = {0x80000000u, 0xc0000000u, 0x60000000u, 0x90000000u,
                              0xe8000000u, 0x5c000000u, 0x8e000000u, 0xc5000000u};
    for (uint32_t b = 0; b < 8; ++b) {
        CHECK(sobolDim1(1u << b) == dim1[b]);
        CHECK(sobolDim2(1u << b) == dim2[b]);
    }
    CHECK(sobolDim1(1u << 31) == 0xffffffffu);
    CHECK(owenScramble(0x12345678u, 0x9abcdef0u) == 0x1f8b4945u);
    CHECK(owenScramble(0xffffffffu, 1u) == 0x76d5b94cu);
    CHECK(sobolDim1(0xdeadbeefu) == 0x0c66ae4du);
}

// The fused get2D must stay bit-identical to the plain formulation.
TEST_CASE("Fast get2D equals the reference formulation", "[sobol]") {
    Rng rng = Rng::make(5, 0);
    bool identical = true, dimReversed = true;
    for (int k = 0; k < 200000; ++k) {
        const uint32_t seed = uint32_t(rng.next() * 4294967296.0);
        const uint32_t index = uint32_t(rng.next() * 4294967296.0);
        const uint32_t streamId = uint32_t(rng.next() * 1000.0f);
        dimReversed &= sobolDim1Reversed(index) == reverseBits(sobolDim1(index));
        const Sampler s{seed, index};
        const uint32_t h = hashCombine(seed, streamId);
        const uint32_t i = owenScramble(index, h);
        const Vec2f ref{unitFloat(owenScramble(sobolDim0(i), mix32(h ^ 0x5bd1e995u))),
                        unitFloat(owenScramble(sobolDim1(i), mix32(h ^ 0x1b873593u)))};
        const Vec2f got = s.get2D(streamId);
        const Vec3f got3 = s.get3D(streamId);
        identical &= got.x == ref.x && got.y == ref.y && got3.x == ref.x && got3.y == ref.y;
        identical &= s.get1D(streamId) == ref.x;
    }
    CHECK(dimReversed);
    CHECK(identical);
}

namespace {

// Every elementary interval of area 1/N holds exactly one of N points.
bool isNet02(const std::vector<Vec2f> &pts, int k) {
    for (int a = 0; a <= k; ++a) {
        const int nx = 1 << a, ny = 1 << (k - a);
        std::vector<int> count(size_t(nx) * size_t(ny), 0);
        for (const Vec2f &p : pts)
            ++count[size_t(int(p.y * float(ny))) * size_t(nx) + size_t(int(p.x * float(nx)))];
        for (int c : count)
            if (c != 1)
                return false;
    }
    return true;
}

} // namespace

// Owen scrambling and the index shuffle keep the net property, for the
// progressive prefix as well as for later aligned blocks of samples.
TEST_CASE("Scrambled 2D samples form (0,2)-nets", "[sobol]") {
    constexpr int k = 8, n = 1 << k;
    for (const int32_t pixel : {0, 1, 12345}) {
        for (const uint32_t streamId : {stream::kPixelFilter, stream::bounce(3, stream::kBsdf)}) {
            for (const uint32_t block : {0u, 1u, 5u}) {
                std::vector<Vec2f> pts;
                for (uint32_t i = 0; i < n; ++i)
                    pts.push_back(Sampler::make(pixel, 7, block * n + i).get2D(streamId));
                CHECK(isNet02(pts, k));
            }
        }
    }
}

// The first 2^k points of a 3D stream form a (1,k,3)-net: every elementary
// box of volume 2^(1-k) holds exactly two points.
TEST_CASE("Scrambled 3D samples form (1,k,3)-nets", "[sobol]") {
    for (int k = 3; k <= 10; ++k) {
        const int n = 1 << k;
        std::vector<Vec3f> pts;
        for (uint32_t i = 0; i < uint32_t(n); ++i)
            pts.push_back(Sampler::make(42, -3, i).get3D(stream::bounce(1, stream::kLight)));
        bool net = true;
        for (int a = 0; a <= k - 1; ++a) {
            for (int b = 0; a + b <= k - 1; ++b) {
                const int c = k - 1 - a - b;
                const int na = 1 << a, nb = 1 << b, nc = 1 << c;
                std::vector<int> count(size_t(na) * size_t(nb) * size_t(nc), 0);
                for (const Vec3f &p : pts)
                    ++count[(size_t(int(p.z * float(nc))) * size_t(nb) + size_t(int(p.y * float(nb)))) * size_t(na) +
                            size_t(int(p.x * float(na)))];
                for (int cnt : count)
                    net &= cnt == 2;
            }
        }
        CHECK(net);
    }
}

// The shuffle maps every aligned block of 2^k indices onto an aligned block.
TEST_CASE("The index shuffle preserves aligned blocks", "[sobol]") {
    for (const uint32_t seed : {0x1234u, 0xdeadbeefu, 77u}) {
        const uint32_t s = mix32(seed);
        for (int k = 1; k <= 12; ++k) {
            for (const uint32_t j : {0u, 1u, 7u}) {
                const uint32_t n = 1u << k;
                std::vector<int> seen(n, 0);
                const uint32_t high = owenScramble(j << k, s) >> k;
                bool same = true;
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t v = owenScramble((j << k) | i, s);
                    same &= (v >> k) == high;
                    ++seen[v & (n - 1)];
                }
                bool permutation = true;
                for (int c : seen)
                    permutation &= c == 1;
                CHECK(same);
                CHECK(permutation);
            }
        }
    }
}

TEST_CASE("Sampler streams are decorrelated and uniform", "[sobol]") {
    constexpr int n = 4096;
    double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const Sampler s = Sampler::make(77, 3, i);
        const double a = s.get1D(stream::bounce(0, stream::kRoulette));
        const double b = s.get1D(stream::bounce(1, stream::kRoulette));
        REQUIRE(a >= 0.0);
        REQUIRE(a < 1.0);
        sa += a, sb += b, saa += a * a, sbb += b * b, sab += a * b;
    }
    const double ma = sa / n, mb = sb / n;
    const double corr = (sab / n - ma * mb) / std::sqrt((saa / n - ma * ma) * (sbb / n - mb * mb));
    CHECK_THAT(ma, WithinAbs(0.5, 1e-3));
    CHECK_THAT(corr, WithinAbs(0.0, 0.05));
    // Different pixels see different sequences, including pixels whose linear
    // index would collide in an overscan window.
    CHECK(Sampler::make(1, 0, 0).get1D(0) != Sampler::make(2, 0, 0).get1D(0));
    CHECK(Sampler::make(-1, 1, 0).pixelSeed != Sampler::make(639, 0, 0).pixelSeed);
}

// Quasi-Monte Carlo integration of a smooth function converges much faster
// than the 1/sqrt(N) of independent random samples.
TEST_CASE("Sobol integration converges quickly", "[sobol]") {
    for (const int32_t pixel : {3, 99}) {
        constexpr int n = 1024;
        double sum = 0.0;
        for (uint32_t i = 0; i < n; ++i) {
            const Vec2f u = Sampler::make(pixel, 0, i).get2D(stream::kLens);
            sum += std::sin(kPi * u.x) * u.y * u.y; // integral = (2/pi) * (1/3)
        }
        CHECK_THAT(sum / n, WithinAbs(2.0 / (3.0 * kPi), 2e-4));
    }
}
