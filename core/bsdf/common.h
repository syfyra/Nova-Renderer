#pragma once

// Conventions of the BSDF lobes (docs/design/M3.md section 5): directions are
// in the lobe's local frame (+Z = its shading normal), wo is the outgoing
// (view) direction and wi the incident (light) direction, both pointing away
// from the surface. Lobes return f * |cos(wi)| ("fcos") and solid-angle pdfs,
// so lobes with different normals (coat, base) combine without converting
// cosines. Everything is NOVA_HOST_DEVICE, float only and free of tables:
// energy compensation and layering are per-hit factors applied by the
// closure that owns the lobe.

#include "core/math/spectrum.h"
#include "core/math/vec.h"

namespace nova {

// A sampled direction. For a delta event (a perfectly smooth interface) the
// density is a discrete probability: pdf is 0, fcos holds the sample weight
// f |cos| / P of the event, and the direction is exact.
struct LobeSample {
    Vec3f wi;
    Spectrum fcos;
    float pdf;
    bool delta;
    bool transmit; // wi lies on the other side of the surface than wo
};

NOVA_INLINE float pow5(float x) {
    const float x2 = x * x;
    return x2 * x2 * x;
}
NOVA_INLINE float pow6(float x) {
    const float x2 = x * x;
    return x2 * x2 * x2;
}

NOVA_INLINE Spectrum spectrumMax(Spectrum a, float b) { return {nova::fmax(a.x, b), nova::fmax(a.y, b), nova::fmax(a.z, b)}; }

// w mirrored about the unit vector m (both pointing away from the surface).
NOVA_INLINE Vec3f reflect(Vec3f w, Vec3f m) { return m * (2.0f * dot(w, m)) - w; }

// w refracted through the interface with unit normal m on w's side;
// eta = n_transmitted / n_incident. cosI = dot(w, m) > 0, cosT the cosine on
// the other side from fresnelDielectric().
NOVA_INLINE Vec3f refract(Vec3f w, Vec3f m, float eta, float cosI, float cosT) {
    return w * (-1.0f / eta) + m * (cosI / eta - cosT);
}

NOVA_INLINE Vec3f mirrorZ(Vec3f w) { return {w.x, w.y, -w.z}; }

} // namespace nova
