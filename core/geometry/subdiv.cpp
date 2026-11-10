// Subdivision surface tessellation with OpenSubdiv Bfr (base face evaluation).
//
// Every base face is evaluated on a uniform Bfr::Tessellation pattern. Points
// on base vertices and edges are evaluated once by an owner face and shared by
// index (as in OpenSubdiv's bfr_tutorial_2_2), so the mesh is watertight with
// bit-identical positions. At infinitely sharp edges/vertices the other faces
// get copies of the shared points with their own normals, so sharp creases
// stay sharp in shading.
//
// Pass 1 (serial) assigns every output point and triangle a slot, pass 2
// evaluates faces in parallel (deterministic output) for every deformation
// step, pass 3 resolves copies and degenerate normals.

#include "core/geometry/subdiv.h"

#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <opensubdiv/bfr/surfaceFactoryCache.h>
#include <opensubdiv/bfr/tessellation.h>
#include <opensubdiv/far/topologyDescriptor.h>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <shared_mutex>

namespace nova {

namespace {

using namespace OpenSubdiv; // OpenSubdiv::v3_7_0

using ThreadSafeCache = Bfr::SurfaceFactoryCacheThreaded<std::shared_mutex, std::shared_lock<std::shared_mutex>,
                                                         std::unique_lock<std::shared_mutex>>;
using SurfaceFactory = Bfr::RefinerSurfaceFactory<ThreadSafeCache>;
using Surface = Bfr::Surface<float>;

bool fail(std::string *error, const char *msg) {
    if (error)
        *error = msg;
    return false;
}

// Far reports through process-global callbacks (printf by default); setting
// them is not thread-safe, so do it once.
void installOsdCallbacks() {
    static std::once_flag once;
    std::call_once(once, [] {
        Far::SetErrorCallback([](Far::ErrorType, const char *msg) {
            std::fprintf(stderr, "[Nova] OpenSubdiv error: %s\n", msg ? msg : "");
        });
        Far::SetWarningCallback(
            [](const char *msg) { std::fprintf(stderr, "[Nova] OpenSubdiv warning: %s\n", msg ? msg : ""); });
    });
}

Sdc::SchemeType toSdcScheme(SubdivScheme s) {
    return s == SubdivScheme::Loop       ? Sdc::SCHEME_LOOP
           : s == SubdivScheme::Bilinear ? Sdc::SCHEME_BILINEAR
                                         : Sdc::SCHEME_CATMARK;
}

Sdc::Options toSdcOptions(const SubdivMeshDesc &d) {
    Sdc::Options o; // set all four: the Sdc defaults are not USD's
    o.SetVtxBoundaryInterpolation(Sdc::Options::VtxBoundaryInterpolation(int(d.vtxBoundary)));
    o.SetFVarLinearInterpolation(Sdc::Options::FVarLinearInterpolation(int(d.fvarLinear)));
    o.SetCreasingMethod(d.creaseMethod == SubdivCreaseMethod::Chaikin ? Sdc::Options::CREASE_CHAIKIN
                                                                       : Sdc::Options::CREASE_UNIFORM);
    o.SetTriangleSubdivision(d.triangleRule == SubdivTriangleRule::Smooth ? Sdc::Options::TRI_SUB_SMOOTH
                                                                          : Sdc::Options::TRI_SUB_CATMARK);
    return o;
}

int ceilLog2(int x) {
    int l = 0;
    while ((1 << l) < x)
        ++l;
    return l;
}

// A point of the face's tessellation pattern to evaluate into an output slot;
// copies (position == false) only take the normal, pass 3 copies the position.
struct EvalJob {
    int coord;
    int32_t slot;
    bool position;
};

} // namespace

uint64_t subdivTriangleEstimate(SubdivScheme scheme, size_t faces, size_t faceVertices, int rate) {
    const uint64_t r2 = uint64_t(rate) * uint64_t(rate);
    if (scheme == SubdivScheme::Loop)
        return uint64_t(faces) * r2;
    return uint64_t(faceVertices) * r2 / 2;
}

uint64_t subdivTriangleEstimate(const SubdivMeshDesc &d, int rate) {
    return subdivTriangleEstimate(d.scheme, d.faceVertexCounts.size(), d.faceVertexIndices.size(), rate);
}

int chooseSubdivRate(SubdivScheme scheme, size_t faces, size_t faceVertices, int level, uint64_t budget) {
    for (int l = std::min(level, 10); l >= 1; --l)
        if (subdivTriangleEstimate(scheme, faces, faceVertices, 1 << l) <= budget)
            return 1 << l;
    return 0;
}

bool tessellateSubdiv(const SubdivMeshDesc &d, int rate, MeshDesc &out, std::string *error, SubdivStats *stats,
                      std::vector<int32_t> *triangleFaces) {
    TessellateOptions options;
    options.triangleFaces = triangleFaces;
    return tessellateSubdiv(d, rate, out, options, error, stats);
}

bool tessellateSubdiv(const SubdivMeshDesc &d, int rate, MeshDesc &out, const TessellateOptions &options,
                      std::string *error, SubdivStats *stats) {
    std::vector<int32_t> *triangleFaces = options.triangleFaces;
    installOsdCallbacks();
    if (d.scheme == SubdivScheme::None)
        return fail(error, "scheme 'none' is not a subdivision surface");
    if (rate < 1 || rate > 1024)
        return fail(error, "tessellation rate must be in [1, 1024]");
    const uint32_t steps = std::max(d.pointSteps, 1u);
    if (d.points.size() % steps != 0)
        return fail(error, "points do not split into the motion steps");
    const size_t pointsPerStep = d.points.size() / steps;

    // ---- Validation: Far::TopologyDescriptor bounds-checks neither holes nor
    //      crease vertices; OpenSubdiv rejects Loop with non-triangles itself.
    size_t allFaceVerts = 0;
    bool degenerate = false;
    for (int32_t c : d.faceVertexCounts) {
        if (c < 0)
            return fail(error, "negative face-vertex count");
        allFaceVerts += size_t(c);
        degenerate = degenerate || c < 3;
    }
    if (allFaceVerts != d.faceVertexIndices.size())
        return fail(error, "faceVertexIndices does not match faceVertexCounts");

    // Faces with fewer than 3 vertices have no surface: they are dropped (the
    // polygon path and Karma ignore them too) instead of failing the mesh.
    // faceMap: kept face -> authored face; keptCorners: authored face-vertex
    // positions of the kept faces (for face-varying data).
    const std::vector<int32_t> *faceCounts = &d.faceVertexCounts, *faceIndices = &d.faceVertexIndices;
    std::vector<int32_t> keptCounts, keptIndices, faceMap, keptCorners;
    uint32_t droppedFaces = 0;
    if (degenerate) {
        size_t k = 0;
        for (size_t f = 0; f < d.faceVertexCounts.size(); ++f) {
            const int32_t c = d.faceVertexCounts[f];
            if (c >= 3) {
                keptCounts.push_back(c);
                keptIndices.insert(keptIndices.end(), d.faceVertexIndices.begin() + ptrdiff_t(k),
                                   d.faceVertexIndices.begin() + ptrdiff_t(k + size_t(c)));
                faceMap.push_back(int32_t(f));
                for (int32_t j = 0; j < c; ++j)
                    keptCorners.push_back(int32_t(k) + j);
            } else {
                ++droppedFaces;
            }
            k += size_t(c);
        }
        faceCounts = &keptCounts;
        faceIndices = &keptIndices;
    }
    const auto authoredFace = [&](int f) { return degenerate ? faceMap[size_t(f)] : f; };
    const int numFaces = int(faceCounts->size());
    if (numFaces == 0)
        return fail(error, "mesh has no faces");
    const size_t numFaceVerts = faceIndices->size();
    int32_t maxIndex = -1;
    for (int32_t i : *faceIndices) {
        if (i < 0)
            return fail(error, "negative face-vertex index");
        maxIndex = std::max(maxIndex, i);
    }
    if (size_t(maxIndex) >= pointsPerStep)
        return fail(error, "face-vertex index beyond the points array");
    if (d.scheme == SubdivScheme::Loop && numFaceVerts != size_t(numFaces) * 3)
        return fail(error, "Loop subdivision requires triangles");
    const int numVertices = maxIndex + 1; // as PxOsdRefinerFactory: trailing unused points are ignored

    std::vector<int32_t> holes, creasePairs, corners;
    std::vector<float> creaseWeights, cornerWeights;
    std::vector<int32_t> keptFace; // authored face -> kept face (-1: dropped)
    if (degenerate) {
        keptFace.assign(d.faceVertexCounts.size(), -1);
        for (int f = 0; f < numFaces; ++f)
            keptFace[size_t(faceMap[size_t(f)])] = f;
    }
    for (int32_t h : d.holeIndices) {
        if (h < 0 || size_t(h) >= d.faceVertexCounts.size())
            continue;
        const int32_t kept = degenerate ? keptFace[size_t(h)] : h;
        if (kept >= 0)
            holes.push_back(kept);
    }
    for (size_t e = 0; e < d.creaseSharpness.size() && 2 * e + 1 < d.creaseEdges.size(); ++e) {
        const int32_t v0 = d.creaseEdges[2 * e], v1 = d.creaseEdges[2 * e + 1];
        if (v0 >= 0 && v0 < numVertices && v1 >= 0 && v1 < numVertices) {
            creasePairs.insert(creasePairs.end(), {v0, v1});
            creaseWeights.push_back(std::max(0.0f, d.creaseSharpness[e]));
        }
    }
    for (size_t i = 0; i < d.cornerVertices.size() && i < d.cornerSharpness.size(); ++i)
        if (d.cornerVertices[i] >= 0 && d.cornerVertices[i] < numVertices) {
            corners.push_back(d.cornerVertices[i]);
            cornerWeights.push_back(std::max(0.0f, d.cornerSharpness[i]));
        }

    // Primvars with unusable data render as if absent; faceVarying ones get
    // one face-varying channel each.
    struct Primvar {
        const SubdivPrimvar *src;
        PrimvarInterp interp;
        std::vector<int32_t> fvarIndices;
        int channel = -1;
    };
    std::vector<Primvar> primvars;
    for (const SubdivPrimvar &pv : d.primvars) {
        if (pv.values.empty() || pv.width < 1 || pv.width > 4 || pv.values.size() % pv.width != 0)
            continue;
        const size_t count = pv.values.size() / pv.width;
        Primvar p{&pv, pv.interp, {}, -1};
        if (p.interp == PrimvarInterp::FaceVarying) {
            if (pv.indices.empty()) {
                p.fvarIndices.resize(allFaceVerts);
                std::iota(p.fvarIndices.begin(), p.fvarIndices.end(), 0);
            } else {
                p.fvarIndices = pv.indices;
            }
            bool valid = p.fvarIndices.size() == allFaceVerts;
            for (int32_t i : p.fvarIndices)
                valid = valid && i >= 0 && size_t(i) < count;
            if (!valid)
                continue;
            if (degenerate) {
                std::vector<int32_t> kept(keptCorners.size());
                for (size_t k = 0; k < keptCorners.size(); ++k)
                    kept[k] = p.fvarIndices[size_t(keptCorners[k])];
                p.fvarIndices = std::move(kept);
            }
        }
        if ((p.interp == PrimvarInterp::Vertex || p.interp == PrimvarInterp::Varying) && count < size_t(numVertices))
            continue;
        if (p.interp == PrimvarInterp::Uniform && count < d.faceVertexCounts.size())
            continue;
        primvars.push_back(std::move(p));
    }

    // ---- Base-level topology (never refined; Bfr builds per-face patches) --
    Far::TopologyDescriptor desc;
    desc.numVertices = numVertices;
    desc.numFaces = numFaces;
    desc.numVertsPerFace = faceCounts->data();
    desc.vertIndicesPerFace = faceIndices->data();
    desc.numCreases = int(creaseWeights.size());
    desc.creaseVertexIndexPairs = creasePairs.data();
    desc.creaseWeights = creaseWeights.data();
    desc.numCorners = int(cornerWeights.size());
    desc.cornerVertexIndices = corners.data();
    desc.cornerWeights = cornerWeights.data();
    desc.numHoles = int(holes.size());
    desc.holeIndices = holes.data();
    desc.isLeftHanded = d.leftHanded;
    std::vector<Far::TopologyDescriptor::FVarChannel> channels;
    for (Primvar &p : primvars) {
        if (p.interp != PrimvarInterp::FaceVarying)
            continue;
        p.channel = int(channels.size());
        Far::TopologyDescriptor::FVarChannel channel;
        channel.numValues = int(p.src->values.size() / p.src->width);
        channel.valueIndices = p.fvarIndices.data();
        channels.push_back(channel);
    }
    desc.numFVarChannels = int(channels.size());
    desc.fvarChannels = channels.empty() ? nullptr : channels.data();
    using Factory = Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
    const std::unique_ptr<Far::TopologyRefiner> refiner(
        Factory::Create(desc, Factory::Options(toSdcScheme(d.scheme), toSdcOptions(d))));
    if (!refiner)
        return fail(error, "OpenSubdiv rejected the topology");

    const Far::TopologyLevel &base = refiner->GetLevel(0);
    const int numEdges = base.GetNumEdges(), numBaseVerts = base.GetNumVertices();
    // Segments per control edge: the uniform rate or the caller's.
    std::vector<int> edgeRates(size_t(numEdges), rate);
    int maxRate = rate;
    if (options.edgeRate) {
        maxRate = 1;
        for (int e = 0; e < numEdges; ++e) {
            const Far::ConstIndexArray ev = base.GetEdgeVertices(e);
            edgeRates[size_t(e)] = std::clamp(options.edgeRate(ev[0], ev[1]), 1, 1024);
            maxRate = std::max(maxRate, edgeRates[size_t(e)]);
        }
    }

    // Patch-tree depth around extraordinary vertices: >= log2(rate) makes the
    // samples exact limit points, +1 keeps derivative normals within ~0.2 deg
    // of the exact limit normals (the default of 2 is ~0.3% off in position).
    SurfaceFactory::Options factoryOptions;
    factoryOptions.SetApproxLevelSmooth(std::clamp(ceilLog2(maxRate) + 1, 3, 10));
    factoryOptions.SetApproxLevelSharp(std::clamp(ceilLog2(maxRate) + 1, 6, 10));
    if (!channels.empty())
        factoryOptions.SetDefaultFVarID(0);
    const SurfaceFactory factory(*refiner, factoryOptions);

    // ---- Pass 1 (serial): sharp features, ownership, output slots ----------
    // Faces disagree on the normal at interior infinitely sharp / non-manifold
    // edges, at their vertices and at sharp (cone) vertices: those faces get
    // their own copies of the shared points.
    std::vector<uint8_t> edgeSharp(size_t(numEdges), 0), vertSharp(size_t(numBaseVerts), 0);
    for (int e = 0; e < numEdges; ++e) {
        if ((base.GetEdgeFaces(e).size() == 2 && base.IsEdgeInfSharp(e)) || base.IsEdgeNonManifold(e)) {
            edgeSharp[size_t(e)] = 1;
            const Far::ConstIndexArray ev = base.GetEdgeVertices(e);
            vertSharp[size_t(ev[0])] = vertSharp[size_t(ev[1])] = 1;
        }
    }
    for (int v = 0; v < numBaseVerts; ++v)
        if ((base.IsVertexInfSharp(v) && base.GetVertexFaces(v).size() > 1) || base.IsVertexNonManifold(v))
            vertSharp[size_t(v)] = 1;

    Bfr::Tessellation::Options tessOptions; // triangles
    struct Counts {
        int interior, facets;
    };
    std::map<std::pair<int, int>, Counts> countCache; // uniform: (parameterization type, face size)
    auto faceRates = [&](int f, std::vector<int> &rates) {
        const Far::ConstIndexArray fe = base.GetFaceEdges(f);
        rates.resize(size_t(fe.size()));
        for (int i = 0; i < fe.size(); ++i)
            rates[size_t(i)] = edgeRates[size_t(fe[i])];
    };
    std::vector<int> rateScratch;
    auto countsFor = [&](int f, const Bfr::Parameterization &p) {
        if (options.edgeRate) {
            faceRates(f, rateScratch);
            const Bfr::Tessellation t(p, int(rateScratch.size()), rateScratch.data(), tessOptions);
            return Counts{t.GetNumInteriorCoords(), t.GetNumFacets()};
        }
        const auto key = std::make_pair(int(p.GetType()), p.GetFaceSize());
        auto it = countCache.find(key);
        if (it == countCache.end()) {
            const Bfr::Tessellation t(p, rate, tessOptions);
            it = countCache.emplace(key, Counts{t.GetNumInteriorCoords(), t.GetNumFacets()}).first;
        }
        return it->second;
    };

    std::vector<uint8_t> faceValid(size_t(numFaces), 0);
    std::vector<int32_t> vertOwner(size_t(numBaseVerts), -1), vertOut(size_t(numBaseVerts), -1);
    std::vector<int32_t> edgeOwner(size_t(numEdges), -1), edgeOut(size_t(numEdges), -1),
        edgeStart(size_t(numEdges), -1);
    auto perEdge = [&](int e) { return edgeRates[size_t(e)] - 1; }; // points inside a control edge
    int32_t nextPoint = 0;
    uint32_t skippedFaces = 0;
    for (int f = 0; f < numFaces; ++f) {
        // false for holes (incl. boundary faces when interpolateBoundary is
        // "none", which OpenSubdiv tags as holes) and degenerate faces
        faceValid[size_t(f)] = factory.FaceHasLimitSurface(f) ? 1 : 0;
        if (!faceValid[size_t(f)]) {
            skippedFaces += base.IsFaceHole(f) ? 0u : 1u;
            continue;
        }
        const Far::ConstIndexArray fv = base.GetFaceVertices(f), fe = base.GetFaceEdges(f);
        for (int i = 0; i < fv.size(); ++i) {
            if (vertOwner[size_t(fv[i])] < 0) {
                vertOwner[size_t(fv[i])] = f;
                vertOut[size_t(fv[i])] = nextPoint++;
            }
            if (edgeOwner[size_t(fe[i])] < 0) {
                edgeOwner[size_t(fe[i])] = f;
                edgeStart[size_t(fe[i])] = fv[i]; // the owner walks the edge from this vertex
                edgeOut[size_t(fe[i])] = nextPoint;
                nextPoint += perEdge(fe[i]);
            }
        }
    }
    const int32_t sharedPoints = nextPoint;
    std::vector<int32_t> interiorStart(size_t(numFaces) + 1, 0), copyStart(size_t(numFaces) + 1, 0),
        facetStart(size_t(numFaces) + 1, 0);
    for (int f = 0; f < numFaces; ++f) {
        int interior = 0, facets = 0, copies = 0;
        if (faceValid[size_t(f)]) {
            const Counts c = countsFor(f, factory.GetFaceParameterization(f));
            interior = c.interior;
            facets = c.facets;
            const Far::ConstIndexArray fv = base.GetFaceVertices(f), fe = base.GetFaceEdges(f);
            for (int i = 0; i < fv.size(); ++i) {
                copies += (vertSharp[size_t(fv[i])] && vertOwner[size_t(fv[i])] != f) ? 1 : 0;
                copies += (edgeSharp[size_t(fe[i])] && edgeOwner[size_t(fe[i])] != f) ? perEdge(fe[i]) : 0;
            }
        }
        interiorStart[size_t(f) + 1] = interiorStart[size_t(f)] + interior;
        copyStart[size_t(f) + 1] = copyStart[size_t(f)] + copies;
        facetStart[size_t(f) + 1] = facetStart[size_t(f)] + facets;
    }
    const int32_t copyBase = sharedPoints + interiorStart[size_t(numFaces)];
    const size_t numPoints = size_t(copyBase) + size_t(copyStart[size_t(numFaces)]);
    const size_t numTris = size_t(facetStart[size_t(numFaces)]);
    if (numTris == 0)
        return fail(error, "every face is a hole");

    out.pointSteps = steps;
    out.points.assign(numPoints * steps, Vec3f{0.0f, 0.0f, 0.0f});
    out.normals.assign(numPoints * steps, Vec3f{0.0f, 0.0f, 0.0f});
    out.indices.assign(numTris * 3, 0u);
    std::vector<int32_t> copySource(static_cast<size_t>(copyStart[size_t(numFaces)]), -1);
    // The output attributes, one per usable primvar, in the same order.
    out.attributes.clear();
    for (const Primvar &p : primvars) {
        MeshAttribute a;
        a.name = p.src->name;
        a.width = p.src->width;
        switch (p.interp) {
        case PrimvarInterp::Vertex:
        case PrimvarInterp::Varying:
            a.rate = AttributeRate::Vertex;
            a.data.assign(numPoints * a.width, 0.0f);
            break;
        case PrimvarInterp::FaceVarying:
            a.rate = AttributeRate::Corner;
            a.data.assign(numTris * 3 * a.width, 0.0f);
            break;
        case PrimvarInterp::Uniform:
            a.rate = AttributeRate::Uniform;
            a.data.assign(numTris * a.width, 0.0f);
            break;
        default:
            a.rate = AttributeRate::Constant;
            a.data.assign(p.src->values.begin(), p.src->values.begin() + a.width);
            break;
        }
        out.attributes.push_back(std::move(a));
    }

    // ---- Pass 2 (parallel over faces): evaluate and emit --------------------
    const float *meshPoints = &d.points[0].x;
    tbb::parallel_for(tbb::blocked_range<int>(0, numFaces, 64), [&](const tbb::blocked_range<int> &range) {
        Surface surface, varyingSurface, fvarSurface;
        std::vector<float> coords, patch, pvPatch, fvarValues;
        std::vector<int> boundaryIdx, facets, localFacets, rates;
        std::vector<EvalJob> jobs;
        for (int f = range.begin(); f != range.end(); ++f) {
            if (!faceValid[size_t(f)] || !factory.InitVertexSurface(f, &surface))
                continue;
            faceRates(f, rates);
            std::optional<Bfr::Tessellation> pattern; // not movable
            if (options.edgeRate)
                pattern.emplace(surface.GetParameterization(), int(rates.size()), rates.data(), tessOptions);
            else
                pattern.emplace(surface.GetParameterization(), rate, tessOptions);
            Bfr::Tessellation &tess = *pattern;
            coords.resize(size_t(tess.GetNumCoords()) * 2);
            tess.GetCoords(coords.data());
            patch.resize(size_t(surface.GetNumPatchPoints()) * 3);

            // Which pattern coordinates this face evaluates into which slots.
            // Boundary of the pattern: vertex i, then the points of edge i.
            const int nb = tess.GetNumBoundaryCoords();
            boundaryIdx.resize(size_t(nb));
            jobs.clear();
            const Far::ConstIndexArray fv = base.GetFaceVertices(f), fe = base.GetFaceEdges(f);
            int32_t nextCopy = copyBase + copyStart[size_t(f)];
            int b = 0;
            for (int i = 0; i < fv.size(); ++i) {
                const int v = fv[i], e = fe[i];
                if (vertOwner[size_t(v)] == f) {
                    jobs.push_back({b, vertOut[size_t(v)], true});
                    boundaryIdx[size_t(b)] = vertOut[size_t(v)];
                } else if (vertSharp[size_t(v)]) {
                    jobs.push_back({b, nextCopy, false});
                    copySource[size_t(nextCopy - copyBase)] = vertOut[size_t(v)];
                    boundaryIdx[size_t(b)] = nextCopy++;
                } else {
                    boundaryIdx[size_t(b)] = vertOut[size_t(v)];
                }
                ++b;
                const bool owner = edgeOwner[size_t(e)] == f;
                const bool sameDirection = owner || edgeStart[size_t(e)] == v;
                const int inner = perEdge(e);
                for (int j = 0; j < inner; ++j, ++b) {
                    const int32_t shared = edgeOut[size_t(e)] + (sameDirection ? j : inner - 1 - j);
                    if (owner) {
                        jobs.push_back({b, shared, true});
                        boundaryIdx[size_t(b)] = shared;
                    } else if (edgeSharp[size_t(e)]) {
                        jobs.push_back({b, nextCopy, false});
                        copySource[size_t(nextCopy - copyBase)] = shared;
                        boundaryIdx[size_t(b)] = nextCopy++;
                    } else {
                        boundaryIdx[size_t(b)] = shared;
                    }
                }
            }
            const int32_t interiorOut = sharedPoints + interiorStart[size_t(f)];
            for (int c = nb; c < tess.GetNumCoords(); ++c)
                jobs.push_back({c, interiorOut + (c - nb), true});

            // The same pattern for every deformation step; colors do not move.
            for (uint32_t s = 0; s < steps; ++s) {
                surface.PreparePatchPoints(meshPoints + size_t(s) * pointsPerStep * 3, 3, patch.data(), 3);
                const size_t stepBase = size_t(s) * numPoints;
                for (const EvalJob &job : jobs) {
                    const float *uv = &coords[size_t(job.coord) * 2];
                    float P[3], Du[3], Dv[3];
                    surface.Evaluate(uv, patch.data(), 3, P, Du, Dv);
                    const Vec3f du{Du[0], Du[1], Du[2]}, dv{Dv[0], Dv[1], Dv[2]}, n = cross(du, dv);
                    // Degenerate parameterization (e.g. a valence-2 boundary
                    // vertex with edgeOnly: Du and Dv antiparallel): pass 3
                    // uses the triangles.
                    out.normals[stepBase + size_t(job.slot)] =
                        dot(n, n) > 1e-6f * dot(du, du) * dot(dv, dv) ? n : Vec3f{0.0f, 0.0f, 0.0f};
                    if (!job.position)
                        continue;
                    out.points[stepBase + size_t(job.slot)] = {P[0], P[1], P[2]};
                }
            }

            // Point primvars: on the limit surface (vertex) or bilinear (varying).
            for (size_t pi = 0; pi < primvars.size(); ++pi) {
                const Primvar &pv = primvars[pi];
                if (pv.interp != PrimvarInterp::Vertex && pv.interp != PrimvarInterp::Varying)
                    continue;
                const Surface *eval = &surface;
                if (pv.interp == PrimvarInterp::Varying) {
                    if (!factory.InitVaryingSurface(f, &varyingSurface))
                        continue;
                    eval = &varyingSurface;
                }
                const int w = pv.src->width;
                pvPatch.resize(size_t(eval->GetNumPatchPoints()) * size_t(w));
                eval->PreparePatchPoints(pv.src->values.data(), w, pvPatch.data(), w);
                float *dst = out.attributes[pi].data.data();
                for (const EvalJob &job : jobs)
                    if (job.position)
                        eval->Evaluate(&coords[size_t(job.coord) * 2], pvPatch.data(), w, dst + size_t(job.slot) * size_t(w));
            }

            facets.resize(size_t(tess.GetNumFacets()) * 3);
            tess.GetFacets(facets.data());
            localFacets = facets; // pattern-local coordinate indices
            tess.TransformFacetCoordIndices(facets.data(), boundaryIdx.data(), interiorOut - nb);
            const size_t firstCorner = size_t(facetStart[size_t(f)]) * 3;
            for (size_t k = 0; k < facets.size(); ++k)
                out.indices[firstCorner + k] = uint32_t(facets[k]);

            // Per-triangle and per-corner primvars.
            const size_t firstTri = size_t(facetStart[size_t(f)]);
            for (size_t pi = 0; pi < primvars.size(); ++pi) {
                const Primvar &pv = primvars[pi];
                const size_t w = pv.src->width;
                float *dst = out.attributes[pi].data.data();
                if (pv.interp == PrimvarInterp::Uniform) {
                    const float *v = &pv.src->values[size_t(authoredFace(f)) * w];
                    for (size_t t = 0; t < facets.size() / 3; ++t)
                        std::copy(v, v + w, dst + (firstTri + t) * w);
                } else if (pv.interp == PrimvarInterp::FaceVarying &&
                           factory.InitFaceVaryingSurface(f, &fvarSurface, pv.channel)) {
                    pvPatch.resize(size_t(fvarSurface.GetNumPatchPoints()) * w);
                    fvarSurface.PreparePatchPoints(pv.src->values.data(), int(w), pvPatch.data(), int(w));
                    fvarValues.resize(coords.size() / 2 * w);
                    for (size_t c = 0; c < coords.size() / 2; ++c)
                        fvarSurface.Evaluate(&coords[c * 2], pvPatch.data(), int(w), &fvarValues[c * w]);
                    for (size_t k = 0; k < localFacets.size(); ++k)
                        std::copy_n(&fvarValues[size_t(localFacets[k]) * w], w, dst + (firstCorner + k) * w);
                }
            }
        }
    });

    // ---- Pass 3: copies take the owner's position bits; normals -------------
    for (uint32_t s = 0; s < steps; ++s) {
        Vec3f *points = out.points.data() + size_t(s) * numPoints;
        Vec3f *normals = out.normals.data() + size_t(s) * numPoints;
        for (size_t i = 0; i < copySource.size(); ++i)
            points[size_t(copyBase) + i] = points[size_t(copySource[i])];
        std::vector<Vec3f> triangleSum; // only built if some derivative was degenerate
        for (size_t i = 0; i < numPoints; ++i) {
            Vec3f &n = normals[i];
            if (!(dot(n, n) > 0.0f) || !isFinite(n)) {
                if (triangleSum.empty()) {
                    triangleSum.assign(numPoints, Vec3f{0.0f, 0.0f, 0.0f});
                    for (size_t t = 0; t < numTris; ++t) {
                        const uint32_t *ix = &out.indices[t * 3];
                        const Vec3f ng = cross(points[ix[1]] - points[ix[0]], points[ix[2]] - points[ix[0]]);
                        for (int k = 0; k < 3; ++k)
                            triangleSum[ix[k]] += ng;
                    }
                }
                n = triangleSum[i];
            }
            n = normalize(n);
        }
    }
    // Copies take the owner's point primvars.
    for (size_t pi = 0; pi < primvars.size(); ++pi) {
        if (primvars[pi].interp != PrimvarInterp::Vertex && primvars[pi].interp != PrimvarInterp::Varying)
            continue;
        const size_t w = primvars[pi].src->width;
        float *data = out.attributes[pi].data.data();
        for (size_t i = 0; i < copySource.size(); ++i)
            std::copy_n(data + size_t(copySource[i]) * w, w, data + (size_t(copyBase) + i) * w);
    }

    if (options.copies) {
        options.copies->clear();
        for (size_t i = 0; i < copySource.size(); ++i)
            options.copies->emplace_back(int32_t(copyBase) + int32_t(i), copySource[i]);
    }
    if (triangleFaces) {
        triangleFaces->resize(numTris);
        for (int f = 0; f < numFaces; ++f)
            std::fill(triangleFaces->begin() + facetStart[size_t(f)], triangleFaces->begin() + facetStart[size_t(f) + 1],
                      int32_t(authoredFace(f)));
    }
    if (stats) {
        stats->triangles = uint32_t(numTris);
        stats->splitVertices = uint32_t(copySource.size());
        stats->skippedFaces = skippedFaces;
        stats->droppedFaces = droppedFaces;
    }
    return true;
}

} // namespace nova
