#pragma once

// What a render-buffer binding shows (docs/design/M6.md D1). husk binds one
// buffer per RenderVar under its husk:name and passes the RenderVar's
// source (sourceType lpe/raw/primvar, sourceName) and every authored
// driver:parameters:aov:* attribute in the binding's aovSettings; Solaris'
// viewport binds Hydra's own AOV names. Karma's vocabulary (ray:* sources,
// LPEs) and Hydra's names resolve to the same kinds.

#include "core/film/film.h"

#include <pxr/imaging/hd/aov.h>
#include <pxr/pxr.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

struct HdNovaPlate; // denoise.h

enum class HdNovaAovKind : uint8_t {
    None,       // nothing Nova fills: the buffer keeps its clear value
    Beauty,     // radiance and alpha (C.*, C.*[LO], Hydra color)
    Lpe,        // another light path expression (M6.2)
    Depth,      // planar camera-space depth (Hydra depth/cameraDepth, ray:hitPz)
    PrimId,     // host prim id (Hydra primId, ray:primid)
    InstanceId, // index into the mesh's instances (Hydra instanceId)
    Normal,     // world shading normal at the first hit (Hydra normal, ray:hitN)
    Albedo,     // the first hit's all-lobe albedo, the denoiser guide (Hydra albedo)
    Utility,    // a utility plane (core/film/film.h UtilityKind): Karma's ray:* and primvar sources (M6.4)
    Crypto,     // a cryptomatte (karma:cryptomatte): the preview here, the ranks as extra planes (M6.5)
    SampleCount, // samples per pixel (adaptive sampling, M7.3): Karma's intrinsic ray:pixelsamples
};

struct HdNovaAovSource {
    HdNovaAovKind kind = HdNovaAovKind::None;
    std::string expression; // the LPE (Lpe)
    // An int buffer saved as a float image (husk:format float, Karma's ids):
    // husk copies the buffer's bytes, so the values are written as floats.
    bool intAsFloat = false;
    int output = -1;        // the LPE program's output (Lpe; -1: the expression failed)
    bool invert = false;    // !expression: 1 - value (Lpe)
    int shadowSlot = -1;    // shadow;expression: its lit sum, the value 1 - lit / unoccluded (Lpe)
    // holdout;shadow; expressions: the lit slot of the background points'
    // whole unoccluded light, added to both sums (docs/design/M11-P2.md
    // D14; -1: none).
    int holdoutSlot = -1;
    // Karma's shadow catcher image filter on this beauty (denoise.h
    // HdNovaBackgroundFilter): the shadow AOV's output, lit slot and holdout
    // slot, and the factor its value adds to the alpha (-1: none).
    int catcherOutput = -1, catcherSlot = -1, catcherHoldoutSlot = -1;
    float catcherAlpha = 0.0f;
    // Karma's background plate composite on this beauty (denoise.h
    // HdNovaPlate, docs/design/M11-P2.md D16): the plate, the shadow AOV's
    // output, lit and holdout slots, the extras' outputs.
    std::shared_ptr<const HdNovaPlate> plate;
    int plateShadowOutput = -1, plateShadowSlot = -1, plateHoldoutSlot = -1;
    std::vector<int> plateExtras;
    nova::UtilityRequest utility; // Utility
    int utilityIndex = -1;  // the film's plane (Utility)
    nova::CryptoRequest crypto;   // Crypto
    int cryptoIndex = -1;   // the film's plane (Crypto)
    int cryptoRank = 6;     // karma:cryptomatterank: id/coverage pairs kept
    std::string cryptoSidecar; // karma:cryptomattesidecar: the manifest file (empty: in the header)
    std::string aovName;    // the binding's name: the cryptomatte layer
    std::string describe;   // for messages: "name (sourceType source)"
};

// Resolves a binding; warns once per AOV that Nova does not fill.
HdNovaAovSource HdNovaResolveAov(HdRenderPassAovBinding const &binding);

PXR_NAMESPACE_CLOSE_SCOPE
