#pragma once

#include "core/math/vec.h"

namespace nova {

// Orthonormal basis around a unit normal (Duff et al. 2017).
struct Frame {
    Vec3f t, b, n;

    NOVA_INLINE static Frame fromNormal(Vec3f n) {
        const float sign = std::copysign(1.0f, n.z);
        const float a = -1.0f / (sign + n.z);
        const float bb = n.x * n.y * a;
        Frame f;
        f.t = {1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x};
        f.b = {bb, sign + n.y * n.y * a, -n.y};
        f.n = n;
        return f;
    }
    NOVA_INLINE Vec3f toWorld(Vec3f v) const { return t * v.x + b * v.y + n * v.z; }
    NOVA_INLINE Vec3f toLocal(Vec3f v) const { return {dot(v, t), dot(v, b), dot(v, n)}; }
};

} // namespace nova
