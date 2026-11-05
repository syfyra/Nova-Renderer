// HdMeshTopology + PxOsdSubdivTags -> nova::SubdivMeshDesc, mirroring
// PxOsdRefinerFactory's USD conventions (pxr/imaging/pxOsd/refinerFactory.cpp,
// OpenUSD v26.05: Converter::GetType / GetOptions / assignComponentTopology /
// assignComponentTags). Verified against the factory for 18 tag combinations.

#include "meshSubdiv.h"

#include "diagnostics.h"

#include <pxr/imaging/pxOsd/tokens.h>

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

bool HdNovaGetSubdivScheme(TfToken const &scheme, nova::SubdivScheme *out) {
    if (scheme == PxOsdOpenSubdivTokens->catmullClark)
        *out = nova::SubdivScheme::CatmullClark;
    else if (scheme == PxOsdOpenSubdivTokens->loop)
        *out = nova::SubdivScheme::Loop;
    else if (scheme == PxOsdOpenSubdivTokens->bilinear)
        *out = nova::SubdivScheme::Bilinear;
    else
        return false; // "none" (and unknown schemes) are polygon meshes
    return true;
}

bool HdNovaToSubdivDesc(HdMeshTopology const &topology, PxOsdSubdivTags const &tags,
                        std::vector<VtVec3fArray> const &pointSteps, SdfPath const &id, nova::SubdivMeshDesc *out) {
    nova::SubdivMeshDesc &d = *out;
    if (!HdNovaGetSubdivScheme(topology.GetScheme(), &d.scheme) || pointSteps.empty())
        return false;
    const char *name = id.GetText();

    // interpolateBoundary: bilinear is forced to edgeAndCorner (pxOsd works
    // around an old OpenSubdiv bug); empty = edgeAndCorner (legacy default);
    // unknown tokens keep the Sdc default (none) after a warning.
    const TfToken vb = d.scheme == nova::SubdivScheme::Bilinear ? PxOsdOpenSubdivTokens->edgeAndCorner
                                                                 : tags.GetVertexInterpolationRule();
    if (vb.IsEmpty() || vb == PxOsdOpenSubdivTokens->edgeAndCorner)
        d.vtxBoundary = nova::SubdivVtxBoundary::EdgeAndCorner;
    else if (vb == PxOsdOpenSubdivTokens->edgeOnly)
        d.vtxBoundary = nova::SubdivVtxBoundary::EdgeOnly;
    else if (vb == PxOsdOpenSubdivTokens->none)
        d.vtxBoundary = nova::SubdivVtxBoundary::None;
    else {
        HD_NOVA_WARN("unknown vertex boundary interpolation rule '%s' on %s", vb.GetText(), name);
        d.vtxBoundary = nova::SubdivVtxBoundary::None;
    }

    // faceVaryingLinearInterpolation: empty = none (smooth); unknown = Sdc default (all).
    const TfToken fv = tags.GetFaceVaryingInterpolationRule();
    if (fv.IsEmpty() || fv == PxOsdOpenSubdivTokens->none)
        d.fvarLinear = nova::SubdivFVarLinear::None;
    else if (fv == PxOsdOpenSubdivTokens->cornersOnly)
        d.fvarLinear = nova::SubdivFVarLinear::CornersOnly;
    else if (fv == PxOsdOpenSubdivTokens->cornersPlus1)
        d.fvarLinear = nova::SubdivFVarLinear::CornersPlus1;
    else if (fv == PxOsdOpenSubdivTokens->cornersPlus2)
        d.fvarLinear = nova::SubdivFVarLinear::CornersPlus2;
    else if (fv == PxOsdOpenSubdivTokens->boundaries)
        d.fvarLinear = nova::SubdivFVarLinear::Boundaries;
    else if (fv == PxOsdOpenSubdivTokens->all)
        d.fvarLinear = nova::SubdivFVarLinear::All;
    else {
        HD_NOVA_WARN("unknown face-varying interpolation rule '%s' on %s", fv.GetText(), name);
        d.fvarLinear = nova::SubdivFVarLinear::All;
    }

    const TfToken cm = tags.GetCreaseMethod(); // not authored by UsdGeom; Hydra leaves it empty
    d.creaseMethod = cm == PxOsdOpenSubdivTokens->chaikin ? nova::SubdivCreaseMethod::Chaikin
                                                         : nova::SubdivCreaseMethod::Uniform;
    if (!cm.IsEmpty() && cm != PxOsdOpenSubdivTokens->uniform && cm != PxOsdOpenSubdivTokens->chaikin)
        HD_NOVA_WARN("unknown creasing method '%s' on %s", cm.GetText(), name);

    const TfToken ts = tags.GetTriangleSubdivision();
    d.triangleRule = ts == PxOsdOpenSubdivTokens->smooth ? nova::SubdivTriangleRule::Smooth
                                                        : nova::SubdivTriangleRule::CatmullClark;
    if (!ts.IsEmpty() && ts != PxOsdOpenSubdivTokens->smooth && ts != PxOsdOpenSubdivTokens->catmullClark)
        HD_NOVA_WARN("unknown triangle subdivision rule '%s' on %s", ts.GetText(), name);

    // Any orientation other than rightHanded flips (as pxOsd and HdMeshUtil).
    d.leftHanded = topology.GetOrientation() != PxOsdOpenSubdivTokens->rightHanded;

    const VtIntArray &counts = topology.GetFaceVertexCounts(), &indices = topology.GetFaceVertexIndices();
    d.faceVertexCounts.assign(counts.cbegin(), counts.cend());
    d.faceVertexIndices.assign(indices.cbegin(), indices.cend());
    const VtIntArray &holes = topology.GetHoleIndices();
    d.holeIndices.assign(holes.cbegin(), holes.cend());

    // Creases: weights per crease (one per creaseLengths entry) or per edge
    // (sum of length - 1); any other count drops all creases.
    const VtIntArray &ci = tags.GetCreaseIndices(), &cl = tags.GetCreaseLengths();
    const VtFloatArray &cw = tags.GetCreaseWeights();
    size_t numSets = cl.size();
    const bool perEdge = cw.size() != numSets;
    if (perEdge) {
        size_t numEdges = 0;
        for (int len : cl)
            numEdges += size_t(std::max(len - 1, 0));
        if (cw.size() != numEdges) {
            HD_NOVA_WARN("invalid number of crease sharpnesses on %s, ignoring the creases", name);
            numSets = 0;
        }
    }
    d.creaseEdges.clear();
    d.creaseSharpness.clear();
    for (size_t i = 0, c = 0, s = 0; i < numSets; ++i) {
        const int segments = std::max(cl[i] - 1, 0);
        for (int j = 0; j < segments; ++j) {
            if (c + size_t(j) + 1 < ci.size()) { // core drops pairs outside [0, numVertices)
                d.creaseEdges.push_back(ci[c + size_t(j)]);
                d.creaseEdges.push_back(ci[c + size_t(j) + 1]);
                d.creaseSharpness.push_back(std::max(0.0f, cw[s]));
            }
            if (perEdge)
                ++s;
        }
        if (!perEdge)
            ++s;
        c += size_t(std::max(cl[i], 0));
    }

    const VtIntArray &cv = tags.GetCornerIndices();
    const VtFloatArray &cvw = tags.GetCornerWeights();
    d.cornerVertices.clear();
    d.cornerSharpness.clear();
    if (cvw.size() != cv.size()) {
        if (!cv.empty())
            HD_NOVA_WARN("invalid number of corner sharpnesses on %s, ignoring the corners", name);
    } else {
        d.cornerVertices.assign(cv.cbegin(), cv.cend());
        for (float w : cvw)
            d.cornerSharpness.push_back(std::max(0.0f, w));
    }

    // Control points of every step (all steps have the same size).
    const size_t count = pointSteps[0].size();
    d.pointSteps = uint32_t(pointSteps.size());
    d.points.resize(count * pointSteps.size());
    for (size_t s = 0; s < pointSteps.size(); ++s) {
        if (pointSteps[s].size() != count)
            return false;
        for (size_t i = 0; i < count; ++i)
            d.points[s * count + i] = {pointSteps[s][i][0], pointSteps[s][i][1], pointSteps[s][i][2]};
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
