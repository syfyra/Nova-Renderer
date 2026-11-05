// M6.1: the light path expression compiler (docs/design/M6.md D2, D3):
// parse errors, Karma's expressions, the automaton against an independent
// backtracking matcher on random paths, shorthands and the A/B rule, and the
// collapse of names no expression uses.

#include "core/film/lpe_compile.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace nova::lpe;

namespace {

Event ev(Type t, Scatter s = ScatterNone, std::string tag = "", std::string label = "") {
    return Event{t, s, std::move(tag), std::move(label)};
}

std::set<uint16_t> outputsOf(Compiled const &c, std::vector<Event> const &path) {
    const Program p = c.program();
    const State s = c.run(path);
    return std::set<uint16_t>(p.outputs(s), p.outputs(s) + p.outputCount(s));
}

// Karma's authored strings (docs/research/m6/karma-aovs.md 1.3, 1.5, 1.6) and more.
const std::vector<std::string> kKarma = {
    "C.*[LO]",          "unoccluded;C.*[LO]", "shadow;C.*[LO]",    "C<RD>.*L",          "C<RD>L",
    "C<RD>.+L",         "unoccluded;C<RD>L",  "C<RG>.*L",          "C<RG>L",            "C<RG>.+L",
    "C<TG>.*L",         "C<...'coat'>.*L",    "C<TD>.*L",          "CL",                "C.*O",
    "CO",               "C.+O",               "CV.*L",             "CVL",               "CV.+L",
    "CDA",              "C<RG>A",             "C<TG>A",            "C<TG>*DA",          "!C<TG>*B",
    "holdout;C.*[LO]",  "C.*<L.'key'>",       "C.*<L.'fill'>",     "C<RD>.*<L.'key'>",  "C<RG'metal'>.*[LO]",
};

} // namespace

TEST_CASE("LPE syntax errors are reported per expression", "[lpe]") {
    const std::vector<std::string> bad = {"C<RD",  "C(D",   "C{2,1}L", "C<'x'>L", "C<RD'a''b''c'>L",
                                          "C[]L",  "C<RD>Q", "C'unterminated", "C<Q>L", "C)"};
    std::vector<std::string> all = bad;
    all.push_back("C<RD>L");
    const Compiled c = compile(all);
    REQUIRE(c.ok());
    for (size_t i = 0; i < bad.size(); ++i) {
        INFO(bad[i]);
        CHECK_FALSE(c.errors[i].empty());
    }
    CHECK(c.errors.back().empty());
    CHECK(outputsOf(c, {ev(TypeCamera), ev(TypeReflect, ScatterDiffuse), ev(TypeLight)}) ==
          std::set<uint16_t>{uint16_t(bad.size())});
    CHECK_FALSE(compile({"C<RD"}).ok()); // nothing compiled
}

TEST_CASE("Karma's expressions compile into one small automaton", "[lpe]") {
    const Compiled c = compile(kKarma);
    REQUIRE(c.ok());
    for (size_t i = 0; i < kKarma.size(); ++i) {
        INFO(kKarma[i]);
        CHECK(c.errors[i].empty());
    }
    CHECK(c.stateCount < 200);
    CHECK(c.classCount < 64);
    CHECK(c.outputFlags[1] == OutputUnoccluded);
    CHECK(c.outputFlags[2] == OutputShadow);
    CHECK(c.outputFlags[24] == OutputInvert);
    CHECK(c.outputFlags[25] == OutputHoldout);
    // A diffuse bounce off a coat-labelled lobe of a 'metal' object to the key light.
    const std::set<uint16_t> got = outputsOf(c, {ev(TypeCamera), ev(TypeReflect, ScatterDiffuse, "metal", "coat"),
                                                 ev(TypeLight, ScatterNone, "key")});
    CHECK(got.count(0));  // beauty
    CHECK(got.count(3));  // C<RD>.*L
    CHECK(got.count(4));  // C<RD>L
    CHECK_FALSE(got.count(5)); // C<RD>.+L
    CHECK(got.count(11)); // coat
    CHECK(got.count(26)); // key light
    CHECK_FALSE(got.count(27)); // fill light
    CHECK(got.count(28)); // C<RD>.*<L.'key'>
    CHECK_FALSE(got.count(29)); // glossy only
}

TEST_CASE("The automaton agrees with a backtracking matcher on random paths", "[lpe]") {
    std::vector<std::string> exprs = kKarma;
    for (const char *e : {"C[^D]*L", "C(D|G){2}L", "CD?G+L", "C.*", "C<[RT][DG]>{1,3}L", "C'metal'.*L",
                          "C<.[^G]>.*[LO]", "C<R.'metal''coat'>L", "C(<RD>|<TG>)+<L.[^'key']>", "C.{2,}O",
                          "CB", "C<TG>*B", "C[<RD><RG>]L", "C<[^R]D>L"})
        exprs.push_back(e);
    const Compiled c = compile(exprs);
    REQUIRE(c.ok());
    for (size_t i = 0; i < exprs.size(); ++i) {
        INFO(exprs[i]);
        REQUIRE(c.errors[i].empty());
    }

    std::mt19937 rng(7);
    const std::vector<std::string> tags = {"", "metal", "key", "fill", "unknown"};
    const std::vector<std::string> labels = {"", "coat", "sss", "other"};
    auto pick = [&](auto const &v) { return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)]; };
    for (int trial = 0; trial < 4000; ++trial) {
        std::vector<Event> path{ev(TypeCamera)};
        const int bounces = std::uniform_int_distribution<int>(0, 5)(rng);
        for (int b = 0; b < bounces; ++b) {
            const int kind = std::uniform_int_distribution<int>(0, 4)(rng);
            const Type t = kind < 2 ? TypeReflect : kind < 4 ? TypeTransmit : TypeVolume;
            const Scatter s = std::uniform_int_distribution<int>(0, 1)(rng) ? ScatterDiffuse : ScatterGlossy;
            path.push_back(ev(t, s, pick(tags), pick(labels)));
        }
        const int end = std::uniform_int_distribution<int>(0, 4)(rng);
        if (end == 0)
            path.push_back(ev(TypeLight, ScatterNone, pick(tags)));
        else if (end == 1)
            path.push_back(ev(TypeObject, ScatterNone, pick(tags)));
        else if (end == 2)
            path.push_back(ev(TypeBackground));
        else if (end == 3)
            path.push_back(ev(TypeAlbedo));
        const std::set<uint16_t> got = outputsOf(c, path);
        for (size_t e = 0; e < exprs.size(); ++e) {
            const bool expected = matches(exprs[e], path);
            if (expected != (got.count(uint16_t(e)) != 0)) {
                std::string text;
                for (Event const &x : path)
                    text += std::string(1, "CRTVLOBA"[x.type]) + std::string(1, "DGSsx"[x.scatter]) + "'" + x.tag +
                            "''" + x.label + "' ";
                INFO(exprs[e] << " on " << text);
                CHECK(expected == (got.count(uint16_t(e)) != 0));
            }
        }
    }
}

TEST_CASE("LPE shorthands and the A and B events", "[lpe]") {
    const std::vector<Event> diffuse{ev(TypeCamera), ev(TypeReflect, ScatterDiffuse), ev(TypeLight)};
    const std::vector<Event> albedo{ev(TypeCamera), ev(TypeReflect, ScatterDiffuse), ev(TypeAlbedo)};
    const std::vector<Event> escape{ev(TypeCamera), ev(TypeTransmit, ScatterGlossy), ev(TypeBackground)};
    CHECK(matches("CDL", diffuse));
    CHECK(matches("CRL", diffuse));
    CHECK(matches("C.*", diffuse));
    CHECK_FALSE(matches("C.*", albedo)); // '.' never matches A or B (D4)
    CHECK_FALSE(matches("C.*", escape));
    CHECK(matches("CDA", albedo));
    CHECK(matches("C<TG>*B", escape));
    CHECK(matches("C'metal'L", {ev(TypeCamera), ev(TypeReflect, ScatterGlossy, "metal"), ev(TypeLight)}));
    CHECK_FALSE(matches("C'metal'L", {ev(TypeCamera), ev(TypeReflect, ScatterGlossy, "", "metal"), ev(TypeLight)}));
    CHECK(matches("unoccluded;C<RD>L", diffuse)); // prefixes do not change matching
    CHECK(matches("!C<RD>L", diffuse));
}

TEST_CASE("Names no expression uses collapse into one event class", "[lpe]") {
    const Compiled beauty = compile({"C.*[LO]"});
    REQUIRE(beauty.ok());
    CHECK(beauty.tags.size() == 1);
    CHECK(beauty.labels.size() == 1);
    CHECK(beauty.stateCount <= 4); // dead, before C, the .* loop, after [LO]
    CHECK(beauty.classCount <= 4); // C, scattering, L/O, A/B
    // A light group: every scattering event is one class.
    const Compiled group = compile({"C.*<L.'key'>", "C.*[LO]"});
    REQUIRE(group.ok());
    const Program p = group.program();
    const uint16_t rd = p.symbolClass[p.symbol(TypeReflect, ScatterDiffuse, 0, 0)];
    CHECK(p.symbolClass[p.symbol(TypeReflect, ScatterGlossy, 0, 0)] == rd);
    CHECK(p.symbolClass[p.symbol(TypeTransmit, ScatterGlossy, 0, 0)] == rd);
    CHECK(p.symbolClass[p.symbol(TypeLight, ScatterNone, group.tagId("key"), 0)] !=
          p.symbolClass[p.symbol(TypeLight, ScatterNone, 0, 0)]);
}
