#pragma once

// Holdouts along a path (docs/design/M11-P2.md; Karma's holdoutmode,
// measured): shared by the CPU wavefront (wavefront.cpp) and the GPU kernels
// (core/gpu/kernels.cu).
//
// - A camera ray that hits a holdout (Matte or Background) makes the path a
//   holdout path: alpha 0 (the volumes in front keep theirs), nothing to the
//   beauty, its light to the holdout; outputs only (lpeHoldoutState). The
//   path is shaded as usual, its light samples from a background holdout
//   are background shadow rays (kRayFromBackground).
// - Other rays see a Matte object as an ordinary object, and a Background
//   object as pre-lit: an emitting surface (an O event) of radiance = its
//   diffuse albedo x the fraction of the light that reaches it (one shadow
//   ray towards a light sample picked among a few candidates by their
//   unoccluded contribution, prelitPick), plus the lights with
//   illumbackground as they light any object; the path ends there.

#include "core/bsdf/closure.h"
#include "core/film/lpe.h"
#include "core/lights/lights.h"
#include "core/math/rng.h"

#include <cstdint>

namespace nova {

// The LPE state of a path whose camera ray hits a holdout: the camera ray
// reaches its first surface without events of its own, so the path is at
// the program's start; from there the holdout; outputs' automaton (0: none).
NOVA_INLINE lpe::State lpeHoldoutState(const lpe::Program &p, lpe::State s) {
    return s == p.start ? p.holdoutStart : lpe::State(0);
}

// The diffuse albedo of a closure at its wo (Karma: "the diffuse albedo of
// the shader" is the pre-lit plate's colour): the reflected albedo of its
// diffuse lobes; an external closure's whole albedo.
NOVA_INLINE Spectrum diffuseAlbedo(const ClosureList &cl) {
    if (cl.external.active)
        return cl.albedo;
    Spectrum a{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < cl.count; ++i)
        if (cl.lobe[i].diffuse && !cl.lobe[i].sss)
            a += cl.lobe[i].albedoR;
    return a;
}

// Rounds of candidates of the pre-lit shadow fraction: each a light sample
// and a cosine-distributed direction towards the lights at infinity (domes
// that light sampling never picks), weighted against each other by MIS.
constexpr int kPrelitRounds = 4;

// A candidate shadow ray: direction, length, the shadow-link category;
// `visible`: no ray, the light counts as reaching the point (an
// illumbackground light, whose shadows Karma leaves out).
struct PrelitCandidate {
    Vec3f wi{0.0f, 0.0f, 1.0f};
    float distance = 0.0f;
    uint16_t category = 0xFFFFu;
    bool visible = false;
};

struct PrelitPick {
    bool found = false; // false: no light reaches the point unoccluded either (Karma: no light, 0)
    PrelitCandidate ray;
};

// Picks the shadow ray that decides a background point's lit fraction: the
// candidates of `candidate(rng, kind, out)` (kind 0 a light sample, 1 a
// direction; it returns the candidate's unoccluded diffuse contribution as
// luminance), one of them in proportion to that contribution (resampled
// importance, a reservoir). Over the candidates the shadow ray's visibility
// estimates lit / unoccluded light, the fraction Karma's pre-lit plates
// take (measured: 0 where no light arrives, e.g. on faces turned away).
template <class Candidate>
NOVA_INLINE PrelitPick prelitPick(Rng &rng, const Candidate &candidate) {
    PrelitPick pick;
    float total = 0.0f;
    for (int i = 0; i < kPrelitRounds; ++i)
        for (int kind = 0; kind < 2; ++kind) {
            PrelitCandidate c;
            const float w = candidate(rng, kind, c);
            if (!(w > 0.0f))
                continue;
            total += w;
            if (rng.next() * total < w) {
                pick.found = true;
                pick.ray = c;
            }
        }
    return pick;
}

} // namespace nova
