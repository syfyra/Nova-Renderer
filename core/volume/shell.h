#pragma once

// Media bounded by surfaces (docs/design/M11-P5.md): a gprim whose volume
// shader fills its inside (Karma's uniform volumes, the Karma Fog Box). The
// shell itself is invisible; a ray finds the shell's inside from the
// shell's crossings along it, with no state (docs/design/M8.md D9: media are
// found geometrically): the camera inside a box, shadow rays and nested or
// overlapping shells all follow.
// - Closed shells (every edge shared by two triangles; spheres): inside
//   where an odd number of crossings lie ahead (the even-odd rule,
//   independent of the faces' orientation).
// - Open meshes (warned by the host): a line through a hole crosses the
//   shell an odd number of times, where parity means nothing. Their inside
//   follows the faces' orientation over the whole line: a point is inside if
//   the faces ahead of it hold more exits (crossed along their normal) than
//   entries, or the faces behind it more entries than exits. Holes then
//   behave as closed, from every direction; for closed, outward-facing
//   meshes it is the even-odd rule.
//
// The shell is a triangle mesh in the volume's object space (one motion
// step; transform motion is the volume's) with a BVH of its own, or an
// analytic sphere. Shared by the CPU and the GPU: ShellView holds pointers
// into the host's (or the device's copy of) MediumShell.

#include "core/math/common.h"
#include "core/math/vec.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace nova {

enum class ShellKind : uint8_t { None, Triangles, Sphere };

// A BVH node: a leaf holds `count` triangles from `first`; an inner node
// (count 0) has its children at `first` and `first + 1`.
struct ShellNode {
    Vec3f lower{0.0f, 0.0f, 0.0f}, upper{0.0f, 0.0f, 0.0f};
    uint32_t first = 0;
    uint32_t count = 0;
};

struct ShellView {
    ShellKind kind = ShellKind::None;
    uint8_t closed = 1;               // every edge shared by two triangles (a sphere: always)
    const Vec3f *triangles = nullptr; // three corners per triangle, in BVH order
    const ShellNode *nodes = nullptr;
    uint32_t nodeCount = 0;
    Vec3f center{0.0f, 0.0f, 0.0f}; // the sphere
    float radius = 0.0f;
};

// The crossings a ray keeps (the nearest; the rest are only counted).
constexpr int kShellCrossings = 32;
constexpr int kShellStack = 64;

namespace shell_detail {

NOVA_INLINE float axis(Vec3f v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

// Inserts crossing (t, s) into the ascending list [0, kept) of at most
// kShellCrossings.
NOVA_INLINE void keep(float *c, int8_t *sign, int &kept, float t, int8_t s) {
    int i = kept;
    if (kept < kShellCrossings) {
        ++kept;
    } else {
        if (!(t < c[kShellCrossings - 1]))
            return;
        i = kShellCrossings - 1;
    }
    while (i > 0 && c[i - 1] > t) {
        c[i] = c[i - 1];
        sign[i] = sign[i - 1];
        --i;
    }
    c[i] = t;
    sign[i] = s;
}

// The ray's watertight projection (Woop, Benthin and Wald 2013).
struct WatertightRay {
    int kx = 0, ky = 1, kz = 2;
    float sx = 0.0f, sy = 0.0f, sz = 1.0f;

    NOVA_INLINE explicit WatertightRay(Vec3f d) {
        const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
        kz = ax > ay ? (ax > az ? 0 : 2) : (ay > az ? 1 : 2);
        kx = kz == 2 ? 0 : kz + 1;
        ky = kx == 2 ? 0 : kx + 1;
        if (axis(d, kz) < 0.0f) {
            const int s = kx;
            kx = ky;
            ky = s;
        }
        sx = axis(d, kx) / axis(d, kz);
        sy = axis(d, ky) / axis(d, kz);
        sz = 1.0f / axis(d, kz);
    }

    // The line's parameter t at triangle (a, b, c) relative to origin o (any
    // sign), and the side it crosses from: `leaves` if along the triangle's
    // winding normal. False if the line misses it.
    NOVA_INLINE bool hit(Vec3f o, Vec3f a, Vec3f b, Vec3f c, float &t, bool &leaves) const {
        const Vec3f A = a - o, B = b - o, C = c - o;
        const float Ax = axis(A, kx) - sx * axis(A, kz), Ay = axis(A, ky) - sy * axis(A, kz);
        const float Bx = axis(B, kx) - sx * axis(B, kz), By = axis(B, ky) - sy * axis(B, kz);
        const float Cx = axis(C, kx) - sx * axis(C, kz), Cy = axis(C, ky) - sy * axis(C, kz);
        float U = Cx * By - Cy * Bx, V = Ax * Cy - Ay * Cx, W = Bx * Ay - By * Ax;
        if (U == 0.0f || V == 0.0f || W == 0.0f) { // on an edge in float: decide in double
            U = float(double(Cx) * double(By) - double(Cy) * double(Bx));
            V = float(double(Ax) * double(Cy) - double(Ay) * double(Cx));
            W = float(double(Bx) * double(Ay) - double(By) * double(Ax));
        }
        if ((U < 0.0f || V < 0.0f || W < 0.0f) && (U > 0.0f || V > 0.0f || W > 0.0f))
            return false;
        const float det = U + V + W;
        if (det == 0.0f)
            return false;
        // Through an edge (or a corner): one of the triangles sharing it
        // counts the crossing, as a rasterizer's fill rule decides. The edge
        // in winding order, turned to face the ray: neighbours crossed by
        // the ray traverse it in opposite directions, so exactly one owns it;
        // at a silhouette both or neither do (0 or 2 crossings: the parity
        // holds).
        const auto owns = [det](float px, float py, float qx, float qy) {
            float dx = qx - px, dy = qy - py;
            if (det < 0.0f) {
                dx = -dx;
                dy = -dy;
            }
            return dy > 0.0f || (dy == 0.0f && dx > 0.0f);
        };
        if ((U == 0.0f && !owns(Bx, By, Cx, Cy)) || (V == 0.0f && !owns(Cx, Cy, Ax, Ay)) ||
            (W == 0.0f && !owns(Ax, Ay, Bx, By)))
            return false;
        const float T = U * sz * axis(A, kz) + V * sz * axis(B, kz) + W * sz * axis(C, kz);
        t = T / det;
        // det's sign is the winding seen along the ray (the axis swap above
        // keeps it): positive when the winding normal faces the origin.
        leaves = det < 0.0f;
        return true;
    }
};

// Slab test of box [lo, hi] against the line o + t d, t in (tMin, inf).
NOVA_INLINE bool boxHit(Vec3f o, Vec3f inv, Vec3f lo, Vec3f hi, float tMin) {
    float t0 = tMin, t1 = 3.0e38f;
    const float oo[3] = {o.x, o.y, o.z}, ii[3] = {inv.x, inv.y, inv.z};
    const float l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    for (int a = 0; a < 3; ++a) {
        float ta = (l[a] - oo[a]) * ii[a], tb = (h[a] - oo[a]) * ii[a];
        if (ta > tb) {
            const float s = ta;
            ta = tb;
            tb = s;
        }
        t0 = ta > t0 ? ta : t0;
        t1 = tb < t1 ? tb : t1;
    }
    // A little slack for rays along faces (both signs of t).
    return t0 - t1 <= 4.0e-7f * (std::fabs(t0) + std::fabs(t1));
}

} // namespace shell_detail

// The crossings of the shell by the line o + t d (object space, d not
// necessarily unit) with t > tMin: the nearest kShellCrossings ascending
// in `out` with their sides in `sign` (+1 leaving along the face's normal,
// -1 entering); returns how many there are in all.
NOVA_INLINE int shellCrossings(const ShellView &s, Vec3f o, Vec3f d, float tMin, float *out, int8_t *sign) {
    int total = 0, kept = 0;
    if (s.kind == ShellKind::Sphere) {
        // |o + t d - c|^2 = r^2, the stable roots: entering, then leaving.
        const Vec3f f = o - s.center;
        const float a = dot(d, d), b = dot(f, d), c = dot(f, f) - s.radius * s.radius;
        const float disc = b * b - a * c;
        if (!(disc > 0.0f) || !(a > 0.0f))
            return 0;
        const float q = -(b + (b < 0.0f ? -std::sqrt(disc) : std::sqrt(disc)));
        float t0 = q / a, t1 = q != 0.0f ? c / q : t0;
        if (t0 > t1) {
            const float x = t0;
            t0 = t1;
            t1 = x;
        }
        if (t0 > tMin) {
            out[kept] = t0;
            sign[kept++] = -1;
            ++total;
        }
        if (t1 > tMin && t1 != t0) {
            out[kept] = t1;
            sign[kept++] = 1;
            ++total;
        }
        return total;
    }
    if (s.kind != ShellKind::Triangles || s.nodeCount == 0)
        return 0;
    const shell_detail::WatertightRay wr(d);
    // Axis-parallel rays: a huge reciprocal instead of infinity (no 0 x inf).
    const auto reciprocal = [](float x) { return 1.0f / (x != 0.0f ? x : 1.0e-30f); };
    const Vec3f inv{reciprocal(d.x), reciprocal(d.y), reciprocal(d.z)};
    uint32_t stack[kShellStack];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const ShellNode &n = s.nodes[stack[--top]];
        if (!shell_detail::boxHit(o, inv, n.lower, n.upper, tMin))
            continue;
        if (n.count == 0) {
            if (top + 2 <= kShellStack) {
                stack[top++] = n.first + 1;
                stack[top++] = n.first;
            }
            continue;
        }
        for (uint32_t i = n.first; i < n.first + n.count; ++i) {
            const Vec3f *v = s.triangles + size_t(i) * 3;
            float t;
            bool leaves;
            if (wr.hit(o, v[0], v[1], v[2], t, leaves) && t > tMin) {
                ++total;
                shell_detail::keep(out, sign, kept, t, leaves ? int8_t(1) : int8_t(-1));
            }
        }
    }
    return total;
}

// The shell's inside along the ray o + t d within [t0, t1) (0 <= t0): calls
// emit(a, b) for each interval, in order. Closed shells take the crossings
// ahead (t > 0), open ones the whole line; past the kept crossings the rest
// decide up to t1.
template <class Emit>
NOVA_INLINE void shellIntervals(const ShellView &s, Vec3f o, Vec3f d, float t0, float t1, Emit &&emit) {
    float c[kShellCrossings];
    int8_t sign[kShellCrossings];
    const bool line = !s.closed;
    if (line && s.kind == ShellKind::Triangles && s.nodeCount > 0) {
        // Open: holes close within the shell's bounds (its BVH root) only.
        const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
        const Vec3f lo = s.nodes[0].lower, hi = s.nodes[0].upper;
        const float l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
        for (int a = 0; a < 3; ++a) {
            if (dd[a] == 0.0f) {
                if (oo[a] < l[a] || oo[a] > h[a])
                    return;
                continue;
            }
            float ta = (l[a] - oo[a]) / dd[a], tb = (h[a] - oo[a]) / dd[a];
            if (ta > tb) {
                const float x = ta;
                ta = tb;
                tb = x;
            }
            t0 = ta > t0 ? ta : t0;
            t1 = tb < t1 ? tb : t1;
        }
        if (!(t0 < t1))
            return;
    }
    const int total = shellCrossings(s, o, d, line ? -3.0e38f : 0.0f, c, sign);
    const int kept = total < kShellCrossings ? total : kShellCrossings;
    // Open: the faces ahead of an interval minus the exits behind it.
    int ahead = 0, behind = 0;
    if (line)
        for (int i = 0; i < kept; ++i)
            ahead += sign[i];
    bool inside = line ? ahead > 0 : (total & 1) != 0;
    float start = -3.0e38f;
    for (int i = 0; i <= kept; ++i) {
        const float end = i < kept ? c[i] : 3.0e38f;
        if (inside) {
            const float a = start > t0 ? start : t0, b = end < t1 ? end : t1; // std::max, std::min
            if (a < b)
                emit(a, b);
        }
        if (!(end < t1) || i == kept)
            break;
        if (line) {
            ahead -= sign[i];
            behind -= sign[i]; // an entry behind counts +1
            inside = ahead > 0 || behind > 0;
        } else {
            inside = !inside;
        }
        start = end;
    }
}

// Whether distance t >= 0 along the ray lies inside the shell.
NOVA_INLINE bool shellInsideAt(const ShellView &s, Vec3f o, Vec3f d, float t) {
    bool inside = false;
    shellIntervals(s, o, d, t, t * 1.0000001f + 1e-30f, [&](float, float) { inside = true; });
    return inside;
}

// Whether object-space point p lies inside the shell (along a fixed
// direction that no mesh is likely to align with).
NOVA_INLINE bool shellContains(const ShellView &s, Vec3f p) {
    if (s.kind == ShellKind::Sphere) {
        const Vec3f f = p - s.center;
        return dot(f, f) < s.radius * s.radius;
    }
    return shellInsideAt(s, p, Vec3f{0.8017837f, 0.5345225f, 0.2672612f}, 0.0f);
}

// A shell on the host: the triangles in BVH order and the nodes, or the
// sphere, and its object-space bounds.
struct MediumShell {
    ShellKind kind = ShellKind::None;
    std::vector<Vec3f> triangles;
    std::vector<ShellNode> nodes;
    Vec3f center{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    Vec3f lower{0.0f, 0.0f, 0.0f}, upper{0.0f, 0.0f, 0.0f};
    // Edges not shared by exactly two triangles (0: closed).
    uint32_t openEdges = 0;

    ShellView view() const {
        ShellView v;
        v.kind = kind;
        v.closed = openEdges == 0 ? 1 : 0;
        v.triangles = triangles.data();
        v.nodes = nodes.data();
        v.nodeCount = uint32_t(nodes.size());
        v.center = center;
        v.radius = radius;
        return v;
    }
};
using MediumShellPtr = std::shared_ptr<const MediumShell>;

// A shell from a triangle mesh (`indices`: three per triangle into
// `points`); degenerate triangles are dropped. Null if nothing is left.
MediumShellPtr makeTriangleShell(const Vec3f *points, size_t pointCount, const uint32_t *indices,
                                 size_t triangleCount);
// A sphere shell.
MediumShellPtr makeSphereShell(Vec3f center, float radius);

} // namespace nova
