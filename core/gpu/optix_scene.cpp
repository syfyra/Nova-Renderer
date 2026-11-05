#include "core/gpu/optix_scene.h"

#include "core/geometry/curves.h"
#include "core/gpu/embedded.h"
#include "core/math/frame.h"
#include "core/scene/scene.h"

#include <optix.h>
#include <optix_stack_size.h>
#include <optix_stubs.h>

#include <algorithm>
#include <cstring>

namespace nova::gpu {

namespace {

constexpr size_t kRecord = OPTIX_SBT_RECORD_HEADER_SIZE; // no record data
constexpr unsigned kGeometryFlags = OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;
constexpr unsigned kBuildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_COMPACTION;

// The static pipeline traces one instancing level; the motion pipeline also
// the SRT motion transforms between the IAS and the GASes. Both trace single
// GASes too (subsurface walks), whose custom intersectors find the mesh in
// the one payload value.
OptixPipelineCompileOptions pipelineOptions(bool motion) {
    OptixPipelineCompileOptions p{};
    p.usesMotionBlur = motion ? 1 : 0;
    p.traversableGraphFlags = motion ? OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY
                                     : OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING |
                                           OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    p.numPayloadValues = 1;
    p.numAttributeValues = 2;
    p.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    p.pipelineLaunchParamsVariableName = "params";
    p.pipelineLaunchParamsSizeInBytes = sizeof(TraceParams);
    p.usesPrimitiveTypeFlags =
        unsigned(OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE | OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_LINEAR |
                 OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_CUBIC_BSPLINE | OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_CATMULLROM |
                 OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_CUBIC_BEZIER | OPTIX_PRIMITIVE_TYPE_FLAGS_SPHERE |
                 OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM);
    return p;
}

// Curves Embree renders as normal-oriented ribbons (its condition): a cubic
// basis with a normal per point (and step). Other oriented curves are flat.
bool isRibbon(const MeshDesc &m) {
    return m.shape == MeshShape::Curves && m.curveStyle == CurveStyle::Oriented && m.curveBasis != CurveBasis::Linear &&
           (m.normals.size() == m.pointCount() || m.normalsPerStep());
}

// Normal-oriented ribbons as triangles (D5): per segment quads quads over
// [u_q, u_q+1], spanned across the authored normal (interpolated on the
// basis, as Embree's) to half-width the radius; one set of points per
// motion step, step-major. Triangle 2q of a segment is (left u_q, right u_q,
// right u_q+1), 2q + 1 (left u_q, right u_q+1, left u_q+1): programs.cu's
// ribbonHit maps a hit back.
void tessellateRibbons(const MeshDesc &m, uint32_t quads, std::vector<Vec3f> &points, std::vector<uint32_t> &indices) {
    const uint32_t steps = m.pointSteps > 1 ? m.pointSteps : 1u;
    const size_t segments = m.curveSegments.size(), n = m.pointCount(), perSegment = 2 * size_t(quads + 1);
    points.resize(steps * segments * perSegment);
    for (uint32_t s = 0; s < steps; ++s) {
        // Step s as the curve evaluates it: from s0 by f (the last from its predecessor).
        const size_t s0 = steps > 1 ? std::min<size_t>(s, steps - 2) : 0;
        const float f = float(s) - float(s0);
        for (size_t seg = 0; seg < segments; ++seg) {
            const uint32_t start = m.curveSegments[seg];
            Vec3f *out = &points[(s * segments + seg) * perSegment];
            for (uint32_t j = 0; j <= quads; ++j) {
                const float u = float(j) / float(quads);
                const CurvePoint c = evaluateCurve(m, uint32_t(seg), u, s0, f);
                const CurveWeights wt = curveWeights(m.curveBasis, u);
                Vec3f no{0.0f, 0.0f, 0.0f};
                for (int i = 0; i < 4; ++i) {
                    const size_t v = size_t(start) + size_t(i);
                    no += (m.normalsPerStep() ? m.normals[s * n + v] : m.normals[v]) * wt.w[i];
                }
                const Vec3f T = dot(c.tangent, c.tangent) > 0.0f ? normalize(c.tangent) : Vec3f{0.0f, 0.0f, 1.0f};
                Vec3f b = cross(T, no);
                if (!(dot(b, b) > 0.0f) || !isFinite(b))
                    b = Frame::fromNormal(T).t;
                b = normalize(b) * c.radius;
                out[2 * j] = c.center - b;
                out[2 * j + 1] = c.center + b;
            }
        }
    }
    indices.clear();
    indices.reserve(segments * quads * 6);
    for (size_t seg = 0; seg < segments; ++seg)
        for (uint32_t q = 0; q < quads; ++q) {
            const uint32_t l0 = uint32_t(seg * perSegment + 2 * q), r0 = l0 + 1, l1 = l0 + 2, r1 = l0 + 3;
            indices.insert(indices.end(), {l0, r0, r1, l0, r1, l1});
        }
}

// The hit group of a mesh: by shape, curve basis and point style.
OptixScene::Kind kindOf(const MeshDesc &m) {
    switch (m.shape) {
    case MeshShape::Curves:
        if (isRibbon(m))
            return OptixScene::Triangles;        switch (m.curveBasis) { // ray-facing ribbons: the round tube of the basis (D5)
        case CurveBasis::Linear:
            return OptixScene::Linear;
        case CurveBasis::BSpline:
            return OptixScene::BSpline;
        case CurveBasis::CatmullRom:
            return OptixScene::CatmullRom;
        default:
            return OptixScene::Bezier;
        }
    case MeshShape::Points:
        return m.pointStyle == PointStyle::OrientedDisc ? OptixScene::Disc
               : m.pointStyle == PointStyle::Disc       ? OptixScene::BuiltinSphere
                                                        : OptixScene::Sphere;
    case MeshShape::Sphere:
        return OptixScene::Sphere;
    default:
        return OptixScene::Triangles;
    }
}

// Nova's SRT key as OptiX's (the same fields, another order).
OptixSRTData srtData(const SrtTransform &s) {
    OptixSRTData d{};
    d.sx = s.sx;
    d.a = s.sxy;
    d.b = s.sxz;
    d.pvx = s.px;
    d.sy = s.sy;
    d.c = s.syz;
    d.pvy = s.py;
    d.sz = s.sz;
    d.pvz = s.pz;
    d.qx = s.qx;
    d.qy = s.qy;
    d.qz = s.qz;
    d.qw = s.qw;
    d.tx = s.tx;
    d.ty = s.ty;
    d.tz = s.tz;
    return d;
}

} // namespace

bool OptixScene::supports(const MeshRecord &rec) {
    const MeshDesc &m = rec.desc;
    if (!rec.alive || !m.visible || rec.instanceCount() == 0)
        return false;
    switch (m.shape) {
    case MeshShape::Triangles:
        return m.triangleCount() > 0;
    case MeshShape::Curves:
        return !m.curveSegments.empty();
    case MeshShape::Points:
        return m.pointCount() > 0 && !m.pointRadius.empty() &&
               (m.pointStyle != PointStyle::OrientedDisc || m.normals.size() >= m.pointCount());
    case MeshShape::Sphere:
        return !m.points.empty() && !m.sphereRadius.empty();
    }
    return false;
}

std::vector<SrtTransform> OptixScene::continuousKeys(const MeshRecord &rec) {
    std::vector<SrtTransform> keys = rec.motionKeys;
    const size_t count = rec.instanceCount();
    for (uint32_t s = 1; s < rec.motionSteps; ++s)
        for (size_t i = 0; i < count; ++i) {
            const SrtTransform &p = keys[size_t(s - 1) * count + i];
            SrtTransform &q = keys[size_t(s) * count + i];
            if (p.qw * q.qw + p.qx * q.qx + p.qy * q.qy + p.qz * q.qz < 0.0f) {
                q.qw = -q.qw;
                q.qx = -q.qx;
                q.qy = -q.qy;
                q.qz = -q.qz;
            }
        }
    return keys;
}

OptixScene::~OptixScene() {
    Device::Scope scope(m_device);
    for (Pipeline &p : m_pipelines)
        destroyPipeline(p);
}

void OptixScene::destroyPipeline(Pipeline &p) {
    if (p.pipeline)
        optixPipelineDestroy(p.pipeline);
    for (OptixProgramGroup g : p.groups)
        if (g)
            optixProgramGroupDestroy(g);
    if (p.module)
        optixModuleDestroy(p.module);
    for (OptixModule mod : p.builtin)
        if (mod)
            optixModuleDestroy(mod);
    p = Pipeline{};
}

bool OptixScene::init(std::string &error) {
    Device::Scope scope(m_device);
    return createPipeline(m_pipelines[0], false, error) &&
           (m_params.reserve(m_device, sizeof(TraceParams)) || (error = m_device.lastError(), false));
}

bool OptixScene::createPipeline(Pipeline &p, bool motion, std::string &error) {
    OptixModuleCompileOptions mco{};
    mco.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    mco.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
    const OptixPipelineCompileOptions pco = pipelineOptions(motion);
    char log[2048];
    size_t logSize = sizeof(log);
    const auto fail = [&](bool withLog) {
        error = m_device.lastError() + (withLog ? std::string(": ") + log : std::string());
        destroyPipeline(p);
        return false;
    };
    if (!m_device.check(optixModuleCreate(m_device.optix(), &mco, &pco, reinterpret_cast<const char *>(kNovaProgramsIr_words),
                                          kNovaProgramsIr_size, log, &logSize, &p.module),
                        "optixModuleCreate"))
        return fail(true);
    // The built-in intersectors of the curves and spheres.
    const OptixPrimitiveType builtins[5] = {OPTIX_PRIMITIVE_TYPE_ROUND_LINEAR, OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE,
                                            OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM,
                                            OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BEZIER, OPTIX_PRIMITIVE_TYPE_SPHERE};
    for (int i = 0; i < 5; ++i) {
        OptixBuiltinISOptions bo{};
        bo.builtinISModuleType = builtins[i];
        bo.usesMotionBlur = motion ? 1 : 0;
        bo.buildFlags = kBuildFlags;
        bo.curveEndcapFlags = OPTIX_CURVE_ENDCAP_DEFAULT;
        if (!m_device.check(optixBuiltinISModuleGet(m_device.optix(), &mco, &pco, &bo, &p.builtin[i]),
                            "optixBuiltinISModuleGet"))
            return fail(false);
    }
    constexpr uint32_t kGroups = 4 + 2 * kKinds;
    OptixProgramGroupDesc desc[kGroups] = {};
    desc[0].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    desc[0].raygen.module = p.module;
    desc[0].raygen.entryFunctionName = "__raygen__closest";
    desc[1].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    desc[1].raygen.module = p.module;
    desc[1].raygen.entryFunctionName = "__raygen__occluded";
    desc[2].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    desc[2].raygen.module = p.module;
    desc[2].raygen.entryFunctionName = "__raygen__walk";
    desc[3].kind = OPTIX_PROGRAM_GROUP_KIND_MISS; // no program: hit objects report misses
    // Hit groups: intersection only (hit objects), the filtered ones with the
    // any-hit filter as well.
    for (uint32_t g = 0; g < 2 * kKinds; ++g) {
        const uint32_t k = g % kKinds;
        OptixProgramGroupDesc &d = desc[4 + g];
        d.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        if (k >= Linear && k <= BuiltinSphere) {
            d.hitgroup.moduleIS = p.builtin[k - Linear];
        } else if (k == Sphere || k == Disc) {
            d.hitgroup.moduleIS = p.module;
            d.hitgroup.entryFunctionNameIS = k == Sphere ? "__intersection__sphere" : "__intersection__disc";
        }
        if (g >= kKinds) {
            d.hitgroup.moduleAH = p.module;
            d.hitgroup.entryFunctionNameAH = "__anyhit__filter";
        }
    }
    OptixProgramGroupOptions pgo{};
    logSize = sizeof(log);
    if (!m_device.check(optixProgramGroupCreate(m_device.optix(), desc, kGroups, &pgo, log, &logSize, p.groups),
                        "optixProgramGroupCreate"))
        return fail(true);
    OptixPipelineLinkOptions plo{};
    plo.maxTraceDepth = 1;
    logSize = sizeof(log);
    if (!m_device.check(
            optixPipelineCreate(m_device.optix(), &pco, &plo, p.groups, kGroups, log, &logSize, &p.pipeline),
            "optixPipelineCreate"))
        return fail(true);
    OptixStackSizes ss{};
    for (OptixProgramGroup g : p.groups)
        if (!m_device.check(optixUtilAccumulateStackSizes(g, &ss, p.pipeline), "optixUtilAccumulateStackSizes"))
            return fail(false);
    unsigned traversal = 0, state = 0, continuation = 0;
    // Graph depth: IAS -> GAS, or IAS -> SRT motion transform -> GAS.
    if (!m_device.check(optixUtilComputeStackSizes(&ss, 1, 0, 0, &traversal, &state, &continuation),
                        "optixUtilComputeStackSizes") ||
        !m_device.check(optixPipelineSetStackSize(p.pipeline, traversal, state, continuation, motion ? 3 : 2),
                        "optixPipelineSetStackSize"))
        return fail(false);
    // Records: the three raygens, the miss, the hit groups.
    std::vector<char> records(kGroups * kRecord, 0);
    for (uint32_t i = 0; i < kGroups; ++i)
        if (!m_device.check(optixSbtRecordPackHeader(p.groups[i], records.data() + size_t(i) * kRecord),
                            "optixSbtRecordPackHeader"))
            return fail(false);
    if (!m_device.upload(p.sbt, records))
        return fail(false);
    p.table.raygenRecord = p.sbt.ptr();
    p.table.missRecordBase = p.sbt.ptr() + 3 * kRecord;
    p.table.missRecordStrideInBytes = unsigned(kRecord);
    p.table.missRecordCount = 1;
    p.table.hitgroupRecordBase = p.sbt.ptr() + 4 * kRecord;
    p.table.hitgroupRecordStrideInBytes = unsigned(kRecord);
    p.table.hitgroupRecordCount = 2 * kKinds;
    return true;
}

bool OptixScene::buildGas(Gas &gas, const OptixBuildInput &in, uint32_t motionKeys) {
    OptixAccelBuildOptions o{};
    o.buildFlags = kBuildFlags;
    o.operation = OPTIX_BUILD_OPERATION_BUILD;
    if (motionKeys > 1) { // deformation: keys uniform over the normalized shutter
        o.motionOptions.numKeys = uint16_t(motionKeys);
        o.motionOptions.timeBegin = 0.0f;
        o.motionOptions.timeEnd = 1.0f;
        o.motionOptions.flags = OPTIX_MOTION_FLAG_NONE;
    }
    OptixAccelBufferSizes sizes{};
    if (!m_device.check(optixAccelComputeMemoryUsage(m_device.optix(), &o, &in, 1, &sizes),
                        "optixAccelComputeMemoryUsage"))
        return false;
    // The compacted size goes to 8 bytes after the (aligned) temporary space.
    const size_t tempBytes = (sizes.tempSizeInBytes + 7) & ~size_t(7);
    DeviceBuffer out, compacted;
    if (!m_temp.reserve(m_device, tempBytes + 8) || !out.reserve(m_device, sizes.outputSizeInBytes))
        return false;
    const CUdeviceptr sizeSlot = m_temp.ptr() + tempBytes;
    OptixAccelEmitDesc emit{sizeSlot, OPTIX_PROPERTY_TYPE_COMPACTED_SIZE};
    OptixTraversableHandle handle = 0;
    if (!m_device.check(optixAccelBuild(m_device.optix(), m_device.stream(), &o, &in, 1, m_temp.ptr(),
                                        sizes.tempSizeInBytes, out.ptr(), sizes.outputSizeInBytes, &handle, &emit, 1),
                        "optixAccelBuild"))
        return false;
    uint64_t compactSize = 0;
    if (!m_device.download(&compactSize, sizeSlot, sizeof(compactSize)))
        return false;
    if (compactSize > 0 && compactSize < sizes.outputSizeInBytes) {
        if (!compacted.reserve(m_device, compactSize) ||
            !m_device.check(optixAccelCompact(m_device.optix(), m_device.stream(), handle, compacted.ptr(), compactSize,
                                              &handle),
                            "optixAccelCompact") ||
            !m_device.sync())
            return false;
        gas.buffer = std::move(compacted);
    } else {
        gas.buffer = std::move(out);
    }
    gas.handle = handle;
    return m_device.sync(); // the input buffers go out of scope
}

bool OptixScene::build(const Scene &scene, uint32_t &skipped) {
    Device::Scope scope(m_device);
    skipped = 0;
    m_gas.resize(scene.meshCount());
    m_transforms.clear();
    std::vector<OptixInstance> instances;
    std::vector<uint32_t> first(scene.meshCount() + 1, 0);
    std::vector<ShapeData> shapes(scene.meshCount());
    std::vector<MeshGas> meshGas(scene.meshCount());
    std::vector<OptixAabb> boxes;
    bool motion = false;
    for (MeshId id = 0; id < scene.meshCount(); ++id) {
        const MeshRecord &rec = scene.meshRecord(id);
        first[id] = uint32_t(instances.size());
        Gas &gas = m_gas[id];
        if (!rec.alive || !rec.desc.visible || rec.instanceCount() == 0) {
            gas = Gas{};
            continue;
        }
        if (!supports(rec)) {
            gas = Gas{};
            ++skipped;
            continue;
        }
        const MeshDesc &m = rec.desc;
        const uint32_t steps = m.pointSteps > 1 ? m.pointSteps : 1u, n = m.pointCount();
        motion |= steps > 1 || rec.motionSteps > 1;
        // Filtered meshes (opacity, fake caustics, shadow linking): the
        // any-hit filter runs on them.
        const bool filtered = rec.filtered;
        // The GAS of the same geometry and filtering is kept (IPR, D14).
        if (!gas.handle || gas.version != rec.geometryVersion || gas.filtered != filtered) {
            gas = Gas{};
            gas.kind = kindOf(m);
            gas.version = rec.geometryVersion;
            gas.filtered = filtered;
            const auto put = [&](const auto *data, size_t count) -> CUdeviceptr {
                gas.inputs.emplace_back();
                return m_device.upload(gas.inputs.back(), data, count) ? gas.inputs.back().ptr() : 0;
            };
            OptixBuildInput in{};
            const unsigned flags[1] = {filtered ? unsigned(OPTIX_GEOMETRY_FLAG_NONE) : kGeometryFlags};
            // Normal-oriented ribbons: their triangles, 8 quads per segment (4 for
            // large meshes).
            std::vector<Vec3f> ribbonPoints;
            std::vector<uint32_t> ribbonIndices;
            const bool ribbon = isRibbon(m);
            if (ribbon) {
                gas.shape.ribbonQuads = m.curveSegments.size() <= (1u << 18) ? 8u : 4u;
                tessellateRibbons(m, gas.shape.ribbonQuads, ribbonPoints, ribbonIndices);
            }
            const std::vector<Vec3f> &P = ribbon ? ribbonPoints : m.points;
            const uint32_t vertices = ribbon ? uint32_t(ribbonPoints.size() / steps) : n;
            // Per motion key: the points, radii and boxes (the build input points
            // at these arrays of device pointers).
            const CUdeviceptr pointBase = put(P.data(), P.size());
            CUdeviceptr points[kMaxMotionSteps] = {}, widths[kMaxMotionSteps] = {}, aabbs[kMaxMotionSteps] = {};
            for (uint32_t s = 0; s < steps; ++s)
                points[s] = pointBase + size_t(s) * vertices * sizeof(Vec3f);
            if (gas.kind == Triangles) {
                in.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
                OptixBuildInputTriangleArray &t = in.triangleArray;
                t.vertexBuffers = points;
                t.numVertices = vertices;
                t.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
                t.vertexStrideInBytes = sizeof(Vec3f);
                const std::vector<uint32_t> &indices = ribbon ? ribbonIndices : m.indices;
                t.indexBuffer = put(indices.data(), indices.size());
                t.numIndexTriplets = unsigned(indices.size() / 3);
                t.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
                t.flags = flags;
                t.numSbtRecords = 1;
            } else if (gas.kind <= Bezier) {
                static const OptixPrimitiveType types[4] = {
                    OPTIX_PRIMITIVE_TYPE_ROUND_LINEAR, OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE,
                    OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM, OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BEZIER};
                // A radius per control point and step (fewer: repeated).
                std::vector<float> radii(size_t(n) * steps);
                const size_t given = m.curveRadius.size();
                for (size_t i = 0; i < radii.size(); ++i)
                    radii[i] = given == 0 ? 0.0f : given >= radii.size() ? m.curveRadius[i] : m.curveRadius[i % given];
                const CUdeviceptr widthBase = put(radii.data(), radii.size());
                for (uint32_t s = 0; s < steps; ++s)
                    widths[s] = widthBase + size_t(s) * n * sizeof(float);
                in.type = OPTIX_BUILD_INPUT_TYPE_CURVES;
                OptixBuildInputCurveArray &c = in.curveArray;
                c.curveType = types[gas.kind - Linear];
                c.numPrimitives = unsigned(m.curveSegments.size());
                c.vertexBuffers = points;
                c.numVertices = n;
                c.vertexStrideInBytes = sizeof(Vec3f);
                c.widthBuffers = widths;
                c.widthStrideInBytes = sizeof(float);
                c.indexBuffer = put(m.curveSegments.data(), m.curveSegments.size());
                c.indexStrideInBytes = sizeof(uint32_t);
                c.flag = flags[0];
                c.endcapFlags = OPTIX_CURVE_ENDCAP_DEFAULT;
            } else if (gas.kind == BuiltinSphere) {
                const bool single = m.pointRadius.size() == 1;
                std::vector<float> radii(single ? 1 : size_t(n) * steps);
                for (size_t i = 0; i < radii.size(); ++i)
                    radii[i] = m.pointRadius[m.pointRadius.size() >= radii.size() ? i : i % m.pointRadius.size()];
                const CUdeviceptr radiusBase = put(radii.data(), radii.size());
                for (uint32_t s = 0; s < steps; ++s)
                    widths[s] = single ? radiusBase : radiusBase + size_t(s) * n * sizeof(float);
                in.type = OPTIX_BUILD_INPUT_TYPE_SPHERES;
                OptixBuildInputSphereArray &s = in.sphereArray;
                s.vertexBuffers = points;
                s.numVertices = n;
                s.vertexStrideInBytes = sizeof(Vec3f);
                s.radiusBuffers = widths;
                s.radiusStrideInBytes = sizeof(float);
                s.singleRadius = single ? 1 : 0;
                s.flags = flags;
                s.numSbtRecords = 1;
            } else { // custom spheres and discs: their boxes per key, their data for the programs
                const bool sphere = m.shape == MeshShape::Sphere;
                const std::vector<float> &radii = sphere ? m.sphereRadius : m.pointRadius;
                const uint32_t count = sphere ? 1u : n;
                const auto radiusAt = [&](uint32_t s, uint32_t i) {
                    return radii.size() == 1                       ? radii[0]
                           : radii.size() >= size_t(count) * steps ? radii[size_t(s) * count + i]
                                                                   : radii[i];
                };
                boxes.resize(size_t(count) * steps);
                for (uint32_t s = 0; s < steps; ++s)
                    for (uint32_t i = 0; i < count; ++i) {
                        const Vec3f c = m.points[size_t(s) * count + i];
                        const float r = radiusAt(s, i);
                        boxes[size_t(s) * count + i] = {c.x - r, c.y - r, c.z - r, c.x + r, c.y + r, c.z + r};
                    }
                const CUdeviceptr boxBase = put(boxes.data(), boxes.size());
                for (uint32_t s = 0; s < steps; ++s)
                    aabbs[s] = boxBase + size_t(s) * count * sizeof(OptixAabb);
                ShapeData &d = gas.shape;
                d.centers = reinterpret_cast<const Vec3f *>(pointBase);
                d.radii = reinterpret_cast<const float *>(put(radii.data(), radii.size()));
                d.radiusCount = uint32_t(radii.size());
                d.count = count;
                d.steps = steps;
                if (gas.kind == Disc) {
                    d.normals = reinterpret_cast<const Vec3f *>(put(m.normals.data(), m.normals.size()));
                    d.normalCount = uint32_t(m.normals.size());
                }
                in.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
                OptixBuildInputCustomPrimitiveArray &c = in.customPrimitiveArray;
                c.aabbBuffers = aabbs;
                c.numPrimitives = count;
                c.strideInBytes = sizeof(OptixAabb);
                c.flags = flags;
                c.numSbtRecords = 1;
            }
            if (!buildGas(gas, in, steps))
                return false;
        }
        shapes[id] = gas.shape;
        meshGas[id] = {gas.handle, gas.kind};
        // Moving instances: an SRT motion transform each, between the IAS
        // and the GAS (the keys continuous, D6).
        const std::vector<SrtTransform> keys = rec.motionSteps > 1 ? continuousKeys(rec) : std::vector<SrtTransform>{};
        for (uint32_t i = 0; i < rec.instanceCount(); ++i) {
            OptixInstance inst{};
            OptixTraversableHandle child = gas.handle;
            if (rec.motionSteps > 1) {
                const size_t bytes = sizeof(OptixSRTMotionTransform) + size_t(rec.motionSteps - 2) * sizeof(OptixSRTData);
                std::vector<char> blob(bytes, 0);
                auto *t = reinterpret_cast<OptixSRTMotionTransform *>(blob.data());
                t->child = gas.handle;
                t->motionOptions.numKeys = uint16_t(rec.motionSteps);
                t->motionOptions.timeBegin = 0.0f;
                t->motionOptions.timeEnd = 1.0f;
                t->motionOptions.flags = OPTIX_MOTION_FLAG_NONE;
                for (uint32_t s = 0; s < rec.motionSteps; ++s)
                    t->srtData[s] = srtData(keys[size_t(s) * rec.instanceCount() + i]);
                m_transforms.emplace_back();
                DeviceBuffer &buf = m_transforms.back();
                if (!buf.reserve(m_device, bytes) || !m_device.upload(buf.ptr(), blob.data(), bytes) ||
                    !m_device.check(optixConvertPointerToTraversableHandle(
                                        m_device.optix(), buf.ptr(), OPTIX_TRAVERSABLE_TYPE_SRT_MOTION_TRANSFORM, &child),
                                    "optixConvertPointerToTraversableHandle"))
                    return false;
                inst.transform[0] = inst.transform[5] = inst.transform[10] = 1.0f; // the keys move it
            } else {
                const Xform34 &x = rec.instances[i];
                // Row-major 3x4 from Nova's columns (x, y, z axes and translation).
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        inst.transform[r * 4 + c] = x.c[c][r];
            }
            inst.instanceId = id;
            inst.sbtOffset = gas.kind + (filtered ? kKinds : 0u);
            // Bit 7: every instance blocks unlinked lights; bits 0-6: the
            // shadow-link categories 0-6 it is in (launch.h).
            inst.visibilityMask = 0x80u;
            for (uint16_t c = 0; c < 7; ++c)
                if (scene.castsShadow(id, i, c))
                    inst.visibilityMask |= 1u << c;
            inst.flags = filtered ? OPTIX_INSTANCE_FLAG_NONE : OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT;
            inst.traversableHandle = child;
            instances.push_back(inst);
        }
    }
    // The motion pipeline, made the first time a scene moves.
    m_motion = motion;
    if (motion && !m_pipelines[1].pipeline) {
        std::string error;
        if (!createPipeline(m_pipelines[1], true, error))
            return false;
    }
    first[scene.meshCount()] = uint32_t(instances.size());
    if (!m_device.upload(m_firstInstance, first) || !m_device.upload(m_shapes, shapes) ||
        !m_device.upload(m_meshGas, meshGas))
        return false;
    m_ias = 0;
    if (instances.empty())
        return m_device.sync();
    if (!m_device.upload(m_instances, instances))
        return false;
    OptixBuildInput in{};
    in.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    in.instanceArray.instances = m_instances.ptr();
    in.instanceArray.numInstances = unsigned(instances.size());
    OptixAccelBuildOptions o{};
    o.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    o.operation = OPTIX_BUILD_OPERATION_BUILD;
    OptixAccelBufferSizes sizes{};
    if (!m_device.check(optixAccelComputeMemoryUsage(m_device.optix(), &o, &in, 1, &sizes),
                        "optixAccelComputeMemoryUsage") ||
        !m_temp.reserve(m_device, sizes.tempSizeInBytes) || !m_iasBuffer.reserve(m_device, sizes.outputSizeInBytes))
        return false;
    return m_device.check(optixAccelBuild(m_device.optix(), m_device.stream(), &o, &in, 1, m_temp.ptr(),
                                          sizes.tempSizeInBytes, m_iasBuffer.ptr(), sizes.outputSizeInBytes, &m_ias,
                                          nullptr, 0),
                          "optixAccelBuild (IAS)") &&
           m_device.sync();
}

bool OptixScene::launch(Raygen raygen, TraceParams params, uint32_t width) {
    if (width == 0)
        return true;
    Device::Scope scope(m_device);
    const Pipeline &p = m_pipelines[m_motion ? 1 : 0];
    params.handle = m_ias; // 0 (an empty scene): every ray misses
    params.firstInstance = firstInstance();
    params.shapes = m_shapes.as<ShapeData>();
    params.gas = m_meshGas.as<MeshGas>();
    OptixShaderBindingTable table = p.table;
    table.raygenRecord = p.sbt.ptr() + size_t(raygen) * kRecord;
    return m_device.upload(m_params.ptr(), &params, sizeof(params)) &&
           m_device.check(optixLaunch(p.pipeline, m_device.stream(), m_params.ptr(), sizeof(TraceParams), &table,
                                      width, 1, 1),
                          "optixLaunch");
}

} // namespace nova::gpu
