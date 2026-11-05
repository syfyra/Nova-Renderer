// Subdivision surfaces against analytic limit values (Catmull-Clark and Loop
// limit masks), watertightness and orientation.

#include "core/geometry/subdiv.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <utility>

using namespace nova;
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

SubdivMeshDesc creaseAll(SubdivMeshDesc c, float sharpness) {
    for (size_t f = 0, o = 0; f < c.faceVertexCounts.size(); o += size_t(c.faceVertexCounts[f]), ++f)
        for (int k = 0; k < c.faceVertexCounts[f]; ++k) {
            const int a = c.faceVertexIndices[o + size_t(k)];
            const int b = c.faceVertexIndices[o + size_t((k + 1) % c.faceVertexCounts[f])];
            if (a < b) {
                c.creaseEdges.insert(c.creaseEdges.end(), {a, b});
                c.creaseSharpness.push_back(sharpness);
            }
        }
    return c;
}

bool hasPoint(const MeshDesc &m, Vec3f p, float eps = 2e-6f) {
    for (size_t i = 0; i < m.pointCount(); ++i)
        if (maxComponent(absolute(p - m.points[i])) <= eps)
            return true;
    return false;
}

// Edges used by other than two triangles, by position (copies at sharp
// features count as the same vertex).
int openEdges(const MeshDesc &m) {
    using Key = std::tuple<long long, long long, long long>;
    auto key = [](Vec3f p) { return Key{std::llround(p.x * 1e5), std::llround(p.y * 1e5), std::llround(p.z * 1e5)}; };
    std::map<std::pair<Key, Key>, int> count;
    for (size_t t = 0; t < m.indices.size() / 3; ++t)
        for (int k = 0; k < 3; ++k) {
            Key a = key(m.points[m.indices[t * 3 + size_t(k)]]), b = key(m.points[m.indices[t * 3 + size_t((k + 1) % 3)]]);
            if (b < a)
                std::swap(a, b);
            ++count[{a, b}];
        }
    int open = 0;
    for (const auto &kv : count)
        open += kv.second != 2;
    return open;
}

int inwardNormals(const MeshDesc &m) { // for shapes around the origin
    int n = 0;
    for (size_t i = 0; i < m.pointCount(); ++i)
        n += dot(m.normals[i], m.points[i]) <= 0.0f;
    return n;
}

int flippedTriangles(const MeshDesc &m) {
    int n = 0;
    for (size_t t = 0; t < m.indices.size() / 3; ++t) {
        const uint32_t *i = &m.indices[t * 3];
        const Vec3f ng = cross(m.points[i[1]] - m.points[i[0]], m.points[i[2]] - m.points[i[0]]);
        n += dot(ng, m.normals[i[0]] + m.normals[i[1]] + m.normals[i[2]]) <= 0.0f;
    }
    return n;
}

constexpr float kCorner = 0.25f, kFace = 34.0f / 81.0f, kEdge = 395.0f / 1296.0f;

} // namespace

// Catmull-Clark limit masks for the unit cube: corner (n = 3) 1/4, face
// center 34/81, edge midpoint 395/1296.
TEST_CASE("Catmull-Clark cube reaches the analytic limit surface", "[subdiv]") {
    for (const int rate : {2, 4, 8, 16}) {
        INFO("rate " << rate);
        MeshDesc m;
        SubdivStats st;
        std::string err;
        REQUIRE(tessellateSubdiv(unitCube(), rate, m, &err, &st));
        CHECK(st.triangles == 12u * uint32_t(rate * rate));
        CHECK(subdivTriangleEstimate(unitCube(), rate) == st.triangles);
        CHECK(hasPoint(m, {kCorner, kCorner, kCorner}));
        CHECK(hasPoint(m, {-kCorner, kCorner, -kCorner}));
        CHECK(hasPoint(m, {0.0f, 0.0f, kFace}));
        CHECK(hasPoint(m, {-kFace, 0.0f, 0.0f}));
        CHECK(hasPoint(m, {kEdge, 0.0f, -kEdge}));
        CHECK(openEdges(m) == 0);
        CHECK(inwardNormals(m) == 0);
        CHECK(flippedTriangles(m) == 0);
    }
}

TEST_CASE("Subdivision normals are the limit normals", "[subdiv]") {
    MeshDesc m;
    REQUIRE(tessellateSubdiv(unitCube(), 8, m));
    const float s3 = 1.0f / std::sqrt(3.0f), s2 = 1.0f / std::sqrt(2.0f);
    int found = 0;
    for (size_t i = 0; i < m.points.size(); ++i) {
        const Vec3f p = m.points[i], n = m.normals[i];
        if (maxComponent(absolute(p - Vec3f{kCorner, kCorner, kCorner})) < 1e-6f) {
            CHECK(maxComponent(absolute(n - Vec3f{s3, s3, s3})) < 1e-5f);
            ++found;
        }
        if (maxComponent(absolute(p - Vec3f{0.0f, 0.0f, kFace})) < 1e-6f) {
            CHECK(maxComponent(absolute(n - Vec3f{0.0f, 0.0f, 1.0f})) < 1e-5f);
            ++found;
        }
        if (maxComponent(absolute(p - Vec3f{kEdge, 0.0f, -kEdge})) < 1e-6f) {
            CHECK(maxComponent(absolute(n - Vec3f{s2, 0.0f, -s2})) < 1e-5f);
            ++found;
        }
    }
    CHECK(found == 3);
}

// Infinitely sharp edges keep the cube; the faces on either side of a crease
// get their own copies of the shared points, with their own normals.
TEST_CASE("Infinitely sharp creases reproduce the cube with split normals", "[subdiv]") {
    for (const int rate : {2, 4, 8}) {
        INFO("rate " << rate);
        MeshDesc m;
        SubdivStats st;
        REQUIRE(tessellateSubdiv(creaseAll(unitCube(), 10.0f), rate, m, nullptr, &st));
        float dist = 0.0f, normalDeviation = 0.0f;
        for (size_t i = 0; i < m.points.size(); ++i) {
            dist = std::fmax(dist, std::fabs(maxComponent(absolute(m.points[i])) - 0.5f));
            normalDeviation = std::fmax(normalDeviation, std::fabs(maxComponent(absolute(m.normals[i])) - 1.0f));
        }
        CHECK(dist < 1e-6f);
        CHECK(normalDeviation < 1e-5f);
        CHECK(st.splitVertices == 16u + 12u * uint32_t(rate - 1));
        CHECK(openEdges(m) == 0);
        CHECK(flippedTriangles(m) == 0);
    }
}

// Uniform creasing: the corner of a cube whose edges all have sharpness s
// sits at 0.5 - 0.25 * 2^-s for integer s, linear in between.
TEST_CASE("Semi-sharp creases follow uniform creasing", "[subdiv]") {
    for (const float s : {0.0f, 0.5f, 1.0f, 2.0f, 3.0f}) {
        MeshDesc m;
        REQUIRE(tessellateSubdiv(creaseAll(unitCube(), s), 16, m));
        float x = 0.0f;
        for (const Vec3f &p : m.points)
            x = std::fmax(x, std::fmin(p.x, std::fmin(p.y, p.z)));
        const float lo = 0.5f - 0.25f * std::exp2(-std::floor(s)), hi = 0.5f - 0.25f * std::exp2(-std::ceil(s));
        INFO("sharpness " << s);
        CHECK_THAT(x, WithinAbs(lo + (hi - lo) * (s - std::floor(s)), 2e-6));
    }
}

// Loop limit of a valence-4 vertex: (1 - n chi) v + chi sum(e) with
// chi = 31/220 gives 24/55 for the octahedron's vertices.
TEST_CASE("Loop octahedron reaches the analytic limit", "[subdiv]") {
    SubdivMeshDesc o;
    o.scheme = SubdivScheme::Loop;
    o.points = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    o.faceVertexCounts.assign(8, 3);
    o.faceVertexIndices = {0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 4, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3, 5};
    MeshDesc m;
    SubdivStats st;
    REQUIRE(tessellateSubdiv(o, 8, m, nullptr, &st));
    CHECK(hasPoint(m, {24.0f / 55.0f, 0.0f, 0.0f}));
    CHECK(hasPoint(m, {0.0f, 0.0f, -24.0f / 55.0f}));
    CHECK(st.triangles == 8u * 64u);
    CHECK(openEdges(m) == 0);
    CHECK(inwardNormals(m) == 0);
}

TEST_CASE("Boundary interpolation rules of an open quad", "[subdiv]") {
    SubdivMeshDesc p;
    p.points = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    p.faceVertexCounts = {4};
    p.faceVertexIndices = {0, 1, 2, 3};
    MeshDesc a, b, c;
    p.vtxBoundary = SubdivVtxBoundary::EdgeAndCorner; // pinned corner, straight edge
    REQUIRE(tessellateSubdiv(p, 8, a));
    CHECK(hasPoint(a, {1.0f, 1.0f, 0.0f}));
    CHECK(hasPoint(a, {1.0f, 0.0f, 0.0f}));
    p.vtxBoundary = SubdivVtxBoundary::EdgeOnly; // corner at (2/3, 2/3), edge midpoint at 11/12
    REQUIRE(tessellateSubdiv(p, 8, b));
    CHECK(hasPoint(b, {2.0f / 3.0f, 2.0f / 3.0f, 0.0f}));
    CHECK(hasPoint(b, {11.0f / 12.0f, 0.0f, 0.0f}));
    float nz = 0.0f; // degenerate corner derivatives fall back to the triangles
    for (const Vec3f &n : b.normals)
        nz = std::fmax(nz, std::fabs(n.z - 1.0f));
    CHECK(nz < 1e-5f);
    p.vtxBoundary = SubdivVtxBoundary::None; // every boundary face is a hole
    std::string err;
    CHECK_FALSE(tessellateSubdiv(p, 8, c, &err));
}

TEST_CASE("Holes, left-handed meshes and invalid input", "[subdiv]") {
    SubdivMeshDesc h = unitCube();
    h.holeIndices = {0, 42, -3}; // out-of-range entries are ignored
    MeshDesc m;
    SubdivStats st;
    REQUIRE(tessellateSubdiv(h, 8, m, nullptr, &st));
    CHECK(st.triangles == 5u * 128u);
    CHECK(hasPoint(m, {kCorner, kCorner, kCorner}));

    SubdivMeshDesc l = unitCube();
    for (size_t f = 0; f < 6; ++f)
        std::swap(l.faceVertexIndices[f * 4 + 1], l.faceVertexIndices[f * 4 + 3]);
    MeshDesc wrong, right;
    REQUIRE(tessellateSubdiv(l, 4, wrong));
    l.leftHanded = true;
    REQUIRE(tessellateSubdiv(l, 4, right));
    CHECK(inwardNormals(wrong) == int(wrong.points.size()));
    CHECK(inwardNormals(right) == 0);
    CHECK(flippedTriangles(right) == 0);

    SubdivMeshDesc bad = unitCube();
    bad.faceVertexIndices[3] = 99;
    std::string err;
    CHECK_FALSE(tessellateSubdiv(bad, 4, m, &err));
    SubdivMeshDesc loopQuads = unitCube();
    loopQuads.scheme = SubdivScheme::Loop;
    CHECK_FALSE(tessellateSubdiv(loopQuads, 4, m, &err));
}

namespace {
SubdivPrimvar positionPrimvar(const SubdivMeshDesc &c, PrimvarInterp interp) {
    SubdivPrimvar p;
    p.name = "rest";
    p.interp = interp;
    p.width = 3;
    for (const Vec3f &x : c.points) // value = position, so the limit value is the limit position
        p.values.insert(p.values.end(), {x.x, x.y, x.z});
    return p;
}
Vec3f valueAt(const MeshAttribute &a, size_t i) { return {a.data[i * 3], a.data[i * 3 + 1], a.data[i * 3 + 2]}; }
} // namespace

TEST_CASE("Primvars on subdivision surfaces", "[subdiv][attributes]") {
    SubdivMeshDesc c = unitCube();
    c.primvars.push_back(positionPrimvar(c, PrimvarInterp::Vertex));
    MeshDesc m;
    REQUIRE(tessellateSubdiv(c, 8, m));
    const MeshAttribute *a = m.findAttribute("rest");
    REQUIRE(a);
    CHECK(a->rate == AttributeRate::Vertex);
    float worst = 0.0f;
    for (size_t i = 0; i < m.pointCount(); ++i)
        worst = std::fmax(worst, maxComponent(absolute(valueAt(*a, i) - m.points[i])));
    CHECK(worst < 1e-6f);

    c.primvars[0].interp = PrimvarInterp::FaceVarying; // continuous face-varying data equals vertex data
    c.primvars[0].indices = c.faceVertexIndices;
    REQUIRE(tessellateSubdiv(c, 8, m));
    a = m.findAttribute("rest");
    REQUIRE(a);
    CHECK(a->rate == AttributeRate::Corner);
    worst = 0.0f;
    for (size_t k = 0; k < m.indices.size(); ++k)
        worst = std::fmax(worst, maxComponent(absolute(valueAt(*a, k) - m.points[m.indices[k]])));
    CHECK(worst < 2e-6f);

    SubdivMeshDesc u = unitCube();
    u.primvars.push_back({"displayColor", PrimvarInterp::Uniform, 3,
                          {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1}, {}});
    u.primvars.push_back({"id", PrimvarInterp::Constant, 1, {7.0f}, {}});
    REQUIRE(tessellateSubdiv(u, 8, m));
    a = m.findAttribute("displayColor");
    REQUIRE(a);
    CHECK(a->rate == AttributeRate::Uniform);
    REQUIRE(a->data.size() == m.triangleCount() * 3);
    int perFace[6] = {};
    for (size_t t = 0; t < m.triangleCount(); ++t) { // 128 triangles per face, in face order
        const float *v = &a->data[t * 3];
        const float *expected = &u.primvars[0].values[(t / 128) * 3];
        perFace[t / 128] += v[0] == expected[0] && v[1] == expected[1] && v[2] == expected[2];
    }
    for (int f = 0; f < 6; ++f)
        CHECK(perFace[f] == 128);
    const MeshAttribute *id = m.findAttribute("id");
    REQUIRE(id);
    CHECK(id->rate == AttributeRate::Constant);
    CHECK(id->data == std::vector<float>{7.0f});
}

// Face-varying st with a seam around every face (each face its own unit
// square, as UV unwraps do): the values stay discontinuous across the seams,
// the face corners keep their authored values (the default
// faceVaryingLinearInterpolation cornersPlus1 pins them) and the interior
// stays inside the face's square.
TEST_CASE("Face-varying st keeps its seams", "[subdiv][attributes]") {
    SubdivMeshDesc c = unitCube();
    SubdivPrimvar st;
    st.name = "st";
    st.interp = PrimvarInterp::FaceVarying;
    st.width = 2;
    for (size_t f = 0; f < 6; ++f) {
        const float u0 = float(f), corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (const auto &q : corners)
            st.values.insert(st.values.end(), {u0 + q[0], q[1]});
    }
    for (int32_t i = 0; i < 24; ++i)
        st.indices.push_back(i);
    c.primvars.push_back(st);
    MeshDesc m;
    REQUIRE(tessellateSubdiv(c, 8, m));
    const MeshAttribute *a = m.findAttribute("st");
    REQUIRE(a);
    REQUIRE(a->rate == AttributeRate::Corner);
    int outside = 0, cornersFound = 0;
    for (size_t k = 0; k < m.indices.size(); ++k) {
        const size_t face = (k / 3) / 128;
        const float s = a->data[k * 2], t = a->data[k * 2 + 1];
        outside += s < float(face) - 1e-5f || s > float(face) + 1.0f + 1e-5f || t < -1e-5f || t > 1.0f + 1e-5f;
        const float fs = s - float(face);
        cornersFound += (std::fabs(fs) < 1e-6f || std::fabs(fs - 1.0f) < 1e-6f) &&
                        (std::fabs(t) < 1e-6f || std::fabs(t - 1.0f) < 1e-6f);
    }
    CHECK(outside == 0);
    CHECK(cornersFound >= 6 * 4); // every face corner appears with its authored value
}

// nova:subdivMaxTriangles lowers the level until the estimate fits, down to
// the control mesh (rate 0) if even level 1 exceeds it. The unit cube gives
// 24 * rate^2 / 2 triangles.
TEST_CASE("The triangle budget chooses the subdivision rate", "[subdiv]") {
    const auto rate = [](int level, uint64_t budget) {
        return chooseSubdivRate(SubdivScheme::CatmullClark, 6, 24, level, budget);
    };
    CHECK(rate(3, 10) == 0);
    CHECK(rate(3, 47) == 0);
    CHECK(rate(3, 48) == 2);
    CHECK(rate(3, 191) == 2);
    CHECK(rate(3, 192) == 4);
    CHECK(rate(3, 1000000000) == 8);
    CHECK(rate(1, 10) == 0); // a requested level 1 is held to the budget as well
    CHECK(rate(1, 48) == 2);
    CHECK(chooseSubdivRate(SubdivScheme::Loop, 8, 24, 2, 32) == 2); // Loop: faces * rate^2
    CHECK(subdivTriangleEstimate(unitCube(), 8) == 768u);
}

// A face with fewer than 3 vertices (dirty geometry) is ignored, as by the
// polygon path and Karma, instead of disabling subdivision for the mesh.
TEST_CASE("Faces with fewer than 3 vertices are ignored", "[subdiv]") {
    MeshDesc clean;
    REQUIRE(tessellateSubdiv(unitCube(), 8, clean));
    for (const int count : {0, 2}) {
        INFO("degenerate face with " << count << " vertices");
        // Inserted as authored face 2, so the faces after it shift.
        SubdivMeshDesc d = unitCube();
        d.faceVertexCounts.insert(d.faceVertexCounts.begin() + 2, count);
        d.faceVertexIndices.insert(d.faceVertexIndices.begin() + 8, {0, 1}); // 2 entries, ignored if count is 0
        if (count == 0)
            d.faceVertexIndices.erase(d.faceVertexIndices.begin() + 8, d.faceVertexIndices.begin() + 10);
        MeshDesc m;
        SubdivStats st;
        std::string err;
        REQUIRE(tessellateSubdiv(d, 8, m, &err, &st));
        CHECK(st.droppedFaces == 1u);
        CHECK(st.triangles == 768u);
        REQUIRE(m.points.size() == clean.points.size());
        bool samePoints = true;
        for (size_t i = 0; i < m.points.size(); ++i)
            samePoints = samePoints && m.points[i].x == clean.points[i].x && m.points[i].y == clean.points[i].y &&
                         m.points[i].z == clean.points[i].z;
        CHECK(samePoints);
        CHECK(m.indices == clean.indices);

        // Uniform primvars and holes refer to the authored faces.
        SubdivPrimvar faceId{"faceId", PrimvarInterp::Uniform, 1, {}, {}};
        for (int f = 0; f < 7; ++f)
            faceId.values.push_back(float(f));
        d.primvars = {faceId};
        REQUIRE(tessellateSubdiv(d, 8, m, &err, &st));
        const MeshAttribute *ids = m.findAttribute("faceId");
        REQUIRE(ids);
        bool idsMatch = true;
        for (size_t t = 0; t < m.indices.size() / 3; ++t) {
            const size_t kept = t / 128, authored = kept < 2 ? kept : kept + 1;
            idsMatch = idsMatch && ids->data[t] == float(authored);
        }
        CHECK(idsMatch);
        d.holeIndices = {4}; // authored face 4 is the cube's face 3
        REQUIRE(tessellateSubdiv(d, 8, m, &err, &st));
        CHECK(st.triangles == 5u * 128u);
        ids = m.findAttribute("faceId");
        REQUIRE(ids);
        bool face3Gone = true;
        for (size_t t = 0; t < m.indices.size() / 3; ++t)
            face3Gone = face3Gone && ids->data[t] != 4.0f;
        CHECK(face3Gone);
    }
    SubdivMeshDesc none = unitCube();
    none.faceVertexCounts = {0, 2};
    none.faceVertexIndices = {0, 1};
    MeshDesc m;
    std::string err;
    CHECK_FALSE(tessellateSubdiv(none, 4, m, &err)); // nothing left
}

// Deformation blur: every motion step is tessellated with the same topology.
// The limit surface commutes with affine maps, so a moved and scaled cage
// gives the moved and scaled limit surface with the same normals.
TEST_CASE("Subdivision tessellates every motion step", "[subdiv][motion]") {
    SubdivMeshDesc d = unitCube();
    const size_t n = d.points.size();
    d.pointSteps = 2;
    for (size_t i = 0; i < n; ++i)
        d.points.push_back(d.points[i] * 2.0f + Vec3f{1.0f, 0.0f, 0.0f});
    MeshDesc m;
    REQUIRE(tessellateSubdiv(d, 4, m));
    REQUIRE(m.pointSteps == 2);
    REQUIRE(m.points.size() == 2 * m.pointCount());
    REQUIRE(m.normalsPerStep());
    const size_t count = m.pointCount();
    float worstP = 0.0f, worstN = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        worstP = std::fmax(worstP, maxComponent(absolute(m.points[count + i] - (m.points[i] * 2.0f +
                                                                                 Vec3f{1.0f, 0.0f, 0.0f}))));
        worstN = std::fmax(worstN, maxComponent(absolute(m.normals[count + i] - m.normals[i])));
    }
    CHECK(worstP < 1e-5f);
    CHECK(worstN < 1e-5f);

    MeshDesc single;
    REQUIRE(tessellateSubdiv(unitCube(), 4, single));
    CHECK(single.indices == m.indices); // the same topology for every step
}
