#pragma once

// Nested dielectrics (Karma's dielectricpriority; docs/design/M11-P9b.md),
// shared by the CPU (wavefront.cpp) and the GPU (core/gpu/kernels.cu).
//
// Karma, measured: overlapping refracting objects (liquid in a glass) are
// resolved by their priorities, 0 the highest. A path keeps the media it is
// inside, one per priority level: entering an object takes its level's
// place, leaving an object empties its level. A surface whose level is
// below the highest occupied one is a false interface: the path crosses it
// as if it were not there (no event, no Fresnel, no ray-limit count) and
// only its level changes. At a real interface the transmission bends by the
// ratio of the surface's index and the index of the medium on its far side
// (the highest occupied level other than its own); the reflectance keeps the
// surface's own index, and the absorption inside is the highest level's.
// Equal priorities replace each other, which is the behaviour without
// nesting: leaving any object leaves its level empty. Shadow rays and
// fake-caustic pass-throughs cross every surface (Karma: priorities do not
// change shadows).

#include "core/math/spectrum.h"

#include <cstdint>

namespace nova {

struct NestedMedia {
    // Glass, liquid, ice and bubbles: four levels at once; entering a fifth
    // drops the lowest.
    static constexpr int kSlots = 4;
    uint8_t count;
    uint8_t priority[kSlots]; // sorted, highest priority (lowest number) first
    float ior[kSlots];        // the medium's index (its surface's own)
    Spectrum sigmaA[kSlots];  // its absorption (0: none)

    NOVA_INLINE void clear() { count = 0; }

    // A surface of priority p is a false interface: a higher level is occupied.
    NOVA_INLINE bool falseInterface(uint8_t p) const { return count > 0 && priority[0] < p; }

    // The index on the far side of a real interface of priority p: the
    // highest occupied level other than p's (1: none).
    NOVA_INLINE float otherIndex(uint8_t p) const {
        for (int i = 0; i < count; ++i)
            if (priority[i] != p)
                return ior[i];
        return 1.0f;
    }

    // The absorption of the medium the path is in.
    NOVA_INLINE Spectrum absorption() const { return count > 0 ? sigmaA[0] : Spectrum{0.0f, 0.0f, 0.0f}; }

    NOVA_INLINE void leave(uint8_t p) {
        int j = 0;
        for (int i = 0; i < count; ++i)
            if (priority[i] != p) {
                priority[j] = priority[i];
                ior[j] = ior[i];
                sigmaA[j] = sigmaA[i];
                ++j;
            }
        count = uint8_t(j);
    }

    NOVA_INLINE void enter(uint8_t p, float n, Spectrum s) {
        leave(p);
        int at = 0;
        while (at < count && priority[at] < p)
            ++at;
        if (at >= kSlots)
            return; // below four higher levels: dropped
        const int last = count < kSlots ? count : kSlots - 1;
        for (int i = last; i > at; --i) {
            priority[i] = priority[i - 1];
            ior[i] = ior[i - 1];
            sigmaA[i] = sigmaA[i - 1];
        }
        priority[at] = p;
        ior[at] = n;
        sigmaA[at] = s;
        if (count < kSlots)
            ++count;
    }
};

} // namespace nova
