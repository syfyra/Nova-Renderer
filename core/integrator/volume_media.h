#pragma once

// Volumes along rays on the CPU (docs/design/M8.md D8, D9, D18): the
// trackers of core/integrator/medium_track.h (shared with the GPU) over the
// scene's volume records and this thread's evaluators.

#include "core/integrator/medium_track.h"
#include "core/integrator/volume_eval.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace nova {

// Per thread: an evaluator per volume, rebuilt when the scene changes.
class VolumeEvalCache {
public:
    VolumeEvaluator &get(const Scene &scene, VolumeId id, const ShadeGlobals &globals);

private:
    const Scene *m_scene = nullptr;
    uint64_t m_version = ~0ull;
    float m_frame = 0.0f, m_fps = 0.0f;
    std::vector<std::unique_ptr<VolumeEvaluator>> m_evaluators;
};

// A volume record's data for the trackers.
MediumVolume mediumVolume(const VolumeRecord &rec);

// The volumes a ray (unit direction) crosses before tEnd at shutter time
// `time`, by entry distance.
void volumeSpans(const Scene &scene, Vec3f o, Vec3f d, float tEnd, float time, std::vector<VolumeSpan> &spans);

// Ratio tracking along the ray through every volume it crosses: returns the
// transmittance estimate; with `gather`, adds the emission it collects. The
// sky atmosphere's transmittance is part of it (trackAtmosphere).
// `crossT` (ascending distances, may be empty) receive in `crossOut` the
// transmittance in front of each. `u0` is the first uniform (a Sobol
// sample), `rng` gives the rest.
Spectrum trackMedia(const Scene &scene, VolumeEvalCache &cache, const ShadeGlobals &globals, const MediumRay &ray,
                    float u0, Rng &rng, EmissionGather *gather, const float *crossT = nullptr,
                    Spectrum *crossOut = nullptr, int crossCount = 0);

// A free flight (medium_track.h's flyMedia, with the sky atmosphere's choice:
// flyAtmosphere).
MediumFlight flyMedia(const Scene &scene, VolumeEvalCache &cache, const ShadeGlobals &globals, const MediumRay &ray,
                      Spectrum beta, float u0, Rng &rng, EmissionGather *gather, Spectrum *alphaT,
                      const float *crossT = nullptr, Spectrum *crossOut = nullptr, int crossCount = 0,
                      Spectrum *alphaV = nullptr);

// The medium at distance t along the ray (a point query, D17): each covering
// volume's scattering, phase function and id (at most `max`, the rest
// dropped), and their summed extinction. Returns how many scatter.
int mediumAt(const Scene &scene, VolumeEvalCache &cache, const ShadeGlobals &globals, const MediumRay &ray, float t,
             MediumPart *parts, int max, Spectrum &extinction);

// Where a ray's volumes start and end before ray.tEnd (their world boxes):
// false if it crosses none.
bool mediumExtent(const Scene &scene, const MediumRay &ray, float &a, float &b);

// Tentative collisions where the extinction exceeded the majorant (D8: only
// in cells with a sampled estimate; the trackers stay unbiased), since the
// process started. For tests and HDNOVA_VOLUME diagnostics.
uint64_t majorantViolations();

} // namespace nova
