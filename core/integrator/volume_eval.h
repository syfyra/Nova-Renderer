#pragma once

// A volume's coefficients at a point (docs/design/M8.md D4-D6, D15): the
// default volume (white, isotropic, extinction = scattering = density) or
// the material's volume shader with its field reads (D5) sampled there.
// Scattering and absorption are clamped to 0 from below, anisotropies to
// (-0.99, 0.99).

#include "core/integrator/material_eval.h"
#include "core/volume/grid.h"
#include "core/volume/volume_point.h"

#include <memory>
#include <vector>

namespace nova {

// Per thread and volume: the samplers of the volume's fields (NanoVDB
// accessors cache the last nodes) and the shader's rows. Valid while the
// scene is unchanged.
class VolumeEvaluator {
public:
    VolumeEvaluator(const Scene &scene, VolumeId id, const ShadeGlobals &globals = {});
    VolumeEvaluator(const VolumeEvaluator &) = delete;
    VolumeEvaluator &operator=(const VolumeEvaluator &) = delete;

    // At object-space point p (world point w, the ray's direction, normalized
    // time and class, the scattering events before).
    VolumePoint operator()(Vec3f p, Vec3f w, Vec3f dir, float time, uint8_t rayType, uint32_t depth);
    // Field `index` of the volume at object-space point p.
    float field(int32_t index, Vec3f p);

private:
    const Scene &m_scene;
    VolumeId m_id;
    const VolumeRecord &m_rec;
    const MaterialRecord *m_material = nullptr;
    ShadeGlobals m_globals;
    std::vector<std::unique_ptr<VolumeSampler>> m_samplers; // per field (null: no grid)
    std::vector<float> m_rows;
};

} // namespace nova
