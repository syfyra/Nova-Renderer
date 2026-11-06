#pragma once

// Material programs on the device (docs/design/M10.md G2, G6), shared by the
// wavefront kernels (core/gpu/kernels.cu) and the OptiX programs' any-hit
// filter (core/gpu/programs.cu): a program run on one hit's globals,
// primvars and textures (fillAndRun for one lane), the surface inputs of a
// hit, and its opacity (evaluateOpacity) with the stochastic decision of
// Scene::OpacityFilter.

#include "core/gpu/kernel_types.h"
#include "core/scene/material.h"
#include "core/shading/vm.h"

#include <cstring>

namespace nova::gpu {

// Runs `code` with uniform block U on one hit into rows R: the globals and
// primvars of the bindings first (footX, footY: the ray cone's footprint
// vectors; zero: none).
NOVA_INLINE void runMaterialProgram(const ShadeGlobals &globals, uint32_t depth, const GpuMesh &mesh, uint32_t material,
                                    const GpuMaterial &m, const shading::Instr *code, uint32_t codeSize,
                                    const float *U, Span<shading::GlobalBinding> globalBindings,
                                    Span<shading::PrimvarBinding> primvars, const SurfaceHit &hit, Vec3f dir,
                                    float time, uint8_t rayType, Vec3f footX, Vec3f footY, float *R) {
    using namespace shading;
    const AttributeRef *refs = mesh.refs(material);
    material_lane::LaneHit lane;
    lane.hit = &hit;
    lane.dir = dir;
    lane.footprintX = footX;
    lane.footprintY = footY;
    lane.rayType = float(rayType);
    lane.depth = depth;
    lane.tangentUv = refs && uint32_t(m.tangentUv) < m.primvarNames ? &refs[m.tangentUv] : nullptr;
    lane.triangle = mesh.view.desc.indices.data() + size_t(hit.prim) * 3;
    for (size_t i = 0; i < globalBindings.size(); ++i) {
        const GlobalBinding &g = globalBindings[i];
        Vec3f v;
        float s;
        material_lane::globalValue(g.global, lane, globals,
                                   [&] { return instanceToWorld(mesh.view, hit.instance, time); }, v, s);
        if (globalWidth(g.global) == 3) {
            R[g.row] = v.x;
            R[g.row + 1] = v.y;
            R[g.row + 2] = v.z;
        } else {
            R[g.row] = s;
        }
    }
    for (size_t i = 0; i < primvars.size(); ++i) {
        const PrimvarBinding &b = primvars[i];
        float v[4];
        material_lane::primvarValue(refs && b.name < m.primvarNames ? &refs[b.name] : nullptr, lane, b.shift,
                                    b.width, U + b.fallback, v);
        for (int k = 0; k < b.width; ++k)
            R[b.row + k] = v[k];
    }
    TextureContext tex;
    tex.device = m.textures;
    tex.count = m.textureCount;
    execute<1>(code, codeSize, R, U, &tex);
}

// The surface inputs of a hit (evaluateSurfaceInputs for one lane): the
// material's uniform inputs, and where they vary per hit its program's
// outputs over them.
NOVA_INLINE void surfaceInputs(const GpuSettings &st, uint32_t depth, const GpuMesh &mesh, const GpuMaterial &m,
                               uint32_t material, const SurfaceHit &hit, Vec3f dir, float time, uint8_t rayType,
                               Vec3f footX, Vec3f footY, SurfaceInputs &in) {
    copyInputs(in, *m.resolved);
    if (!m.varying)
        return;
    float R[kGpuMaxRows];
    runMaterialProgram(st.globals, depth, mesh, material, m, m.code, m.codeSize, m.uniforms, m.globals, m.primvars,
                       hit, dir, time, rayType, footX, footY, R);
    for (size_t i = 0; i < m.outputs.size(); ++i) {
        const shading::OutputBinding &o = m.outputs[i];
        if (!o.uniform)
            for (int k = 0; k < o.width; ++k)
                in.v[o.field + k] = R[o.index + k];
    }
}

// A hit's opacity (evaluateOpacity): uniform, or the material's opacity
// program run there.
NOVA_INLINE float hitOpacity(const ShadeGlobals &globals, uint32_t depth, const GpuMesh &mesh, uint32_t material,
                             const GpuMaterial &m, const SurfaceHit &hit, Vec3f dir, float time, uint8_t rayType) {
    if (!m.opacityCode)
        return m.uniformOpacity;
    float R[kGpuMaxRows];
    runMaterialProgram(globals, depth, mesh, material, m, m.opacityCode, m.opacityCodeSize, m.opacityUniforms,
                       m.opacityGlobals, m.opacityPrimvars, hit, dir, time, rayType, Vec3f{0.0f, 0.0f, 0.0f},
                       Vec3f{0.0f, 0.0f, 0.0f}, R);
    Spectrum opacity = m.resolved->c(surface_field::opacity);
    float threshold = m.resolved->f(surface_field::opacityThreshold);
    for (size_t i = 0; i < m.opacityOutputs.size(); ++i) {
        const shading::OutputBinding &o = m.opacityOutputs[i];
        float v[4];
        for (int k = 0; k < o.width; ++k)
            v[k] = o.uniform ? m.opacityUniforms[o.index + size_t(k)] : R[o.index + k];
        if (o.field == surface_field::opacity)
            opacity = {v[0], o.width > 1 ? v[1] : v[0], o.width > 2 ? v[2] : v[0]};
        else if (o.field == surface_field::opacityThreshold)
            threshold = v[0];
    }
    return surfaceOpacity(opacity, threshold);
}

// Scene::OpacityFilter's decision for a candidate hit: fake-caustic objects
// are ignored by shadow rays (their transmittance comes after), shadow-link
// categories beyond the visibility masks' seven are tested here, and a
// partly opaque hit is kept with probability `opacity` by one number per
// (ray, surface).
NOVA_INLINE bool acceptHit(const GpuScene &sc, const ShadeGlobals &globals, uint32_t depth, uint32_t meshId,
                           uint32_t prim, uint32_t instance, Vec2f uv, float t, Vec3f org, Vec3f dir, float time,
                           float seed, uint8_t rayType, bool occlusion, uint16_t category, uint8_t rayFlags = 0) {
    const GpuMesh &mesh = sc.meshes[meshId];
    // Ray visibility; shadow rays from background points (Scene::OpacityFilter).
    if (!visibleTo(mesh.visibility, rayType))
        return false;
    if (rayFlags & kRayProbeBackground) {
        if (mesh.holdout != uint8_t(HoldoutMode::Background))
            return false;
    } else if ((rayFlags & kRayFromBackground) && mesh.holdout == uint8_t(HoldoutMode::Background)) {
        return false;
    }
    if (occlusion && mesh.fakeCaustics)
        return false;
    if (occlusion && category != 0xFFFFu && category >= 7 && !mesh.castsShadow(instance, category))
        return false;
    const uint32_t material = materialIdOf(mesh, prim);
    const GpuMaterial &m = sc.materials[material];
    if (!m.hasOpacity)
        return true;
    float o = m.uniformOpacity;
    if (m.opacityCode) {
        const SurfaceHit hit = resolveMeshHit(mesh.view, meshId, prim, uv, t, instance, time, org, dir);
        o = hitOpacity(globals, depth, mesh, material, m, hit, dir, time,
                       occlusion ? uint8_t(RayClass::Shadow) : rayType);
    }
    if (o >= 1.0f)
        return true;
    if (!(o > 0.0f))
        return false;
    uint32_t seedBits;
    std::memcpy(&seedBits, &seed, sizeof(seedBits));
    const auto mix = hit_detail::hashMix;
    const uint32_t h = mix(seedBits ^ mix(meshId * 0x9E3779B1u + mix(prim ^ mix(instance + 0x632BE5ABu))));
    return float(h >> 8) * (1.0f / 16777216.0f) < o;
}

} // namespace nova::gpu
