#pragma once

// Tangent frames in Morten Mikkelsen's MikkTSpace convention (docs/design/
// M11-P8.md), the frame bakers and game engines agree on: a tangent per
// face corner and the sign of the bitangent, B = sign * cross(N, T). Nova's
// own implementation of the published algorithm (Mikkelsen, "Simulation of
// Wrinkled Surfaces Revisited", 2008, and the reference code's rules):
// - quads are split along the diagonal that is shorter in texture space
//   (positions break ties), other polygons arrive as triangles;
// - corners are welded by identical position, normal and texture
//   coordinate;
// - per triangle, the texture-space derivative dP/ds (eq. 18), its sign the
//   orientation of the triangle's texture area;
// - at a welded vertex, the triangles around it that are connected through
//   shared edges and have the same orientation form a group; the group's
//   tangent is the corner-angle weighted sum of their derivatives, each
//   projected onto the vertex normal's plane and normalized (the reference
//   code's default angular threshold of 180 degrees, so a group is never
//   split further);
// - triangles without texture area take the tangent of another group at the
//   vertex.
// The sign follows the corners' order: a mesh whose faces were reversed
// (left-handed USD meshes turned right-handed) flips every sign, its
// tangents stay.

#include "core/math/vec.h"

#include <cstdint>
#include <vector>

namespace nova {

struct MikkFace {
    uint32_t corner[4]; // into MikkMesh's corner arrays, in face order
    uint8_t count = 3;  // 3 or 4
};

struct MikkMesh {
    std::vector<Vec3f> P;  // per corner
    std::vector<Vec3f> N;  // per corner, unit
    std::vector<Vec2f> uv; // per corner
    std::vector<MikkFace> faces;
};

// The tangent (unit, in the plane of the corner's normal) and bitangent sign
// of every corner.
struct MikkTangent {
    Vec3f t{1.0f, 0.0f, 0.0f};
    float sign = 1.0f;
};

std::vector<MikkTangent> mikkTangents(const MikkMesh &mesh);

} // namespace nova
