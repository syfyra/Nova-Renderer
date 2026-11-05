// Embree 4 backend of IAccel: one BLAS per mesh, instanced into the top-level
// scene by one RTC_GEOMETRY_TYPE_INSTANCE_ARRAY per mesh. The array's
// geometry ID is the MeshId and instPrimID the instance index, so hits map
// back without lookups. Nested Hydra instancers are flattened by the host, so
// one instancing level suffices (vcpkg's Embree has
// RTC_MAX_INSTANCE_LEVEL_COUNT = 1).

#include "core/accel/accel.h"

#include <embree4/rtcore.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

RTC_NAMESPACE_USE

namespace nova {

// Motion keys are handed to Embree as they are.
static_assert(sizeof(SrtTransform) == sizeof(RTCQuaternionDecomposition), "SrtTransform layout");
static_assert(offsetof(SrtTransform, sxy) == offsetof(RTCQuaternionDecomposition, skew_xy), "SrtTransform layout");
static_assert(offsetof(SrtTransform, px) == offsetof(RTCQuaternionDecomposition, shift_x), "SrtTransform layout");
static_assert(offsetof(SrtTransform, qw) == offsetof(RTCQuaternionDecomposition, quaternion_r), "SrtTransform layout");
static_assert(offsetof(SrtTransform, tx) == offsetof(RTCQuaternionDecomposition, translation_x), "SrtTransform layout");

namespace {

constexpr unsigned int kAllRaysMask = 0xFFFFFFFFu;

// Argument filters cost traversal time even when no geometry uses them
// (measured 5 % of the rays/s on cornell_rect), so scenes enable them only
// for filtered meshes.
RTCSceneFlags blasFlags(bool filtered) {
    return filtered ? RTC_SCENE_FLAG_ROBUST | RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS : RTC_SCENE_FLAG_ROBUST;
}
RTCSceneFlags topFlags(bool anyFiltered) {
    const RTCSceneFlags base = RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST;
    return anyFiltered ? base | RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS : base;
}

void embreeError(void *, RTCError code, const char *message) {
    std::fprintf(stderr, "[Nova] Embree error %d: %s\n", int(code), message ? message : "");
}

// Ray query context handed to the argument filter: Embree's context first,
// so the callback can cast back to it.
struct FilterContext {
    RTCRayQueryContext embree;
    const IHitFilter *filter;
    const RayBatch *rays;
    bool occlusion;
};

// Embree argument filter: runs only for geometries enabled with
// rtcSetGeometryEnableFilterFunctionFromArguments (the filtered meshes).
// N is 1 for rtcIntersect1/rtcOccluded1.
void filterHits(const RTCFilterFunctionNArguments *args) {
    const auto *ctx = reinterpret_cast<const FilterContext *>(args->context);
    for (unsigned int i = 0; i < args->N; ++i) {
        if (args->valid[i] == 0)
            continue;
        HitCandidate hit;
        hit.mesh = RTCHitN_instID(args->hit, args->N, i, 0); // instance array geomID = MeshId
        hit.instance = RTCHitN_instPrimID(args->hit, args->N, i, 0);
        hit.prim = RTCHitN_primID(args->hit, args->N, i);
        hit.uv = {RTCHitN_u(args->hit, args->N, i), RTCHitN_v(args->hit, args->N, i)};
        hit.t = RTCRayN_tfar(args->ray, args->N, i);
        hit.ray = RTCRayN_id(args->ray, args->N, i);
        hit.occlusion = ctx->occlusion;
        if (!ctx->filter->accept(hit, *ctx->rays))
            args->valid[i] = 0;
    }
}

class EmbreeAccel final : public IAccel {
    struct Blas; // below
public:
    explicit EmbreeAccel(int threads) {
        char config[32];
        std::snprintf(config, sizeof(config), "threads=%d", threads > 0 ? threads : 0);
        m_device = rtcNewDevice(config);
        if (!m_device) {
            std::fprintf(stderr, "[Nova] Embree device creation failed (error %d)\n", int(rtcGetDeviceError(nullptr)));
            return;
        }
        rtcSetDeviceErrorFunction(m_device, embreeError, nullptr);
        m_top = rtcNewScene(m_device);
        rtcSetSceneFlags(m_top, topFlags(false));
        rtcSetSceneBuildQuality(m_top, RTC_BUILD_QUALITY_LOW);
        rtcCommitScene(m_top);
    }

    ~EmbreeAccel() override {
        for (Blas &b : m_blas) {
            if (b.instances)
                rtcReleaseGeometry(b.instances);
            if (b.scene)
                rtcReleaseScene(b.scene);
        }
        if (m_top)
            rtcReleaseScene(m_top);
        if (m_device)
            rtcReleaseDevice(m_device);
    }

    void setRefit(bool refit) override { m_refit = refit; }

    // A prepared curve mesh (core/geometry/curves.h): control points with
    // their radius per deformation step, one index per segment (its first
    // control point), Embree's type for the basis and cross-section.
    RTCGeometry newCurveGeometry(const MeshDesc &desc) {
        static constexpr RTCGeometryType kTypes[4][3] = {
            // round, flat (ray-facing), normal-oriented
            {RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE, RTC_GEOMETRY_TYPE_FLAT_LINEAR_CURVE,
             RTC_GEOMETRY_TYPE_FLAT_LINEAR_CURVE}, // Embree has no oriented linear curves
            {RTC_GEOMETRY_TYPE_ROUND_BEZIER_CURVE, RTC_GEOMETRY_TYPE_FLAT_BEZIER_CURVE,
             RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_BEZIER_CURVE},
            {RTC_GEOMETRY_TYPE_ROUND_BSPLINE_CURVE, RTC_GEOMETRY_TYPE_FLAT_BSPLINE_CURVE,
             RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_BSPLINE_CURVE},
            {RTC_GEOMETRY_TYPE_ROUND_CATMULL_ROM_CURVE, RTC_GEOMETRY_TYPE_FLAT_CATMULL_ROM_CURVE,
             RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_CATMULL_ROM_CURVE},
        };
        const int basis = int(desc.curveBasis), style = int(desc.curveStyle);
        const bool oriented = desc.curveStyle == CurveStyle::Oriented && desc.curveBasis != CurveBasis::Linear &&
                              (desc.normals.size() == desc.pointCount() || desc.normalsPerStep());
        const RTCGeometryType type = kTypes[basis][oriented ? 2 : style == 2 ? 1 : style];
        RTCGeometry geom = rtcNewGeometry(m_device, type);
        const uint32_t steps = desc.pointSteps > 0 ? desc.pointSteps : 1u, n = desc.pointCount();
        const bool radiusSteps = desc.curveRadius.size() == desc.points.size() && steps > 1;
        rtcSetGeometryTimeStepCount(geom, steps);
        for (uint32_t s = 0; s < steps; ++s) {
            auto *v = static_cast<float *>(
                rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, s, RTC_FORMAT_FLOAT4, 4 * sizeof(float), n));
            for (uint32_t i = 0; i < n; ++i) {
                const Vec3f &p = desc.points[size_t(s) * n + i];
                v[4 * i] = p.x;
                v[4 * i + 1] = p.y;
                v[4 * i + 2] = p.z;
                v[4 * i + 3] = desc.curveRadius[radiusSteps ? size_t(s) * n + i : i];
            }
            if (oriented) {
                auto *nv = static_cast<float *>(
                    rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_NORMAL, s, RTC_FORMAT_FLOAT3, 3 * sizeof(float), n));
                const size_t o = desc.normalsPerStep() ? size_t(s) * n : 0;
                std::memcpy(nv, desc.normals.data() + o, size_t(n) * sizeof(Vec3f));
            }
        }
        const size_t segments = desc.curveSegments.size();
        auto *idx = static_cast<uint32_t *>(
            rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT, sizeof(uint32_t), segments));
        std::memcpy(idx, desc.curveSegments.data(), segments * sizeof(uint32_t));
        if (type == RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE) {
            // Joints between the segments of a curve: no caps inside a curve.
            auto *flags = static_cast<uint8_t *>(
                rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_FLAGS, 0, RTC_FORMAT_UCHAR, 1, segments));
            for (size_t s = 0; s < segments; ++s) {
                const uint32_t c = desc.segmentCurve[s];
                flags[s] = uint8_t((s > 0 && desc.segmentCurve[s - 1] == c ? RTC_CURVE_FLAG_NEIGHBOR_LEFT : 0) |
                                   (s + 1 < segments && desc.segmentCurve[s + 1] == c ? RTC_CURVE_FLAG_NEIGHBOR_RIGHT : 0));
            }
        }
        return geom;
    }

    // Points (docs/design/M9.md D1): one sphere or disc per point, radius per
    // point (and step); oriented discs face their normals.
    RTCGeometry newPointGeometry(const MeshDesc &desc) {
        const uint32_t steps = desc.pointSteps > 0 ? desc.pointSteps : 1u, n = desc.pointCount();
        const bool oriented = desc.pointStyle == PointStyle::OrientedDisc &&
                              (desc.normals.size() == n || desc.normalsPerStep());
        const RTCGeometryType type = oriented                                    ? RTC_GEOMETRY_TYPE_ORIENTED_DISC_POINT
                                     : desc.pointStyle == PointStyle::Disc ? RTC_GEOMETRY_TYPE_DISC_POINT
                                                                                 : RTC_GEOMETRY_TYPE_SPHERE_POINT;
        RTCGeometry geom = rtcNewGeometry(m_device, type);
        const bool radiusSteps = desc.pointRadius.size() == desc.points.size() && steps > 1;
        rtcSetGeometryTimeStepCount(geom, steps);
        for (uint32_t s = 0; s < steps; ++s) {
            auto *v = static_cast<float *>(
                rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, s, RTC_FORMAT_FLOAT4, 4 * sizeof(float), n));
            for (uint32_t i = 0; i < n; ++i) {
                const Vec3f &p = desc.points[size_t(s) * n + i];
                v[4 * i] = p.x;
                v[4 * i + 1] = p.y;
                v[4 * i + 2] = p.z;
                v[4 * i + 3] = desc.pointRadius[radiusSteps ? size_t(s) * n + i : i];
            }
            if (oriented) {
                auto *nv = static_cast<float *>(
                    rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_NORMAL, s, RTC_FORMAT_FLOAT3, 3 * sizeof(float), n));
                const size_t o = desc.normalsPerStep() ? size_t(s) * n : 0;
                std::memcpy(nv, desc.normals.data() + o, size_t(n) * sizeof(Vec3f));
            }
        }
        return geom;
    }

    // New points of the same topology: Embree refits the mesh's BVH (its    // geometry was built with RTC_BUILD_QUALITY_REFIT). Every 16th edit
    // rebuilds, so the tree's quality cannot drift too far.
    bool refit(Blas &b, const MeshDesc &desc) {
        const uint32_t steps = desc.pointSteps > 0 ? desc.pointSteps : 1u, n = desc.pointCount();
        const uint32_t tris = uint32_t(desc.indices.size() / 3);
        if (!m_refit || !b.scene || !b.refitable || desc.isSphere() || desc.isCurves() || desc.isPoints() ||
            b.refits >= 16 ||
            steps != b.vertexSteps ||
            n != b.vertexCount || tris != b.triangleCount || b.filteredAtBuild != b.filtered)
            return false;
        RTCGeometry geom = rtcGetGeometry(b.scene, 0);
        const void *idx = rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_INDEX, 0);
        if (!idx || std::memcmp(idx, desc.indices.data(), size_t(tris) * 3 * sizeof(uint32_t)) != 0)
            return false;
        for (uint32_t s = 0; s < steps; ++s) {
            void *verts = rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_VERTEX, s);
            std::memcpy(verts, desc.points.data() + size_t(s) * n, size_t(n) * sizeof(Vec3f));
            rtcUpdateGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, s);
        }
        rtcCommitGeometry(geom);
        rtcCommitScene(b.scene);
        if (b.instances)
            rtcCommitGeometry(b.instances);
        ++b.refits;
        return true;
    }

    void setMesh(MeshId id, const MeshDesc &desc) override {
        if (!m_device)
            return;
        if (id >= m_blas.size())
            m_blas.resize(size_t(id) + 1);
        Blas &b = m_blas[id];
        if (refit(b, desc))
            return;
        b.refitable = false;
        b.refits = 0;
        if (b.scene) {
            rtcReleaseScene(b.scene); // an attached instance array keeps its own reference
            b.scene = nullptr;
        }
        const bool sphere = desc.isSphere(), curves = desc.isCurves(), points = desc.isPoints();
        const bool valid = sphere   ? desc.sphereRadius.size() >= desc.points.size()
                           : curves ? !desc.curveSegments.empty() && (desc.curveRadius.size() == desc.pointCount() ||
                                                                      desc.curveRadius.size() == desc.points.size())
                           : points ? desc.pointRadius.size() == desc.pointCount() || desc.pointRadius.size() == desc.points.size()
                                    : desc.indices.size() >= 3;
        if (desc.points.empty() || !valid) {
            dropInstances(id, b);
            return;
        }

        RTCGeometry geom;
        if (curves) {
            geom = newCurveGeometry(desc);
        } else if (points) {
            geom = newPointGeometry(desc);
        } else if (sphere) {
            // One sphere point (center and radius) per deformation step.
            geom = rtcNewGeometry(m_device, RTC_GEOMETRY_TYPE_SPHERE_POINT);
            const uint32_t steps = uint32_t(desc.points.size());
            rtcSetGeometryTimeStepCount(geom, steps);
            for (uint32_t s = 0; s < steps; ++s) {
                auto *v = static_cast<float *>(rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, s,
                                                                       RTC_FORMAT_FLOAT4, 4 * sizeof(float), 1));
                v[0] = desc.points[s].x;
                v[1] = desc.points[s].y;
                v[2] = desc.points[s].z;
                v[3] = desc.sphereRadius[s];
            }
        } else {
            geom = rtcNewGeometry(m_device, RTC_GEOMETRY_TYPE_TRIANGLE);
        // One vertex buffer per deformation step (uniform over ray time
        // [0, 1]); the step count must be set first. Buffers allocated by
        // Embree carry the padding its SIMD loads need.
        const uint32_t steps = desc.pointSteps > 0 ? desc.pointSteps : 1u, n = desc.pointCount();
        rtcSetGeometryTimeStepCount(geom, steps);
        for (uint32_t s = 0; s < steps; ++s) {
            auto *verts = static_cast<float *>(rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, s,
                                                                       RTC_FORMAT_FLOAT3, 3 * sizeof(float), n));
            std::memcpy(verts, desc.points.data() + size_t(s) * n, size_t(n) * sizeof(Vec3f));
        }
        const size_t numTris = desc.indices.size() / 3;
        auto *idx = static_cast<uint32_t *>(rtcSetNewGeometryBuffer(
            geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(uint32_t), numTris));
        std::memcpy(idx, desc.indices.data(), numTris * 3 * sizeof(uint32_t));
        if (m_refit) {
            rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_REFIT);
            b.refitable = true;
            b.vertexSteps = steps;
            b.vertexCount = n;
            b.triangleCount = uint32_t(numTris);
            b.filteredAtBuild = b.filtered;
        }
        }
        rtcSetGeometryMask(geom, kAllRaysMask);
        rtcSetGeometryEnableFilterFunctionFromArguments(geom, b.filtered);
        rtcCommitGeometry(geom);

        b.scene = rtcNewScene(m_device);
        rtcSetSceneFlags(b.scene, b.refitable ? blasFlags(b.filtered) | RTC_SCENE_FLAG_DYNAMIC : blasFlags(b.filtered));
        rtcSetSceneBuildQuality(b.scene, RTC_BUILD_QUALITY_MEDIUM);
        rtcAttachGeometry(b.scene, geom);
        rtcReleaseGeometry(geom);
        rtcCommitScene(b.scene); // BLAS first, then the instance array, then the TLAS

        if (b.instances) { // keep the instances, swap the instanced scene
            rtcSetGeometryInstancedScene(b.instances, b.scene);
            rtcCommitGeometry(b.instances);
        }
    }

    void setInstances(MeshId id, const Xform34 *xforms, uint32_t count) override {
        if (id >= m_blas.size())
            return;
        Blas &b = m_blas[id];
        if (!b.scene || count == 0) {
            // Never leave Embree a pointer into a buffer the caller may free.
            dropInstances(id, b);
            return;
        }
        if (b.instances && (b.steps != 1 || b.srt))
            dropInstances(id, b); // the array changes its time steps or format
        createInstances(id, b, 1);
        if (xforms == b.data && count == b.count)
            rtcUpdateGeometryBuffer(b.instances, RTC_BUFFER_TYPE_TRANSFORM, 0); // edited in place
        else
            rtcSetSharedGeometryBuffer(b.instances, RTC_BUFFER_TYPE_TRANSFORM, 0, RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR,
                                       xforms, 0, sizeof(Xform34), count);
        b.data = xforms;
        b.count = count;
        b.srt = false;
        rtcCommitGeometry(b.instances);
    }

    void setInstanceMotion(MeshId id, const SrtTransform *keys, uint32_t count, uint32_t steps) override {
        if (id >= m_blas.size())
            return;
        Blas &b = m_blas[id];
        if (!b.scene || count == 0 || steps < 2) {
            dropInstances(id, b);
            return;
        }
        if (b.instances && (b.steps != steps || !b.srt))
            dropInstances(id, b);
        createInstances(id, b, steps);
        // One shared key buffer per time step; Embree interpolates the
        // decomposed transforms (lerp of scale and translation, slerp of the
        // rotation), which Scene::resolveHit reproduces.
        for (uint32_t s = 0; s < steps; ++s) {
            const SrtTransform *slot = keys + size_t(s) * count;
            if (keys == b.data && count == b.count)
                rtcUpdateGeometryBuffer(b.instances, RTC_BUFFER_TYPE_TRANSFORM, s);
            else
                rtcSetSharedGeometryBuffer(b.instances, RTC_BUFFER_TYPE_TRANSFORM, s, RTC_FORMAT_QUATERNION_DECOMPOSITION,
                                           slot, 0, sizeof(SrtTransform), count);
        }
        b.data = keys;
        b.count = count;
        b.srt = true;
        rtcCommitGeometry(b.instances);
    }

    void setEnabled(MeshId id, bool enabled) override {
        if (id >= m_blas.size())
            m_blas.resize(size_t(id) + 1);
        Blas &b = m_blas[id];
        if (b.enabled == enabled)
            return;
        b.enabled = enabled;
        if (b.instances)
            enabled ? rtcEnableGeometry(b.instances) : rtcDisableGeometry(b.instances);
    }

    void setMeshFiltered(MeshId id, bool filtered) override {
        if (id >= m_blas.size())
            m_blas.resize(size_t(id) + 1);
        Blas &b = m_blas[id];
        if (b.filtered == filtered)
            return;
        b.filtered = filtered;
        if (!b.scene)
            return; // applied by the next setMesh()
        // The flag lives on the triangle geometry; committing the BLAS again
        // refits nothing else (the vertices are unchanged).
        RTCGeometry geom = rtcGetGeometry(b.scene, 0);
        rtcSetGeometryEnableFilterFunctionFromArguments(geom, filtered);
        rtcCommitGeometry(geom);
        rtcSetSceneFlags(b.scene, blasFlags(filtered));
        rtcCommitScene(b.scene);
        if (b.instances)
            rtcCommitGeometry(b.instances);
    }

    void setHitFilter(const IHitFilter *filter) override { m_filter = filter; }

    void removeMesh(MeshId id) override {
        if (id >= m_blas.size())
            return;
        Blas &b = m_blas[id];
        dropInstances(id, b);
        if (b.scene)
            rtcReleaseScene(b.scene);
        b = Blas{}; // also clears the filter flag: a reused id starts unfiltered
    }

    void commit() override {
        if (!m_top)
            return;
        // The TLAS enables argument filters only while a mesh uses them (a
        // filtered mesh's change also modifies its instance array, so the
        // TLAS rebuilds with the new flags).
        bool anyFiltered = false;
        for (const Blas &b : m_blas)
            anyFiltered |= b.filtered && b.scene != nullptr;
        rtcSetSceneFlags(m_top, topFlags(anyFiltered));
        rtcCommitScene(m_top);
    }

    void intersect(const RayBatch &rays, HitBatch &hits) const override {
        if (!m_top) {
            for (uint32_t i = 0; i < rays.count; ++i)
                hits.meshId[i] = kInvalidId;
            return;
        }
        FilterContext ctx{{}, m_filter, &rays, false};
        rtcInitRayQueryContext(&ctx.embree);
        RTCIntersectArguments args;
        rtcInitIntersectArguments(&args);
        if (m_filter) {
            args.context = &ctx.embree;
            args.filter = filterHits;
        }
        for (uint32_t i = 0; i < rays.count; ++i) {
            RTCRayHit rh;
            rh.ray.org_x = rays.org[i].x;
            rh.ray.org_y = rays.org[i].y;
            rh.ray.org_z = rays.org[i].z;
            rh.ray.tnear = 0.0f;
            rh.ray.dir_x = rays.dir[i].x;
            rh.ray.dir_y = rays.dir[i].y;
            rh.ray.dir_z = rays.dir[i].z;
            rh.ray.time = rays.time ? rays.time[i] : 0.0f;
            rh.ray.tfar = rays.tMax[i];
            rh.ray.mask = kAllRaysMask;
            rh.ray.id = i;
            rh.ray.flags = 0;
            rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
            rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
            rh.hit.instPrimID[0] = RTC_INVALID_GEOMETRY_ID;

            rtcIntersect1(m_top, &rh, &args);

            if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
                hits.meshId[i] = kInvalidId;
                continue;
            }
            hits.t[i] = rh.ray.tfar;
            hits.meshId[i] = rh.hit.instID[0]; // geometry ID of the instance array = MeshId
            if (hits.instance)
                hits.instance[i] = rh.hit.instPrimID[0]; // which instance of that mesh
            hits.primId[i] = rh.hit.primID;
            hits.uv[i] = {rh.hit.u, rh.hit.v};
        }
    }

    bool intersectLocal(MeshId id, Vec3f org, Vec3f dir, float tMin, float tMax, float time, float &t, uint32_t &prim,
                        Vec2f &uv) const override {
        if (id >= m_blas.size() || !m_blas[id].scene)
            return false;
        RTCIntersectArguments args;
        rtcInitIntersectArguments(&args); // no filter: cut-outs do not stop a walk
        RTCRayHit rh;
        rh.ray.org_x = org.x;
        rh.ray.org_y = org.y;
        rh.ray.org_z = org.z;
        rh.ray.tnear = tMin;
        rh.ray.dir_x = dir.x;
        rh.ray.dir_y = dir.y;
        rh.ray.dir_z = dir.z;
        rh.ray.time = time;
        rh.ray.tfar = tMax;
        rh.ray.mask = kAllRaysMask;
        rh.ray.id = 0;
        rh.ray.flags = 0;
        rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
        rtcIntersect1(m_blas[id].scene, &rh, &args);
        if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID)
            return false;
        t = rh.ray.tfar;
        prim = rh.hit.primID;
        uv = {rh.hit.u, rh.hit.v};
        return true;
    }

    void occluded(const RayBatch &rays, uint8_t *occludedOut) const override {
        if (!m_top) {
            std::memset(occludedOut, 0, rays.count);
            return;
        }
        FilterContext ctx{{}, m_filter, &rays, true};
        rtcInitRayQueryContext(&ctx.embree);
        RTCOccludedArguments args;
        rtcInitOccludedArguments(&args);
        if (m_filter) {
            args.context = &ctx.embree;
            args.filter = filterHits;
        }
        for (uint32_t i = 0; i < rays.count; ++i) {
            RTCRay r;
            r.org_x = rays.org[i].x;
            r.org_y = rays.org[i].y;
            r.org_z = rays.org[i].z;
            r.tnear = 0.0f;
            r.dir_x = rays.dir[i].x;
            r.dir_y = rays.dir[i].y;
            r.dir_z = rays.dir[i].z;
            r.time = rays.time ? rays.time[i] : 0.0f;
            r.tfar = rays.tMax[i];
            r.mask = kAllRaysMask;
            r.id = i;
            r.flags = 0;
            rtcOccluded1(m_top, &r, &args);
            occludedOut[i] = r.tfar < 0.0f ? 1 : 0; // Embree sets tfar = -inf on a hit
        }
    }

    void bounds(Vec3f &lower, Vec3f &upper) const override {
        lower = {kInf, kInf, kInf};
        upper = {-kInf, -kInf, -kInf};
        if (!m_top)
            return;
        RTCBounds b;
        rtcGetSceneBounds(m_top, &b); // covers every instance of every array
        lower = {b.lower_x, b.lower_y, b.lower_z};
        upper = {b.upper_x, b.upper_y, b.upper_z};
    }

private:
    struct Blas {
        RTCScene scene = nullptr;        // object-space triangles
        RTCGeometry instances = nullptr; // instance array in m_top, geomID = MeshId
        const void *data = nullptr;      // transforms or motion keys, shared with Embree
        uint32_t count = 0;
        uint32_t steps = 1;              // time steps of the instance array
        bool srt = false;                // keys are decomposed (motion)
        bool enabled = true;
        bool filtered = false;           // hits go through the hit filter
        // Refit (D7): built for refits, how often refitted, its topology.
        bool refitable = false;
        bool filteredAtBuild = false;
        uint32_t refits = 0;
        uint32_t vertexSteps = 0, vertexCount = 0, triangleCount = 0;
    };

    void createInstances(MeshId id, Blas &b, uint32_t steps) {
        if (b.instances)
            return;
        b.instances = rtcNewGeometry(m_device, RTC_GEOMETRY_TYPE_INSTANCE_ARRAY);
        rtcSetGeometryTimeStepCount(b.instances, steps);
        rtcSetGeometryInstancedScene(b.instances, b.scene);
        rtcSetGeometryMask(b.instances, kAllRaysMask);
        rtcAttachGeometryByID(m_top, b.instances, id);
        if (!b.enabled)
            rtcDisableGeometry(b.instances);
        b.steps = steps;
    }

    void dropInstances(MeshId id, Blas &b) {
        if (b.instances) {
            rtcDetachGeometry(m_top, id);
            rtcReleaseGeometry(b.instances);
            b.instances = nullptr;
        }
        b.data = nullptr;
        b.count = 0;
        b.steps = 1;
        b.srt = false;
    }

    RTCDevice m_device = nullptr;
    RTCScene m_top = nullptr;
    const IHitFilter *m_filter = nullptr;
    std::vector<Blas> m_blas; // indexed by MeshId
    bool m_refit = false;      // refit deformed meshes (D7)
};

} // namespace

std::unique_ptr<IAccel> createEmbreeAccel(int threads) { return std::make_unique<EmbreeAccel>(threads); }

} // namespace nova
