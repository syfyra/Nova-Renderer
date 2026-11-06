#pragma once

// A volume prim (docs/design/M8.md): its fields, placed in the volume's
// object space, and its material. The host loads the fields (Hydra: through
// the volume bridge, D1); the scene builds the majorants at commit().

#include "core/scene/material.h"
#include "core/volume/grid.h"
#include "core/volume/shell.h"

#include <cstdint>
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
