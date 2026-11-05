#include "core/integrator/shading_queries.h"

#include "core/bsdf/closure.h"
#include "core/math/frame.h"
#include "core/math/sampling.h"
#include "core/scene/ray.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nova {
namespace {

constexpr int kMaxSamples = 256;

// Sample i of a lane: the additive recurrence of the plastic constant (R3),
// shifted by the lane's random numbers, so one sample per hit is stratified
// across the pixel's samples.
Vec3f patternPoint(Vec3f rnd, int i) {
    const float x = rnd.x + float(i) * 0.81917251f, y = rnd.y + float(i) * 0.67104361f,
                z = rnd.z + float(i) * 0.54970048f;
    return {x - std::floor(x), y - std::floor(y), z - std::floor(z)};
}

int sampleCount(float samples) { return std::clamp(int(samples + 0.5f), 1, kMaxSamples); }

// The query's normal: the connected direction, else the shading normal.
Vec3f queryNormal(const QueryLane &lane, const float *d) {
    const Vec3f n{d[0], d[1], d[2]};
    const float l = length(n);
    return l > 0.0f ? n * (1.0f / l) : lane.N;
}

// The batch of one call (per thread: queries run inside the shade stage's
// parallel loop).
struct Batch {
    std::vector<Vec3f> org, dir;
    std::vector<float> tMax, time, seed, t;
    std::vector<uint8_t> type, occluded;
    std::vector<uint32_t> mesh, prim, instance;
    std::vector<Vec2f> uv;

    void clear() {
        org.clear();
        dir.clear();
        tMax.clear();
        time.clear();
        seed.clear();
    }
    // A ray from p, offset to the side of the surface (geometric normal ng)
    // it leaves.
    void push(Vec3f p, Vec3f ng, Vec3f d, float maxT, float rayTime, float filterSeed) {
        org.push_back(offsetRayOrigin(p, faceForward(ng, d)));
        dir.push_back(d);
        tMax.push_back(maxT);
        time.push_back(rayTime);
        seed.push_back(filterSeed);
    }
    // A ray leaving a query lane's hit: round curves from the tube's far side
    // when d enters it (M9 D4, as path rays), else as push().
    void push(const QueryLane &lane, Vec3f d, float maxT, float filterSeed) {
        org.push_back(lane.tube ? tubeRayOrigin(lane.P, lane.N, lane.tangent, lane.curveRadius, d, lane.curveExitEps)
                                : offsetRayOrigin(lane.P, faceForward(lane.Ng, d)));
        dir.push_back(d);
        tMax.push_back(maxT);
        time.push_back(lane.time);
        seed.push_back(filterSeed);
    }
    // An unoffset ray (rounded edges: the origin is off the surface).
    void pushRaw(Vec3f o, Vec3f d, float maxT, float rayTime, float filterSeed) {
        org.push_back(o);
        dir.push_back(d);
        tMax.push_back(maxT);
        time.push_back(rayTime);
        seed.push_back(filterSeed);
    }
    RayBatch rays(uint32_t depth, const ShadeGlobals *globals) {
        type.assign(org.size(), uint8_t(RayClass::Shadow));
        RayBatch b{org.data(), dir.data(), tMax.data(), uint32_t(org.size()), time.data(), seed.data(), type.data(),
                   depth};
        b.globals = globals;
        return b;
    }
    void intersect(const IAccel &accel, uint32_t depth, const ShadeGlobals *globals) {
        const size_t n = org.size();
        t.resize(n);
        mesh.resize(n);
        prim.resize(n);
        instance.resize(n);
        uv.resize(n);
        HitBatch hits{t.data(), mesh.data(), prim.data(), uv.data(), instance.data()};
        accel.intersect(rays(depth, globals), hits);
    }
    // Occlusion as ambient occlusion: fake-caustic objects block too.
    void occlude(const IAccel &accel, uint32_t depth, const ShadeGlobals *globals) {
        occluded.resize(org.size());
        RayBatch b = rays(depth, globals);
        b.skipFakeCaustics = false;
        accel.occluded(b, occluded.data());
    }
};

// Cosine-weighted direction in the cone of half angle a around +Z
// (sin^2 a = sin2Max): sin^2 theta is uniform in [0, sin^2 a].
Vec3f sampleCosineCone(Vec2f u, float sin2Max) {
    const float sin2 = u.x * sin2Max;
    const float s = std::sqrt(sin2), c = std::sqrt(nova::fmax(0.0f, 1.0f - sin2));
    const float phi = kTwoPi * u.y;
    return {s * std::cos(phi), s * std::sin(phi), c};
}

} // namespace

void SceneShadingQueries::run(QueryKind kind, int count, const QueryLane *lanes, const float *direction,
                              const float *params, float *out) const {
    thread_local Batch b;
    b.clear();
    const IAccel &accel = m_scene.accel();
    switch (kind) {
    case QueryKind::Occlusion: {
        int first[64], samples[64];
        for (int l = 0; l < count; ++l) {
            const QueryLane &lane = lanes[l];
            const float *pa = params + l * 4;
            const float cone = std::clamp(pa[0], 0.0f, 90.0f) * (kPi / 180.0f);
            const float maxT = pa[1];
            first[l] = int(b.org.size());
            samples[l] = maxT > 0.0f ? sampleCount(pa[2]) : 0;
            Vec3f n = queryNormal(lane, direction + l * 3);
            if (pa[3] != 0.0f)
                n = faceForward(n, -lane.I);
            const Frame f = Frame::fromNormal(n);
            const float sinA = std::sin(cone);
            for (int i = 0; i < samples[l]; ++i) {
                const Vec3f u = patternPoint(lane.rnd, i);
                b.push(lane, f.toWorld(sampleCosineCone({u.x, u.y}, sinA * sinA)), maxT, u.z);
            }
        }
        if (!b.org.empty())
            b.occlude(accel, m_depth, m_globals);
        for (int l = 0; l < count; ++l) {
            int open = 0;
            for (int i = 0; i < samples[l]; ++i)
                open += b.occluded[size_t(first[l] + i)] ? 0 : 1;
            out[l * 4] = samples[l] ? float(open) / float(samples[l]) : 1.0f;
        }
        return;
    }
    case QueryKind::Distance: {
        int first[64], samples[64];
        for (int l = 0; l < count; ++l) {
            const QueryLane &lane = lanes[l];
            const float *pa = params + l * 4;
            const float cone = std::clamp(pa[0], 0.0f, 90.0f) * (kPi / 180.0f);
            const float maxT = pa[1];
            const int flags = int(pa[3] + 0.5f);
            first[l] = int(b.org.size());
            samples[l] = maxT > 0.0f ? sampleCount(pa[2]) : 0;
            Vec3f n = queryNormal(lane, direction + l * 3);
            if (flags & 4)
                n = faceForward(n, -lane.I);
            const Frame f = Frame::fromNormal(n);
            const float coneSize = oneMinusCos(cone);
            for (int i = 0; i < samples[l]; ++i) {
                const Vec3f u = patternPoint(lane.rnd, i);
                b.push(lane, f.toWorld(sampleUniformCone({u.x, u.y}, coneSize)), maxT, u.z);
            }
        }
        if (!b.org.empty())
            b.intersect(accel, m_depth, m_globals);
        for (int l = 0; l < count; ++l) {
            const float *pa = params + l * 4;
            const float maxT = pa[1];
            const int flags = int(pa[3] + 0.5f);
            const bool mean = (flags & 1) != 0, normalized = (flags & 2) != 0;
            float nearest = maxT, sum = 0.0f;
            Vec3f nearestDir{0.0f, 0.0f, 0.0f}, dirSum{0.0f, 0.0f, 0.0f};
            int hits = 0;
            for (int i = 0; i < samples[l]; ++i) {
                const size_t r = size_t(first[l] + i);
                if (b.mesh[r] == kInvalidId)
                    continue;
                ++hits;
                sum += b.t[r];
                dirSum += b.dir[r];
                if (b.t[r] < nearest) {
                    nearest = b.t[r];
                    nearestDir = b.dir[r];
                }
            }
            float d = mean ? (hits ? sum / float(hits) : maxT) : nearest;
            Vec3f dir = mean ? (hits ? normalize(dirSum) : dirSum) : nearestDir;
            if (normalized)
                d = maxT > 0.0f ? d / maxT : 1.0f;
            out[l * 4] = samples[l] ? d : (normalized ? 1.0f : maxT);
            out[l * 4 + 1] = dir.x;
            out[l * 4 + 2] = dir.y;
            out[l * 4 + 3] = dir.z;
        }
        return;
    }
    case QueryKind::Curvature: {
        int first[64], samples[64];
        for (int l = 0; l < count; ++l) {
            const QueryLane &lane = lanes[l];
            const float *pa = params + l * 4;
            const float maxT = pa[0];
            first[l] = int(b.org.size());
            samples[l] = maxT > 0.0f ? sampleCount(pa[1]) : 0;
            const Frame f = Frame::fromNormal(queryNormal(lane, direction + l * 3));
            for (int side = 0; side < 2; ++side) // inwards (convex), outwards (concave)
                for (int i = 0; i < samples[l]; ++i) {
                    const Vec3f u = patternPoint(lane.rnd, i);
                    Vec3f d = f.toWorld(sampleUniformCone({u.x, u.y}, 1.0f));
                    if (side == 0)
                        d = -d;
                    b.push(lane, d, maxT, u.z);
                }
        }
        if (!b.org.empty())
            b.intersect(accel, m_depth, m_globals);
        for (int l = 0; l < count; ++l) {
            const float maxT = params[l * 4];
            float sum[2] = {0.0f, 0.0f};
            for (int side = 0; side < 2; ++side)
                for (int i = 0; i < samples[l]; ++i) {
                    const size_t r = size_t(first[l] + side * samples[l] + i);
                    if (b.mesh[r] != kInvalidId)
                        sum[side] += nova::fmax(0.0f, 1.0f - b.t[r] / maxT);
                }
            out[l * 4] = samples[l] ? sum[0] / float(samples[l]) : 0.0f;
            out[l * 4 + 1] = samples[l] ? sum[1] / float(samples[l]) : 0.0f;
        }
        return;
    }
    case QueryKind::RoundedEdge: {
        // Every sample traces the three axes (the normal and the tangents),
        // each through the disk of the radius perpendicular to it around P.
        int first[64], samples[64];
        for (int l = 0; l < count; ++l) {
            const QueryLane &lane = lanes[l];
            const float *pa = params + l * 4;
            const float radius = pa[0];
            first[l] = int(b.org.size());
            samples[l] = radius > 0.0f ? sampleCount(pa[1]) : 0;
            const Frame f = Frame::fromNormal(queryNormal(lane, direction + l * 3));
            for (int i = 0; i < samples[l]; ++i) {
                const Vec3f u = patternPoint(lane.rnd, i);
                const float r = radius * std::sqrt(u.x), phi = kTwoPi * u.y;
                const float x = r * std::cos(phi), y = r * std::sin(phi);
                for (int k = 0; k < 3; ++k) {
                    const Vec3f axis = k == 0 ? f.n : k == 1 ? f.t : f.b;
                    const Vec3f a = k == 0 ? f.t : k == 1 ? f.b : f.n, c = k == 0 ? f.b : k == 1 ? f.n : f.t;
                    const Vec3f q = lane.P + a * x + c * y;
                    b.pushRaw(q + axis * radius, -axis, 2.0f * radius, lane.time, u.z);
                }
            }
        }
        if (!b.org.empty())
            b.intersect(accel, m_depth, m_globals);
        for (int l = 0; l < count; ++l) {
            const QueryLane &lane = lanes[l];
            const float *pa = params + l * 4;
            const float radius = pa[0];
            const int mode = int(pa[2] + 0.5f);
            const float cusp = std::cos(std::clamp(pa[3], 0.0f, 180.0f) * (kPi / 180.0f));
            const Frame f = Frame::fromNormal(queryNormal(lane, direction + l * 3));
            Vec3f sum{0.0f, 0.0f, 0.0f};
            bool changed = false;
            for (int i = 0; i < 3 * samples[l]; ++i) {
                const size_t r = size_t(first[l] + i);
                if (b.mesh[r] != lane.mesh || b.instance[r] != lane.instance) // scope: the object itself
                    continue;
                const SurfaceHit hit = m_scene.resolveHit(b.mesh[r], b.prim[r], b.uv[r], b.t[r], b.instance[r],
                                                          lane.time, b.org[r], b.dir[r]);
                const Vec3f toHit = hit.P - lane.P;
                const float d2 = dot(toHit, toHit) / (radius * radius);
                if (d2 >= 1.0f)
                    continue;
                // Surfaces within the cusp angle, and edges of the excluded
                // kind, keep the normal. Convex: each surface lies below the
                // other's tangent plane (robust near the edge line, where
                // the hit is almost in P's plane).
                Vec3f hn = hit.N;
                if (dot(hit.N, f.n) >= cusp) {
                    hn = f.n;
                } else {
                    const bool convex = dot(hit.N - f.n, toHit) > 0.0f;
                    if ((mode == 1 && convex) || (mode == 2 && !convex))
                        hn = f.n;
                    else
                        changed = true;
                }
                // Multi-sample MIS (balance heuristic) over the three axes,
                // whose densities at the hit are |Ng . axis| / (pi r^2).
                const float pdf = std::fabs(dot(hit.Ng, f.n)) + std::fabs(dot(hit.Ng, f.t)) +
                                  std::fabs(dot(hit.Ng, f.b));
                if (pdf > 0.0f)
                    sum += hn * ((1.0f - d2) / pdf);
            }
            const float len = length(sum);
            const Vec3f n = len > 0.0f && changed ? sum * (1.0f / len) : f.n;
            out[l * 4] = n.x;
            out[l * 4 + 1] = n.y;
            out[l * 4 + 2] = n.z;
            out[l * 4 + 3] = changed ? 1.0f : 0.0f;
        }
        return;
    }
    }
}

} // namespace nova
