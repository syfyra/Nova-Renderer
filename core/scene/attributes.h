#pragma once

// Generic shading attributes (docs/design/M3.md K12): every numeric primvar
// of a mesh or instancer, by name, for the shading programs' Primvar reads.
// Lookup order for a name: the mesh, then the instance, then the program's
// fallback (the old displayColor rule, generalized).

#include "core/math/common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nova {

// Constant: one value. Uniform: one per triangle. Vertex: one per point,
// interpolated with the triangle's barycentrics. Corner: one per triangle
// corner (3 per triangle), interpolated. Curves (MeshShape::Curves) come
// with Uniform per curve, Vertex per control point and Varying per segment
// end (USD's rates); prepareCurves() turns them into Uniform per segment and
// Corner (start, end, end), interpolated along the segment.
enum class AttributeRate : uint8_t { Constant, Uniform, Vertex, Corner, Varying };

struct MeshAttribute {
    std::string name;
    AttributeRate rate = AttributeRate::Constant;
    uint8_t width = 1;       // floats per value, 1..4
    std::vector<float> data; // width floats per element
};

// One value per instance, in the order of the mesh's instances.
struct InstanceAttribute {
    std::string name;
    uint8_t width = 1;
    std::vector<float> data;
};

// An attribute resolved for one (mesh, material) pair: where a program's
// primvar reads come from. POD: the GPU gets the same view.
// Parametric: the hit's own surface parameters (b1, b2), the st of shapes
// without an st primvar (analytic spheres).
enum class AttributeSource : uint8_t { None, Constant, Uniform, Vertex, Corner, Instance, Parametric };
struct AttributeRef {
    const float *data = nullptr;
    AttributeSource source = AttributeSource::None;
    uint8_t width = 0;
};

// The value of `ref` at a hit (barycentrics b1, b2 of corners 1 and 2),
// converted to `width` floats: a 1-wide attribute splats, a narrower one is
// padded with zeros, a wider one truncated. Returns false for None.
NOVA_INLINE bool fetchAttribute(const AttributeRef &ref, const uint32_t *triangle, uint32_t prim, uint32_t instance,
                                float b1, float b2, int width, float *out) {
    const int w = ref.width;
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (ref.source) {
    case AttributeSource::None:
        return false;
    case AttributeSource::Parametric:
        v[0] = b1;
        v[1] = b2;
        v[2] = 0.0f;
        v[3] = 0.0f;
        for (int k = 0; k < width; ++k)
            out[k] = v[k];
        return true;
    case AttributeSource::Constant:
        for (int k = 0; k < w; ++k)
            v[k] = ref.data[k];
        break;
    case AttributeSource::Uniform:
        for (int k = 0; k < w; ++k)
            v[k] = ref.data[size_t(prim) * size_t(w) + size_t(k)];
        break;
    case AttributeSource::Instance:
        for (int k = 0; k < w; ++k)
            v[k] = ref.data[size_t(instance) * size_t(w) + size_t(k)];
        break;
    case AttributeSource::Vertex:
    case AttributeSource::Corner: {
        const float b0 = 1.0f - b1 - b2;
        size_t i0, i1, i2;
        if (ref.source == AttributeSource::Vertex) {
            i0 = triangle[0];
            i1 = triangle[1];
            i2 = triangle[2];
        } else {
            i0 = size_t(prim) * 3;
            i1 = i0 + 1;
            i2 = i0 + 2;
        }
        for (int k = 0; k < w; ++k)
            v[k] = ref.data[i0 * size_t(w) + size_t(k)] * b0 + ref.data[i1 * size_t(w) + size_t(k)] * b1 +
                   ref.data[i2 * size_t(w) + size_t(k)] * b2;
        break;
    }
    }
    for (int k = 0; k < width; ++k)
        out[k] = w == 1 ? v[0] : (k < w ? v[k] : 0.0f);
    return true;
}

} // namespace nova
