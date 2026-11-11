#include "core/integrator/material_eval.h"

#include "core/math/frame.h"
#include "core/shading/vm.h"

#include <algorithm>
#include <vector>

namespace nova {

namespace {

// Runs program p (the material's program, or its opacity program with the
// uniform block U) for `count` hits in an N-lane chunk.
template <int N>
void fillAndRun(const Scene &scene, MaterialId material, const MaterialRecord &rec, const shading::Program &p,
                const float *U, const SurfaceHit *hits, const Vec3f *dirs, const float *times, int count, float *R,
                const ShadeGlobals &globals, const HitTextureData &texture, const HitRayData &ray) {
    using namespace shading;
    // Bound attributes: the material's program's primvars (an opacity
    // program shares their order; a uv set only it reads has no binding).
    const size_t boundRefs = rec.desc.program ? rec.desc.program->primvars.size() : 0;
    auto lane = [count](int l) { return l < count ? l : count - 1; };
    // Footprint vector `axis` (0, 1) of hit h; zero without footprints.
    auto footprint = [&](int h, int axis) {
        const Vec3f *f = axis ? texture.footprintY : texture.footprintX;
        return f ? f[h] : Vec3f{0.0f, 0.0f, 0.0f};
    };

    // Attribute bindings of the lanes (primvars and the tangent uv set).
    const AttributeRef *refs[N] = {};
    const uint32_t *triangles[N] = {};
    if (!p.primvarBindings.empty() || p.tangentUv >= 0) {
        for (int l = 0; l < N; ++l) {
            const SurfaceHit &hit = hits[lane(l)];
            refs[l] = scene.attributeRefs(hit.mesh, material);
            const std::vector<uint32_t> &indices = scene.meshRecord(hit.mesh).desc.indices;
            triangles[l] = indices.size() >= (size_t(hit.prim) + 1) * 3 ? &indices[size_t(hit.prim) * 3] : nullptr;
        }
    }

    // The lanes' hit data for the globals and primvars (material_lane.h).
    material_lane::LaneHit laneHits[N];
    for (int l = 0; l < N; ++l) {
        const int h = lane(l);
        material_lane::LaneHit &lh = laneHits[l];
        lh.hit = &hits[h];
        lh.dir = dirs[h];
        lh.footprintX = footprint(h, 0);
        lh.footprintY = footprint(h, 1);
        lh.rayType = ray.type ? float(ray.type[h]) : 0.0f;
        lh.depth = ray.depth;
        if (!p.primvarBindings.empty() || p.tangentUv >= 0) {
            lh.tangentUv = refs[l] && size_t(p.tangentUv) < boundRefs ? &refs[l][p.tangentUv] : nullptr;
            lh.triangle = triangles[l];
        }
    }

    for (const GlobalBinding &g : p.globals) {
        float *row = R + size_t(g.row) * N;
        for (int l = 0; l < N; ++l) {
            const int h = lane(l);
            const SurfaceHit &hit = hits[h];
            Vec3f v;
            float s;
            material_lane::globalValue(g.global, laneHits[l], globals,
                                       [&] { return scene.objectToWorld(hit.mesh, hit.instance, times[h]); }, v, s);
            if (globalWidth(g.global) == 3) {
                row[l] = v.x;
                row[N + l] = v.y;
                row[2 * N + l] = v.z;
            } else {
                row[l] = s;
            }
        }
    }

    if (!p.primvarBindings.empty()) {
        for (const PrimvarBinding &b : p.primvarBindings) {
            float *row = R + size_t(b.row) * N;
            const float *fallback = U + b.fallback;
            for (int l = 0; l < N; ++l) {
                float v[4];
                material_lane::primvarValue(refs[l] && b.name < boundRefs ? &refs[l][b.name] : nullptr, laneHits[l],
                                            b.shift, b.width, fallback, v);
                for (int k = 0; k < b.width; ++k)
                    row[size_t(k) * N + size_t(l)] = v[k];
            }
        }
    }

    float rnd[N];
    if (texture.rnd)
        for (int l = 0; l < N; ++l)
            rnd[l] = texture.rnd[lane(l)];
    TextureContext tex{scene.textureSampler(), rec.samplers.data(), uint32_t(rec.samplers.size()),
                       texture.rnd ? rnd : nullptr};
    QueryLane lanes[N];
    if (p.needsQueries && ray.queries) {
        for (int l = 0; l < N; ++l) {
            const int h = lane(l);
            const SurfaceHit &hit = hits[h];
            lanes[l] = QueryLane{hit.P,
                                 hit.N,
                                 hit.Ng,
                                 dirs[h],
                                 ray.queryRnd ? ray.queryRnd[h] : Vec3f{0.5f, 0.5f, 0.5f},
                                 times[h],
                                 hit.mesh,
                                 hit.instance};
            lanes[l].tube = hit.curve && scene.meshRecord(hit.mesh).desc.curveStyle == CurveStyle::Round;
            lanes[l].tangent = hit.tangent;
            lanes[l].curveRadius = hit.curveRadius;
            lanes[l].curveExitEps = hit.curveExitEps;
            lanes[l].leave = hit.leave;
        }
        tex.queries = ray.queries;
        tex.lanes = lanes;
    }
    execute<N>(p.code.data(), p.code.size(), R, U, &tex);
}

} // namespace

void evaluateSurfaceInputs(const Scene &scene, MaterialId material, const SurfaceHit *hits, const Vec3f *dirs,
                           const float *times, int count, const SurfaceInputs **out, MaterialEvalScratch &scratch,
                           const ShadeGlobals &globals, const HitTextureData &texture, const HitRayData &ray) {
    const MaterialRecord &rec = scene.material(material);
    if (!rec.varying) {
        for (int l = 0; l < count; ++l)
            out[l] = &rec.resolved;
        return;
    }
    const shading::Program &p = *rec.desc.program;
    const int N = count <= 1 ? 1 : count <= 8 ? 8 : count <= 16 ? 16 : 32;
    scratch.rows.resize(size_t(p.rowCount) * size_t(N));
    float *R = scratch.rows.data();
    switch (N) {
    case 1:
        fillAndRun<1>(scene, material, rec, p, rec.uniforms.data(), hits, dirs, times, count, R, globals, texture, ray);
        break;
    case 8:
        fillAndRun<8>(scene, material, rec, p, rec.uniforms.data(), hits, dirs, times, count, R, globals, texture, ray);
        break;
    case 16:
        fillAndRun<16>(scene, material, rec, p, rec.uniforms.data(), hits, dirs, times, count, R, globals, texture, ray);
        break;
    default:
        fillAndRun<32>(scene, material, rec, p, rec.uniforms.data(), hits, dirs, times, count, R, globals, texture, ray);
        break;
    }
    for (int l = 0; l < count; ++l) {
        SurfaceInputs &in = scratch.inputs[l];
        copyInputs(in, rec.resolved);
        for (const shading::OutputBinding &o : p.outputs) {
            if (o.uniform)
                continue;
            for (int k = 0; k < o.width; ++k)
                in.v[o.field + k] = R[size_t(o.index + k) * size_t(N) + size_t(l)];
        }
        out[l] = &in;
    }
}

namespace {

// docs/design/M5.md D9: an external material's opacity at one hit, from a
// one-lane bridge run with the hit's parameters.
float externalOpacity(const Scene &scene, MaterialId material, const MaterialRecord &rec, const SurfaceHit &hit,
                      Vec3f dir, float time, uint8_t rayType, uint32_t depth, const ShadeGlobals &globals) {
    const ExternalBinding &x = *rec.desc.external;
    thread_local MaterialEvalScratch scratch;
    const SurfaceInputs *in = &rec.resolved;
    evaluateSurfaceInputs(scene, material, &hit, &dir, &time, 1, &in, scratch, globals, HitTextureData{},
                          HitRayData{&rayType, depth});
    const Frame f = Frame::fromNormal(hit.N);
    ExternalLane lane;
    lane.P = hit.P;
    lane.I = dir;
    lane.N = lane.wi = hit.N;
    lane.Ng = hit.Ng;
    lane.dPds = f.t;
    lane.dPdt = f.b;
    lane.s = hit.uv.x;
    lane.t = hit.uv.y;
    lane.time = time;
    const std::vector<ExternalParam> &params = x.shader->params();
    thread_local std::vector<ExternalInput> ins;
    ins.assign(params.size(), ExternalInput{});
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].width == 0)
            continue;
        if (x.slots[i] != kNoExternalSlot)
            ins[i] = {&in->v[x.slots[i]], false};
        else if (x.values[i].size() >= size_t(params[i].width))
            ins[i] = {x.values[i].data(), false};
    }
    ExternalResult out;
    if (!x.shader->run(1, &lane, ins.data(), x.strings.data(), &out, nullptr))
        return 1.0f;
    return surfaceOpacity(out.opacity, 0.0f);
}

} // namespace

float evaluateOpacity(const Scene &scene, MaterialId material, const SurfaceHit &hit, Vec3f dir, float time,
                      uint8_t rayType, uint32_t depth, const ShadeGlobals *globals) {
    const MaterialRecord &rec = scene.material(material);
    // The render's frame, as pass C and the GPU's any-hit program use it.
    const ShadeGlobals g = globals ? *globals : ShadeGlobals{};
    if (rec.externalOpacity && rec.desc.external)
        return externalOpacity(scene, material, rec, hit, dir, time, rayType, depth, g);
    const shading::Program *op = rec.desc.program ? rec.desc.program->opacity.get() : nullptr;
    if (!op || rec.opacityUniforms.empty())
        return rec.uniformOpacity;
    // Filters run on Embree's threads, several queries per thread at once
    // never (the filter does not recurse).
    thread_local std::vector<float> rows;
    rows.resize(std::max<size_t>(op->rowCount, 1));
    fillAndRun<1>(scene, material, rec, *op, rec.opacityUniforms.data(), &hit, &dir, &time, 1, rows.data(), g,
                  HitTextureData{}, HitRayData{&rayType, depth});
    Spectrum opacity = rec.resolved.c(surface_field::opacity);
    float threshold = rec.resolved.f(surface_field::opacityThreshold);
    for (const shading::OutputBinding &o : op->outputs) {
        float v[4] = {};
        for (int k = 0; k < o.width; ++k)
            v[k] = o.uniform ? rec.opacityUniforms[o.index + size_t(k)] : rows[o.index + size_t(k)];
        if (o.field == surface_field::opacity)
            opacity = {v[0], o.width > 1 ? v[1] : v[0], o.width > 2 ? v[2] : v[0]};
        else if (o.field == surface_field::opacityThreshold)
            threshold = v[0];
    }
    return surfaceOpacity(opacity, threshold);
}

} // namespace nova
