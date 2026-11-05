#include "core/geometry/curves.h"

#include <utility>

namespace nova {

bool prepareCurves(MeshDesc &desc) {
    if (!desc.isCurves() || !desc.curveSegments.empty())
        return true;
    const size_t n = desc.pointCount();
    size_t total = 0;
    for (uint32_t c : desc.curveVertexCounts)
        total += c;
    if (desc.curveRadius.size() == 1)
        desc.curveRadius.assign(n, desc.curveRadius[0]);
    const bool radiusOk = desc.curveRadius.size() == n || desc.curveRadius.size() == desc.points.size();
    if (total != n || n == 0 || !radiusOk) {
        desc.segmentCurve.clear();
        return false;
    }

    // Segments, and per curve its first segment and its segment count.
    const uint32_t stride = curveSegmentStride(desc.curveBasis);
    const size_t curves = desc.curveVertexCounts.size();
    std::vector<uint32_t> firstSegment(curves), segmentCounts(curves);
    uint32_t base = 0;
    for (size_t c = 0; c < curves; ++c) {
        const uint32_t count = desc.curveVertexCounts[c];
        const uint32_t segments = curveSegmentCount(desc.curveBasis, count);
        firstSegment[c] = uint32_t(desc.curveSegments.size());
        segmentCounts[c] = segments;
        for (uint32_t s = 0; s < segments; ++s) {
            desc.curveSegments.push_back(base + s * stride);
            desc.segmentCurve.push_back(uint32_t(c));
        }
        base += count;
    }
    const size_t segments = desc.curveSegments.size();

    // Attributes per segment.
    const int k = curveSegmentPoints(desc.curveBasis);
    const CurveWeights at0 = curveWeights(desc.curveBasis, 0.0f), at1 = curveWeights(desc.curveBasis, 1.0f);
    for (MeshAttribute &a : desc.attributes) {
        const size_t w = a.width;
        std::vector<float> out;
        switch (a.rate) {
        case AttributeRate::Constant:
            continue;
        case AttributeRate::Uniform: // per curve
            if (a.data.size() < curves * w)
                break;
            out.resize(segments * w);
            for (size_t s = 0; s < segments; ++s)
                for (size_t j = 0; j < w; ++j)
                    out[s * w + j] = a.data[size_t(desc.segmentCurve[s]) * w + j];
            a.rate = AttributeRate::Uniform;
            break;
        case AttributeRate::Vertex: // per control point: the basis at the segment's ends
            if (a.data.size() < n * w)
                break;
            out.resize(segments * 3 * w);
            for (size_t s = 0; s < segments; ++s)
                for (size_t j = 0; j < w; ++j) {
                    float v0 = 0.0f, v1 = 0.0f;
                    for (int i = 0; i < k; ++i) {
                        const float v = a.data[(size_t(desc.curveSegments[s]) + size_t(i)) * w + j];
                        v0 += v * at0.w[i];
                        v1 += v * at1.w[i];
                    }
                    out[(s * 3) * w + j] = v0;
                    out[(s * 3 + 1) * w + j] = v1;
                    out[(s * 3 + 2) * w + j] = v1;
                }
            a.rate = AttributeRate::Corner;
            break;
        default: { // Varying (and Corner): per segment end, segments + 1 per curve
            size_t need = 0;
            for (size_t c = 0; c < curves; ++c)
                need += segmentCounts[c] > 0 ? segmentCounts[c] + 1 : 0;
            if (a.data.size() < need * w)
                break;
            out.resize(segments * 3 * w);
            size_t offset = 0;
            for (size_t c = 0; c < curves; ++c) {
                for (uint32_t s = 0; s < segmentCounts[c]; ++s) {
                    const size_t seg = size_t(firstSegment[c]) + s;
                    for (size_t j = 0; j < w; ++j) {
                        out[(seg * 3) * w + j] = a.data[(offset + s) * w + j];
                        out[(seg * 3 + 1) * w + j] = a.data[(offset + s + 1) * w + j];
                        out[(seg * 3 + 2) * w + j] = a.data[(offset + s + 1) * w + j];
                    }
                }
                offset += segmentCounts[c] > 0 ? segmentCounts[c] + 1 : 0;
            }
            a.rate = AttributeRate::Corner;
            break;
        }
        }
        a.data = std::move(out); // too little data: empty, the attribute reads as absent
    }
    return true;
}

} // namespace nova
