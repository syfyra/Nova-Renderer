// M10 research probe (docs/research/m10/cuda-wavefront.md, section 6): the
// sizes of Nova's per-path, per-ray and per-hit structures as nvcc sees them
// on the device and the host, and the bytes per path slot of WaveQueues
// (core/integrator/wavefront.h) summed from its members' element types.
//
// Build: cw_build.bat <out> sizes. Run: cw_sizes.exe

#include "core/bsdf/closure.h"
#include "core/bsdf/subsurface.h"
#include "core/bsdf/surface.h"
#include "core/film/lpe.h"
#include "core/lights/light_tree.h"
#include "core/lights/lights.h"
#include "core/math/spectrum.h"
#include "core/volume/phase.h"

#include <cstdio>

using namespace nova;

struct DeviceSizes {
    unsigned closureList, lobe, surfaceInputs, lightData, lightSample, treeNode;
};

__global__ void sizes(DeviceSizes *out) {
    out->closureList = sizeof(ClosureList);
    out->lobe = sizeof(Lobe);
    out->surfaceInputs = sizeof(SurfaceInputs);
    out->lightData = sizeof(LightData);
    out->lightSample = sizeof(LightSample);
    out->treeNode = sizeof(LightTreeNode);
}

// The members of WaveQueues per path slot, by element type (wavefront.h).
struct Member {
    const char *name;
    size_t bytes; // per slot
    double perPath; // slots per path (the shadow queue has 3)
};

int main() {
    DeviceSizes *d, h{};
    cudaMalloc(&d, sizeof(DeviceSizes));
    sizes<<<1, 1>>>(d);
    cudaMemcpy(&h, d, sizeof(DeviceSizes), cudaMemcpyDeviceToHost);
    std::printf("device sizeof: ClosureList %u, Lobe %u (x%d), SurfaceInputs %u (%d floats), LightData %u, "
                "LightSample %u, LightTreeNode %u\n",
                h.closureList, h.lobe, ClosureList::kMaxLobes, h.surfaceInputs, int(surface_field::kBlockSize),
                h.lightData, h.lightSample, h.treeNode);
    std::printf("host sizeof:   ClosureList %zu, Lobe %zu, SurfaceInputs %zu, LightData %zu\n", sizeof(ClosureList),
                sizeof(Lobe), sizeof(SurfaceInputs), sizeof(LightData));

    const size_t S = sizeof(Spectrum), V3 = sizeof(Vec3f), V2 = sizeof(Vec2f), f = 4, u = 4, b = 1;
    const size_t ray = 2 * V3 + 3 * f + u + b; // RayQueue: org dir tMax time path filterSeed rayType
    const size_t walk = 4 * 4 + 2 * V3 + sizeof(SubsurfaceLobe);
    const Member members[] = {
        {"path state (pixel..lpeState, 21 arrays)",
         u + S + S + u + u + f + f + f + S + b + f + f + 8 + f + b + b + V3 + V3 + b + b + sizeof(lpe::State), 1},
        {"rays + nextRays + aoRays (RayQueue)", ray, 3},
        {"aoOccluded", b, 1},
        {"hits (t mesh prim uv instance)", f + u + u + V2 + u, 1},
        {"shadows: RayQueue + contribution .. emitterClamp", ray + S + b + u + b + 2 + u + V2 + f, 3},
        {"shade order (shadeRay .. shadeOrderMaterial)", 4 * u, 1},
        {"volumes (mediumT .. eqB, volumeShade)",
         S + u + b + b + f + u + sizeof(PhaseFunction) + S + u + S + f + f, 1},
        {"walks (WaveQueues::Walk)", walk, 1},
    };
    double total = 0.0;
    for (const Member &m : members) {
        std::printf("  %-52s %5zu B x %.0f = %6.1f B\n", m.name, m.bytes, m.perPath, m.bytes * m.perPath);
        total += m.bytes * m.perPath;
    }
    std::printf("WaveQueues per path slot (without the LPE part vectors): %.0f B\n", total);
    std::printf("  x 2,073,600 paths (1080p, 1 spp): %.2f GB\n", total * 2073600.0 / 1e9);
    return 0;
}
