#pragma once

// OptiX acceleration structures and ray queries (docs/design/M10.md D5, D6):
// one compacted GAS per mesh, one IAS over every visible instance
// (instanceId = MeshId, instances of a mesh contiguous), and the raygen
// programs of core/gpu/programs.cu launched over SoA ray queues. Triangles
// and the round curves of every basis use OptiX's built-in primitives
// (flat, ray-facing curves render as round tubes; normal-oriented ribbons are
// tessellated into triangles), analytic spheres and
// sphere points a custom sphere (exit hits for glass), ray-facing discs the
// built-in sphere, oriented discs a custom disc; subsurface walks trace a
// mesh's GAS alone. Deforming meshes get a
// motion GAS, moving instances an SRT motion transform (OptiX interpolates
// its keys by nlerp, D6); a scene with motion uses the motion pipeline.

#include "core/gpu/device.h"
#include "core/gpu/launch.h"
#include "core/scene/motion.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nova {
class Scene;
struct MeshRecord;
}

namespace nova::gpu {

class OptixScene {
public:
    explicit OptixScene(Device &device) : m_device(device) {}
    ~OptixScene();
    OptixScene(const OptixScene &) = delete;
    OptixScene &operator=(const OptixScene &) = delete;

    // The static pipeline; false on failure.
    bool init(std::string &error);
    // Builds the GAS of every mesh whose geometry or filtering changed since
    // the last call, and the IAS.
    // Meshes without primitives it can build are left out and counted in
    // `skipped`.
    bool build(const Scene &scene, uint32_t &skipped);
    // Whether the GPU renders a mesh (the kinds build() represents).
    static bool supports(const MeshRecord &rec);
    // A moving mesh's instance keys with each instance's consecutive
    // rotations in one hemisphere (OptiX's nlerp then takes the shortest
    // arc, as Embree's slerp; step-major as MeshRecord::motionKeys).
    static std::vector<SrtTransform> continuousKeys(const MeshRecord &rec);

    OptixTraversableHandle handle() const { return m_ias; }
    const uint32_t *firstInstance() const { return m_firstInstance.as<uint32_t>(); }

    // Launches a raygen program over `width` rays (TraceParams::count may
    // cut it on the device). The handle and instance tables are filled in.
    bool launch(Raygen raygen, TraceParams params, uint32_t width);

    // The hit groups, one SBT record each (an instance's sbtOffset); each
    // kind once more with the any-hit filter (kind + kKinds) for filtered
    // meshes (Scene::OpacityFilter, D7).
    enum Kind : uint32_t { Triangles, Linear, BSpline, CatmullRom, Bezier, BuiltinSphere, Sphere, Disc, kKinds };

private:
    // A mesh's GAS with the buffers it was built from and its custom
    // primitives' data, kept while the mesh's geometry and filtering stay
    // (MeshRecord::geometryVersion; IPR, D14).
    struct Gas {
        Gas() = default;
        Gas(Gas &&) = default; // no copies: the buffers move
        Gas &operator=(Gas &&) = default;
        DeviceBuffer buffer;
        OptixTraversableHandle handle = 0;
        Kind kind = Triangles;
        uint64_t version = 0;
        bool filtered = false;
        ShapeData shape;
        std::vector<DeviceBuffer> inputs;
    };
    // The modules, program groups, pipeline and SBT of one variant.
    struct Pipeline {
        OptixModule module = nullptr;
        OptixModule builtin[5] = {}; // linear, B-spline, Catmull-Rom, Bezier, sphere
        OptixProgramGroup groups[4 + 2 * kKinds] = {}; // raygens (closest, occluded, walk), miss, hit groups
        OptixPipeline pipeline = nullptr;
        DeviceBuffer sbt;
        OptixShaderBindingTable table{};
    };

    bool createPipeline(Pipeline &p, bool motion, std::string &error);
    void destroyPipeline(Pipeline &p);
    bool buildGas(Gas &gas, const OptixBuildInput &input, uint32_t motionKeys);

    Device &m_device;
    Pipeline m_pipelines[2]; // static, motion
    bool m_motion = false;   // the scene moves: the motion pipeline traces it
    DeviceBuffer m_params;
    std::vector<Gas> m_gas; // per MeshId
    std::vector<DeviceBuffer> m_transforms;
    DeviceBuffer m_instances, m_iasBuffer, m_firstInstance, m_shapes, m_temp;
    DeviceBuffer m_meshGas; // per MeshId its GAS alone (MeshGas, subsurface walks)
    OptixTraversableHandle m_ias = 0;
};

} // namespace nova::gpu
