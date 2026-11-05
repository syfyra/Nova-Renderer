// OptiX accel probe (docs/research/m10/optix-accel.md): launch parameters and
// SBT data shared by the host (oa_probe.cpp) and the device programs
// (oa_device.cu). Rays are SoA like Nova's RayBatch (core/accel/accel.h):
// org/dir hold 3 floats per ray.
#pragma once

#include <optix_types.h>

struct OaParams {
    OptixTraversableHandle handle;
    const float *org, *dir;   // 3 floats per ray
    const float *tmax, *time; // time may be null
    const float *seed;        // per-ray hit-filter seed, may be null
    const unsigned *count;    // device-side ray count (wavefront queues); null = launch width
    // Hit record, one entry per ray (prim == ~0u: miss).
    float *outT;
    unsigned *outPrim, *outInst;
    float *outUV;             // 2 floats per ray
    unsigned char *outOccl;   // occlusion queries
    float *outInspect;        // 32 floats per ray (__raygen__inspect)
    float opacity;            // __anyhit__opacity: probability that a candidate counts
    unsigned rayFlags;
    unsigned mask;            // visibility mask
    float tmin;
    unsigned *counter;           // __anyhit__opacity_count: any-hit invocations per ray
    unsigned long long linkMask; // __anyhit__shadowlink: instances (by id) that cast shadows
};

// SBT data of the custom-primitive hit groups (points).
struct OaPointData {
    const float *sphere; // 4 floats per point: centre, radius
    const float *normal; // 3 floats per point (oriented discs), else null
};

constexpr int kInspectStride = 32;
