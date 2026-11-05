// Compile-only check (docs/design/M3.md K14): every kernel-side header of
// nova_core must build as CUDA device code, so the GPU backend (M10) can use
// the same code. The kernel below calls each header's functions, which makes
// nvcc compile their device versions; a host-only call inside them (STL
// containers, virtual calls, host statics) is an error (-Werror
// cross-execution-space-call). Never launched.
//
// Add every new kernel-side header here, with a call that reaches its code.

#include "core/bsdf/conductor.h"
#include "core/bsdf/dielectric.h"
#include "core/bsdf/diffuse.h"
#include "core/bsdf/energy.h"
#include "core/bsdf/shading_frame.h"
#include "core/bsdf/sheen.h"
#include "core/bsdf/subsurface.h"
#include "core/bsdf/surface.h"
#include "core/bsdf/thin_film.h"
#include "core/film/filter.h"
#include "core/integrator/material_lane.h"
#include "core/lights/light_tree.h"
#include "core/lights/lights.h"
#include "core/math/common.h"
#include "core/math/frame.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"
#include "core/math/sobol.h"
#include "core/math/spectrum.h"
#include "core/math/vec.h"
#include "core/scene/attributes.h"
#include "core/scene/hit.h"
#include "core/scene/ray.h"
#include "core/scene/scene_view.h"
#include "core/shading/vm.h"

namespace nova {

__global__ void kernelHeadersCheck(float *out, const BsdfTables tables, const float *xyzToRgb,
                                   const shading::Instr *code, unsigned codeSize, const float *uniforms,
                                   const AttributeRef attribute, const uint32_t *triangle) {
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;

    // vec, spectrum, frame
    const Vec3f n = normalize(Vec3f{0.3f, 0.4f, 0.5f + float(i)});
    const Frame frame = Frame::fromNormal(n);
    Spectrum s = spectrum(0.5f) * luminance(Spectrum{1.0f, 2.0f, 3.0f});

    // sampling warps and MIS
    const Vec3f d = frame.toWorld(sampleCosineHemisphere({0.25f, 0.75f}));
    const Vec3f c = sampleUniformCone({0.5f, 0.5f}, oneMinusCos(0.1f));
    const float w = powerHeuristic(cosineHemispherePdf(d.z), uniformSpherePdf());

    // Sobol / Owen scrambling, PCG
    const Sampler sampler{mix32(i), i};
    const Vec3f u = sampler.get3D(stream::bounce(1, stream::kBsdf));
    Rng rng = Rng::make(i, 7u);
    s += Spectrum{u.x, u.y, u.z} * rng.next();

    // pixel filter sampling, ray offsets
    PixelFilter filter;
    filter.type = PixelFilterType::Gaussian;
    filter.sigma = 0.5f;
    filter.gaussErf = 0.99f;
    const Vec2f jitter = filter.sample({u.x, u.y});
    const Vec3f origin = offsetRayOrigin(Vec3f{jitter.x, jitter.y, 1.0f}, n);

    // BSDF lobes, Fresnel and energy tables
    const Vec2f aSs = alphaStandardSurface(0.4f, 0.5f), aPbr = alphaOpenPbr(0.4f, 0.5f);
    const DielectricLobe glass{GgxDistribution::make(aSs.x, aSs.y), openPbrModulatedEta(1.5f, 0.8f), spectrum(1.0f),
                               spectrum(0.9f), DielectricLobe::kBoth};
    const ThinDielectricLobe sheet{GgxDistribution::make(0.1f, 0.1f),
                                   GgxDistribution::make(ThinDielectricLobe::transmissionAlpha(0.1f, 1.5f), 0.1f),
                                   1.5f, spectrum(1.0f), spectrum(0.8f)};
    Spectrum nk, kk;
    artisticIor(spectrum(0.9f), spectrum(0.5f), nk, kk);
    const ConductorLobe metal{GgxDistribution::make(aPbr.x, aPbr.y), FresnelModel::Conductor, nk, kk, 5.0f};
    const LambertLobe diffuse{spectrum(0.5f), false};
    LobeSample ls;
    float pdf = 0.0f;
    Spectrum f = glass.eval(d, -c, pdf) + sheet.eval(d, c, pdf) + metal.eval(d, c, pdf) + diffuse.eval(d, c, pdf);
    if (glass.sample(d, {u.x, u.y}, u.z, ls) && sheet.sample(d, {u.y, u.x}, u.z, ls) && metal.sample(d, {u.x, u.z}, ls) &&
        diffuse.sample(d, {u.z, u.y}, ls))
        f += ls.fcos;
    const QonLobe qon{spectrum(0.5f), 0.7f};
    const EonLobe eon{spectrum(0.5f), 0.7f};
    const CharlieLobe charlie{spectrum(1.0f), 0.3f};
    const ZeltnerLobe zeltner{spectrum(1.0f), 0.5f};
    f += qon.eval(d, c, pdf) + eon.eval(d, c, pdf) + charlie.eval(d, c, pdf) + zeltner.eval(d, c, pdf) +
         eon.directionalAlbedo(d.z) * QonLobe::albedoFit(d.z, 0.7f) * CharlieLobe::albedoFit(d.z, 0.3f);
    if (qon.sample(d, {u.x, u.y}, ls) && eon.sample(d, {u.y, u.z}, u.x, ls) && charlie.sample(d, {u.z, u.x}, ls) &&
        zeltner.sample(d, {u.x, u.z}, ls))
        f += ls.fcos;
    AiryBase airyBase{};
    airyBase.n = spectrum(1.5f);
    airyBase.k = spectrum(0.0f);
    f += fresnelAiry(d.z, ThinFilm{1.0f, 400.0f, 1.33f}, airyBase, xyzToRgb);
    const Vec3f bent = ensureValidSpecularReflection(Vec3f{0.0f, 0.0f, 1.0f}, d, n);
    f *= bumpShadowingTerm(Vec3f{0.0f, 0.0f, 1.0f}, bent, c);
    const float r = lutRough(aSs.x, aSs.y);
    f += spectrum(sheenAlbedo(tables, 0.3f, d.z));
    f *= msCompensation(ggxAlbedo(tables, r, d.z), ggxAlbedoAvg(tables, r), conductorFresnelAvg(nk, kk));
    f += ggxSchlickAlbedo(tables, r, d.z, spectrum(0.04f), spectrum(1.0f), spectrum(f82Coefficient(0.04f, 0.5f, 1.0f, 5.0f)));
    const float e = dielectricAlbedoRT(tables, 1.0f / 1.5f, r, d.z) + dielectricAlbedoR(tables, 1.5f, r, d.z) +
                    dielectricFresnelAvg(tables, 0.8f) + fresnelF82TintAvg(0.5f, 0.5f, 1.0f, 5.0f);

    // Closures of the surface models
    SurfaceInputs in{};
    in.model = (i & 1) ? SurfaceModel::OpenPBR : SurfaceModel::StandardSurface;
    in.set(surface_field::baseColor, spectrum(0.8f));
    in.set(surface_field::specularIor, 1.5f);
    in.set(surface_field::coatWeight, u.x);
    in.set(surface_field::transmissionWeight, u.y);
    const ClosureContext ctx{&tables, xyzToRgb};
    ClosureList cl;
    buildClosure(in, ShadingGeometry{n, n, frame.t, true}, d, ctx, cl);
    ClosureSample cs;
    if (sampleClosure(cl, d, u, cs))
        f += cs.weight * evalClosure(cl, d, cs.wi).pdf;

    // Shading VM (lane executor) and attribute fetches
    float rows[16] = {u.x, u.y, u.z};
    shading::execute<1>(code, codeSize, rows, uniforms);
    float attr[4];
    if (fetchAttribute(attribute, triangle, i, i, u.x, u.y, 3, attr))
        rows[0] += attr[0] + attr[2];
    f += spectrum(rows[0] + rows[1]);

    out[i] = dot(d, c) * w + maxComponent(absolute(faceForward(s, n))) + float(isFinite(s)) + origin.z +
             maxComponent(f) * e;
}

// Hit reconstruction on scene views (docs/design/M10.md D3): every shape.
__global__ void hitHeadersCheck(float *out, const MeshRecordView *meshes, const uint32_t *meshIds, const uint32_t *prims,
                                const Vec2f *uvs, const float *ts) {
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    const MeshRecordView &rec = meshes[meshIds[i]];
    const SurfaceHit hit = resolveMeshHit(rec, meshIds[i], prims[i], uvs[i], ts[i], 0u, 0.5f,
                                          Vec3f{0.0f, 0.0f, 2.0f}, Vec3f{0.0f, 0.0f, -1.0f});
    // A shading program's globals and primvars at the hit (material_lane.h).
    material_lane::LaneHit lane;
    lane.hit = &hit;
    lane.dir = Vec3f{0.0f, 0.0f, -1.0f};
    Vec3f v;
    float s;
    material_lane::globalValue(shading::Global::DPdu, lane, ShadeGlobals{},
                               [&] { return instanceToWorld(rec, 0u, 0.5f); }, v, s);
    float pv[4];
    const float fallback[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    material_lane::primvarValue(nullptr, lane, 1, 3, fallback, pv);
    out[i] = hit.P.x + hit.N.y + meshCurvature(rec, hit, 0.5f) + hit.strandRandom + v.x + s + pv[0];
}

// The subsurface random walk (docs/design/M9.md D7) against a half space.
struct DeviceHalfSpace {
    NOVA_INLINE bool operator()(Vec3f o, Vec3f d, float tMax, float &t, uint32_t &prim, Vec2f &uv) const {
        if (!(d.z > 0.0f))
            return false;
        t = -o.z / d.z;
        prim = 0;
        uv = {0.0f, 0.0f};
        return t > 0.0f && t < tMax;
    }
};

__global__ void walkHeadersCheck(float *out) {
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    const SubsurfaceLobe m = SubsurfaceLobe::make(spectrum(0.8f), Spectrum{1.0f, 0.5f, 0.25f}, 0.2f);
    Rng rng = Rng::make(i, 3u);
    const WalkResult r = randomWalk(m, Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{0.0f, 0.0f, -1.0f}, Vec3f{0.3f, 0.6f, 0.9f},
                                    rng, 256, DeviceHalfSpace{});
    out[i] = luminance(r.weight) + r.org.z;
}

// Light sampling, the light tree, emissive triangles and the light queries
// of BSDF rays (docs/design/M9.md D11-D14).
struct DeviceLightPmf {
    const LightTreeView *tree;
    NOVA_INLINE float operator()(uint32_t i) const {
        return tree->pmf(i, Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{0.0f, 0.0f, 1.0f}, Receiver::None, 0u);
    }
    NOVA_INLINE bool links(uint32_t i) const { return tree->links.links(i, 0u); }
};

__global__ void lightHeadersCheck(float *out, const LightData *lights, uint32_t count, const LightTreeView tree) {
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    const Vec3f p{float(i), 0.5f, 1.0f}, n{0.0f, 0.0f, 1.0f};
    float pmf = 0.0f;
    const uint32_t li = tree.sample(0.3f, p, n, Receiver::OneSided, 0u, pmf);
    const LightData &light = lights[li < count ? li : 0u];
    const LightSample ls = sampleLight(light, p, {0.25f, 0.5f});
    float sum = ls.pdf * pmf + tree.pmf(0u, p, n, Receiver::TwoSided, 0u) + emitterPdf(light, p, light.position);
    const DeviceLightPmf pm{&tree};
    const Spectrum L = areaLightsAlongRay(lights, pm, count, &tree, p, Vec3f{0.0f, 0.0f, -1.0f}, 10.0f, p, 0.5f,
                                          LightStrategy::MIS) +
                       escapedRadiance(lights, pm, count, n, 0.5f, LightStrategy::MIS);
    Vec2f bary;
    sum += sampleTriangle(light.position, light.axisU, light.axisV, p, {0.5f, 0.5f}, bary).pdf + bary.x;
    out[i] = sum + luminance(L) + luminance(ls.Li);
}

} // namespace nova
