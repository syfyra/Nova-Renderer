#pragma once

// A volume prim (docs/design/M8.md): its fields, placed in the volume's
// object space, and its material. The host loads the fields (Hydra: through
// the volume bridge, D1); the scene builds the majorants at commit().

#include "core/scene/material.h"
#include "core/volume/atmosphere.h"
#include "core/volume/grid.h"
#include "core/volume/shell.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nova {

using VolumeId = uint32_t;

// A constant primvar of a medium bounded by a surface (docs/design/M11-P5.md):
// what a volume shader's read of `name` gives there when no field has it.
struct VolumeConstant {
    std::string name;
    std::vector<float> values; // 1 to 4
};

// A planetary sky atmosphere (docs/design/M11-P7.md, the Karma Sky
// Atmosphere): world-space lengths, coefficients per world unit.
struct AtmosphereDesc {
    double center[3] = {0.0, -6371000.0, 0.0};
    double seaLevel = 6370900.0;
    double groundOffset = 0.0;                   // the ground sphere's radius is seaLevel - groundOffset
    Spectrum rayleigh{6e-6f, 12e-6f, 27e-6f};    // scattering at sea level
    float rayleighHeight = 8000.0f;              // scale height
    Spectrum mieScatter{4e-6f, 4e-6f, 4e-6f};
    Spectrum mieAbsorb{4.5e-6f, 4.5e-6f, 4.5e-6f};
    float mieHeight = 1200.0f;
    float mieG = 0.8f;                           // Henyey-Greenstein anisotropy
    Spectrum ozone{6.5e-7f, 1.88e-6f, 8.6e-8f};  // absorption at the layer's peak
    float ozoneAltitude = 25000.0f, ozoneWidth = 30000.0f; // the tent's centre and full width
    Spectrum groundAlbedo{0.2f, 0.2f, 0.2f};
    uint8_t groundVisibility = 0x7E;             // kVisible* mask (Karma's "-primary")
    bool groundShadows = false;                  // the scene's objects shadow the ground
    bool backside = false;                       // the medium continues behind the planet
    bool ignoreCameraClip = true;                // camera rays see it past the far clipping plane

    // The medium's top above sea level: 7 scale heights of the thicker
    // exponential layer, or the ozone layer's top if higher (Karma, measured).
    float topAltitude() const;
};

struct VolumeField {
    std::string name;      // the binding name (relationship name without "field:", e.g. "vel_x")
    std::string fieldName; // the field's own name (Houdini's "vel.x")
    VolumeGridPtr grid;    // null: not loaded or missing
    Affine3f fieldToObject; // the field prim's transform relative to the volume prim

    // The grid's index space to the volume's object space.
    Affine3f indexToObject() const;
};

struct VolumeDesc {
    std::string name; // for messages (the prim's path)
    std::vector<VolumeField> fields;
    MaterialId material = kDefaultMaterialId; // a volume material; the default: white, density (D6)
    bool visible = true;
    int32_t userId = -1;
    float volumeLimit = -1.0f; // the Volume ray limit at its events (D11); negative: the render setting
    std::string lpeTag;        // karma:object:lpetag (empty: the material's tag)
    // A light source (D13, Karma's treat_as_lightsource): its emission is
    // light-sampled and a light (L) in light path expressions, with the
    // lights' multipliers (lightsource_diffusescale / specularscale).
    bool light = false;
    float lightDiffuse = 1.0f, lightSpecular = 1.0f;
    // Velocity blur by lookup offset (D17): the fields are read at
    // p - v(p) dt, with v the vector field `velocity` (its .x/.y/.z
    // components, object units per second) and dt going linearly from
    // velocityTime[0] at shutter open to velocityTime[1] at close (seconds
    // from the fields' time, times Karma's volumevblurscale).
    std::string velocity;
    float velocityTime[2] = {0.0f, 0.0f};
    // A medium bounded by a surface (docs/design/M11-P5.md: Karma's uniform
    // volumes, the Karma Fog Box): the volume fills the inside of this shell
    // (object space) instead of its fields' domain; it has no fields, its
    // shader's reads take `constants`, else their fallbacks. Without a
    // volume material: white, density 1 (Karma, measured).
    MediumShellPtr shell;
    std::vector<VolumeConstant> constants;
    // A planetary sky atmosphere (docs/design/M11-P7.md): the volume has no
    // fields; the scene's one atmosphere (Scene::atmosphere()) is the first
    // visible volume that carries one. Its record holds the atmosphere's
    // volume limit and LPE tag.
    std::shared_ptr<const AtmosphereDesc> atmosphere;

    // The field a material reads as `name` (D5): a binding name or field
    // name; null if there is none with a grid.
    const VolumeField *findField(const std::string &field) const;
};

// The composition of affine maps: first a, then b.
NOVA_INLINE Affine3f compose(const Affine3f &a, const Affine3f &b) {
    Affine3f r;
    r.x = b.vector(a.x);
    r.y = b.vector(a.y);
    r.z = b.vector(a.z);
    r.t = b.point(a.t);
    return r;
}


inline float AtmosphereDesc::topAltitude() const {
    const float exponential = 7.0f * (rayleighHeight > mieHeight ? rayleighHeight : mieHeight);
    const float layer = ozoneWidth > 0.0f && !isBlack(ozone) ? ozoneAltitude + 0.5f * ozoneWidth : 0.0f;
    return exponential > layer ? exponential : layer;
}

inline Affine3f VolumeField::indexToObject() const {
    return grid ? compose(grid->indexToObject(), fieldToObject) : Affine3f{};
}

inline const VolumeField *VolumeDesc::findField(const std::string &field) const {
    for (const VolumeField &f : fields)
        if (f.grid && f.name == field)
            return &f;
    for (const VolumeField &f : fields)
        if (f.grid && f.fieldName == field)
            return &f;
    return nullptr;
}

} // namespace nova
