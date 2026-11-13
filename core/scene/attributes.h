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

// The parameters of a face: the texture coordinates Karma gives geometry
// without st (docs/bug-audit-2026-11.md, section 21; measured). Every face
// has its own (u, v) over 0..1, from its vertices in their order:
//   a quad 0 1 2 3      (0,0) (1,0) (1,1) (0,1) at its corners, linear over
//                       each of its triangles 3 0 1 and 3 1 2;
//   a triangle a b c    u = w_b / (w_b + w_c), v = w_b + w_c of its
//                       barycentrics: a is the whole edge v = 0;
//   more vertices       a strip of quads 0 1 2 n-1, then k+1 k+2 n-1-k n-k
//                       for k = 1 .. and a last triangle where the count is
//                       odd, each as above; a concave face's triangles each
//                       as a triangle.
// A program asks for them with the name kFaceParametersName at the end of a
// primvar's list of names ("st|@face"). A mesh has them as one code per
// triangle (MeshDesc::faceParameters, read through FaceParameters below), or,
// where it was tessellated, as a corner attribute of that name.
// Karma's ray:hituv is the same for a quad and a strip's quads; a triangle a
// b c has (w_b, w_c) there (hit() below).
constexpr char kFaceParametersName[] = "@face";

// A triangle's code: how its barycentrics (w0, w1, w2 of its corners as the
// mesh stores them) become the face's (u, v).
//   kind Linear    the (u, v) of each stored corner, one of the four corners
//                  of the unit square (0: (0,0), 1: (1,0), 2: (1,1), 3: (0,1));
//   kind Triangle  which stored corner is a, b and c of the triangle rule.
// Three fields of two bits, corner 0 lowest, and the kind above them. 0: none
// (the barycentrics themselves).
namespace face_parameters {
constexpr uint32_t kLinear = 1u << 6, kTriangle = 2u << 6;
constexpr uint32_t linear(uint32_t c0, uint32_t c1, uint32_t c2) { return kLinear | c0 | (c1 << 2) | (c2 << 4); }
constexpr uint32_t triangle(uint32_t a, uint32_t b, uint32_t c) { return kTriangle | a | (b << 2) | (c << 4); }

NOVA_INLINE void evaluate(uint32_t code, float b1, float b2, float &u, float &v) {
    const float w[3] = {1.0f - b1 - b2, b1, b2};
    const uint32_t f[3] = {code & 3u, (code >> 2) & 3u, (code >> 4) & 3u};
    if ((code >> 6) == 1u) {
        u = v = 0.0f;
        for (int i = 0; i < 3; ++i) {
            u += f[i] == 1u || f[i] == 2u ? w[i] : 0.0f;
            v += f[i] >= 2u ? w[i] : 0.0f;
        }
    } else if ((code >> 6) == 2u && f[1] < 3u && f[2] < 3u) {
        const float wb = w[f[1]], wc = w[f[2]];
        v = wb + wc;
        u = v != 0.0f ? wb / v : 0.0f; // at a itself every u is right
    } else {
        u = b1;
        v = b2;
    }
}

// Karma's ray:hituv: a quad's parameters, a triangle's barycentrics of b and c.
NOVA_INLINE void hit(uint32_t code, float b1, float b2, float &u, float &v) {
    if ((code >> 6) == 2u && ((code >> 2) & 3u) < 3u && ((code >> 4) & 3u) < 3u) {
        const float w[3] = {1.0f - b1 - b2, b1, b2};
        u = w[(code >> 2) & 3u];
        v = w[(code >> 4) & 3u];
    } else {
        evaluate(code, b1, b2, u, v);
    }
}
} // namespace face_parameters

// An attribute resolved for one (mesh, material) pair: where a program's
// primvar reads come from. POD: the GPU gets the same view.
// Parametric: the hit's own surface parameters (b1, b2), the st of shapes
// without an st primvar (analytic spheres).
// FaceParameters: the parameters of the hit's face (above); data points at one
// code per triangle, each a byte.
enum class AttributeSource : uint8_t { None, Constant, Uniform, Vertex, Corner, Instance, Parametric, FaceParameters };
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
    case AttributeSource::FaceParameters:
        face_parameters::evaluate(reinterpret_cast<const uint8_t *>(ref.data)[prim], b1, b2, v[0], v[1]);
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
