#include "core/scene/material.h"

#include "core/film/crypto.h"

#include "core/bsdf/energy.h"
#include "core/bsdf/thin_film.h"
#include "core/math/frame.h"
#include "core/shading/compiler.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace nova {

namespace {

// docs/design/M9.md D14: whether the material emits. The uniform inputs'
// emission, seen along the normal, and whether an input it depends on
// varies per hit: the native layout's emission weight and color (at every
// native segment's base), a graph's emission terms with their Schlick
// factors, and the segments' weights.
void classifyEmission(MaterialRecord &r) {
    const SurfaceInputs &in = r.resolved;
    if (in.model == SurfaceModel::External)
        return;
    const ClosureContext ctx{&bsdfTables(), kXyzToRec709Grey};
    const ShadingGeometry g{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, false, 0.5f};
    ClosureList cl;
    if (in.model == SurfaceModel::Lambert)
        buildLambertClosure(in.c(surface_field::baseColor), g, {0.0f, 0.0f, 1.0f}, cl);
    else
        buildClosure(in, g, {0.0f, 0.0f, 1.0f}, ctx, cl);
    r.uniformEmission = cl.emission;
    bool varying = false;
    if (const shading::Program *p = r.desc.program.get(); p && r.varying) {
        std::vector<std::pair<uint16_t, uint16_t>> slots; // offset, width
        const auto native = [&](uint16_t base) {
            slots.emplace_back(uint16_t(base + surface_field::emissionWeight), uint16_t(1));
            slots.emplace_back(uint16_t(base + surface_field::emissionColor), uint16_t(3));
        };
        if (const ClosureGraph *G = p->closure.get()) {
            for (int s = 0; s < G->segmentCount; ++s) {
                if (G->segment[s].model != SurfaceModel::Graph)
                    native(G->segment[s].base);
                if (G->segment[s].weight != kNoSlot)
                    slots.emplace_back(G->segment[s].weight, uint16_t(1));
            }
            for (int e = 0; e < G->emissionCount; ++e) {
                slots.emplace_back(G->emission[e].color, uint16_t(3));
                if (G->emission[e].schlick != kNoSlot)
                    slots.emplace_back(G->emission[e].schlick, uint16_t(7));
            }
        } else {
            native(0);
        }
        for (const shading::OutputBinding &o : p->outputs)
            for (const auto &[offset, width] : slots)
                varying |= !o.uniform && o.field < offset + width && offset < o.field + o.width;
    }
    const Spectrum e = r.uniformEmission;
    if (varying)
        r.emission = MaterialRecord::Emission::Varying;
    else if (e.x != 0.0f || e.y != 0.0f || e.z != 0.0f)
        r.emission = MaterialRecord::Emission::Uniform;
}

// docs/design/M5.md D9: an external shader's opacity. A shader that never
// sets Of is opaque. Otherwise a probe runs it at 64 random points with the
// material's parameter values and 64 with random values (primvars may
// replace them per hit): one value everywhere is the material's uniform
// opacity, anything else is evaluated per hit. (A texture whose holes all
// 128 points miss passes as uniform.)
void externalOpacity(const ExternalBinding &x, MaterialRecord &r) {
    if (!x.shader->writesOpacity())
        return;
    constexpr int kProbe = 64;
    uint32_t state = 0x9E3779B9u;
    auto rnd = [&state] {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) * (1.0f / 16777216.0f);
    };
    auto direction = [&] {
        const float z = 2.0f * rnd() - 1.0f, phi = 6.2831853f * rnd(), s = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return Vec3f{s * std::cos(phi), s * std::sin(phi), z};
    };
    const std::vector<ExternalParam> &params = x.shader->params();
    std::vector<ExternalLane> lanes(kProbe);
    std::vector<ExternalResult> out(kProbe);
    std::vector<std::vector<float>> random(params.size());
    float first = -1.0f;
    for (int pass = 0; pass < 2; ++pass) {
        for (ExternalLane &lane : lanes) {
            lane.P = Vec3f{4.0f * rnd() - 2.0f, 4.0f * rnd() - 2.0f, 4.0f * rnd() - 2.0f};
            lane.N = lane.Ng = lane.wi = direction();
            lane.I = -lane.N;
            const Frame f = Frame::fromNormal(lane.N);
            lane.dPds = f.t;
            lane.dPdt = f.b;
            lane.s = rnd();
            lane.t = rnd();
            lane.time = rnd();
        }
        std::vector<ExternalInput> ins(params.size());
        for (size_t i = 0; i < params.size(); ++i) {
            const int w = params[i].width;
            if (w == 0)
                continue;
            if (pass == 1) { // random values per lane (ints 0 or 1)
                random[i].resize(size_t(w) * kProbe);
                for (float &v : random[i])
                    v = params[i].isInt ? float(rnd() < 0.5f) : rnd();
                ins[i] = {random[i].data(), true};
            } else if (i < x.values.size() && x.values[i].size() >= size_t(w)) {
                ins[i] = {x.values[i].data(), false};
            }
        }
        if (!x.shader->run(kProbe, lanes.data(), ins.data(), x.strings.data(), out.data(), nullptr))
            return; // the render fails the same way
        for (const ExternalResult &o : out) {
            const float v = surfaceOpacity(o.opacity, 0.0f);
            if (first < 0.0f)
                first = v;
            if (v != first) {
                r.externalOpacity = true;
                return;
            }
        }
    }
    r.uniformOpacity = first;
}

} // namespace

MaterialRecord makeMaterialRecord(const MaterialDesc &desc) {
    MaterialRecord r;
    r.desc = desc;
    r.resolved = desc.inputs;
    const shading::Program *p = desc.program.get();
    if (!p) {
        r.uniformOpacity = surfaceOpacity(r.resolved.c(surface_field::opacity), r.resolved.f(surface_field::opacityThreshold));
        if (desc.external && desc.external->shader)
            externalOpacity(*desc.external, r);
        r.hasOpacity = r.externalOpacity || r.uniformOpacity < 1.0f;
        classifyEmission(r);
        return r;
    }
    p->initUniforms(desc.params.size() >= p->paramCount ? desc.params.data() : nullptr, r.uniforms);
    r.resolved.graph = p->closure.get();
    for (const shading::OutputBinding &o : p->outputs) {
        r.resolved.size = std::max<uint16_t>(r.resolved.size, uint16_t(o.field + o.width));
        if (!o.uniform)
            continue;
        for (int k = 0; k < o.width; ++k)
            r.resolved.v[o.field + k] = r.uniforms[o.index + size_t(k)];
    }
    if (p->closure) // native segments read their whole layout
        for (int s = 0; s < p->closure->segmentCount; ++s)
            if (p->closure->segment[s].model != SurfaceModel::Graph)
                r.resolved.size = std::max<uint16_t>(r.resolved.size,
                                                     uint16_t(p->closure->segment[s].base + surface_field::kCount));
    r.varying = !p->varyingFree();
    r.samplers.resize(p->textures.size());
    for (size_t i = 0; i < p->textures.size(); ++i) {
        r.samplers[i].slot = p->textures[i];
        r.samplers[i].texture = i < desc.textures.size() ? desc.textures[i] : kNoTexture;
    }
    r.uniformOpacity = surfaceOpacity(r.resolved.c(surface_field::opacity), r.resolved.f(surface_field::opacityThreshold));
    if (p->opacity)
        p->opacity->initUniforms(desc.params.size() >= p->paramCount ? desc.params.data() : nullptr, r.opacityUniforms);
    if (const shading::Program *v = p->volume.get()) {
        v->initUniforms(desc.params.size() >= p->paramCount ? desc.params.data() : nullptr, r.volumeUniforms);
        for (const shading::OutputBinding &o : v->outputs)
            if (o.uniform)
                for (int k = 0; k < o.width; ++k)
                    r.volume.v[o.field + k] = r.volumeUniforms[o.index + size_t(k)];
    }
    if (desc.external && desc.external->shader)
        externalOpacity(*desc.external, r);
    r.hasOpacity = !r.opacityUniforms.empty() || r.externalOpacity || r.uniformOpacity < 1.0f;
    classifyEmission(r);
    return r;
}

MaterialDesc defaultMaterial() {
    static const shading::ProgramPtr program = [] {
        shading::IrGraph g;
        g.model = SurfaceModel::Lambert;
        shading::IrBuilder b(g);
        const shading::Ref color = b.primvar("displayColor", 3, b.constant(0.18f));
        b.output(surface_field::baseColor, 3, color);
        shading::CompileResult result = shading::compile(g);
        assert(result.program);
        return result.program;
    }();
    MaterialDesc d;
    d.inputs = lambertDefaults();
    d.program = program;
    d.cryptoId = cryptoHash("defaultshader"); // Karma's material name of unbound geometry
    return d;
}

MaterialDesc plasticDefaultMaterial() {
    static const shading::ProgramPtr program = [] {
        shading::IrGraph g;
        g.model = SurfaceModel::Plastic;
        shading::IrBuilder b(g);
        const shading::Ref color = b.primvar("displayColor", 3, b.constant(1.0f));
        b.output(surface_field::baseColor, 3, color);
        shading::CompileResult result = shading::compile(g);
        assert(result.program);
        return result.program;
    }();
    MaterialDesc d;
    d.inputs = plasticDefaults();
    d.program = program;
    d.cryptoId = cryptoHash("defaultshader");
    return d;
}

} // namespace nova
