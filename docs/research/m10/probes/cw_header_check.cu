// M10 research probe (docs/research/m10/cuda-wavefront.md, section 9):
// which integrator-side headers that tests/cuda/kernel_headers.cu does not
// cover yet compile as device code (nvcc 12.9, sm_120,
// -Werror=cross-execution-space-call). Each block calls the header's
// functions from a kernel; build with -DCW_CHECK_MAJORANT to include
// core/volume/majorant.h (fails: MajorantLayout::index is host-only).
//
// Build (compile only):
//   nvcc -c -std=c++20 -arch=sm_120 -Werror=cross-execution-space-call -Xcompiler /utf-8
//        -DNOMINMAX -D_USE_MATH_DEFINES -I<repo> [-I$HFS/toolkit/include -DCW_CHECK_MAJORANT]
//        cw_header_check.cu

#include "core/bsdf/hair.h"
#include "core/bsdf/subsurface.h"
#include "core/film/lpe.h"
#include "core/lights/light_sampler.h"
#include "core/lights/light_tree.h"
#include "core/lights/lights.h"
#include "core/math/distribution.h"
#include "core/volume/phase.h"
#if defined(CW_CHECK_MAJORANT)
#include "core/volume/majorant.h"
#endif

namespace nova {

struct TablePmf {
    const float *pmf;
    NOVA_INLINE float operator()(uint32_t i) const { return pmf[i]; }
    NOVA_INLINE bool links(uint32_t) const { return true; }
};

__global__ void headerCheck(float *out, const LightData *lights, uint32_t count, LightTreeView tree,
                            LightSamplerView table, lpe::Program prog, Distribution1DView dist) {
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    const Vec3f p{float(i), 1.0f, 2.0f}, n{0.0f, 0.0f, 1.0f}, d{0.0f, 0.6f, 0.8f};
    float acc = 0.0f;

    // lights.h: sampling, MIS, BSDF-ray queries, camera-visible lights
    const LightSample ls = sampleLight(lights[i % count], p, Vec2f{0.3f, 0.7f});
    acc += ls.pdf + lightSampleWeight(LightStrategy::MIS, ls.pdf, 0.5f, ls.delta);
    const Spectrum along = areaLightsAlongRay(lights, TablePmf{table.pmf}, count, &tree, p, d, 1e30f, 0.5f,
                                              LightStrategy::MIS, 1.0f, NoLightSink{}, NoAtten{});
    const Spectrum esc = escapedRadiance(lights, TablePmf{table.pmf}, count, d, 0.5f, LightStrategy::MIS, 1.0f);
    bool visible = false;
    const Spectrum cam = cameraEnvironment(lights, count, d, visible);
    acc += along.x + esc.y + cam.z;

    // light_tree.h / light_sampler.h: selection and PMF
    float pmf = 0.0f;
    const uint32_t li = tree.sample(0.37f, p, n, Receiver::OneSided, 0u, pmf);
    acc += pmf + tree.pmf(li, p, n, Receiver::OneSided, 0u);
    float tpmf = 0.0f;
    acc += float(table.sample(0.5f, tpmf)) + tpmf;

    // lpe.h: one DFA step
    acc += float(prog.step(prog.start, prog.symbol(lpe::TypeReflect, lpe::ScatterDiffuse, 0, 0)));

    // phase.h, subsurface.h: phase sampling and a random walk with a stub tracer
    const PhaseFunction ph{0.3f, -0.2f, 0.25f};
    float ppdf = 0.0f;
    const Vec3f wi = ph.sample(d, Vec3f{0.2f, 0.4f, 0.6f}, ppdf);
    acc += ph.eval(d, wi) + ppdf;
    Rng rng = Rng::make(i, 3u);
    const SubsurfaceLobe m = SubsurfaceLobe::make(spectrum(0.8f), spectrum(0.1f), 0.0f);
    const auto trace = [&](Vec3f, Vec3f, float tMax, float &t, uint32_t &prim, Vec2f &uv) {
        t = 0.5f * tMax;
        prim = 0;
        uv = Vec2f{0.3f, 0.3f};
        return (rng.next() < 0.5f);
    };
    const WalkResult r = randomWalk(m, p, -n, Vec3f{0.1f, 0.2f, 0.3f}, rng, 64, trace);
    acc += r.weight.x;

    // hair.h
    acc += hair_detail::longitudinal(0.5f, 0.6f, 0.8f, 0.7f, 0.1f) + hair_detail::trimmedLogistic(0.2f, 0.3f);

    // distribution.h
    float dpdf = 0.0f;
    uint32_t off = 0;
    acc += dist.sample(0.5f, dpdf, off);

#if defined(CW_CHECK_MAJORANT)
    MajorantLayout layout;
    MajorantDda dda;
    if (dda.init(layout, p, d, 0.0f, 10.0f)) {
        size_t c;
        float a, b;
        while (dda.next(c, a, b))
            acc += b - a;
    }
#endif
    out[i] = acc;
}

} // namespace nova
