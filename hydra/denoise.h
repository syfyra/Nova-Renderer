#pragma once

// Denoising in the delegate (docs/design/M7.md D2, D3): which bound AOVs a
// render denoises, from Nova's settings or Karma's image filter, and the
// denoise of render buffers with the film's guides.

#include "core/film/film.h"

#include <pxr/pxr.h>

#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaRenderBuffer;

struct HdNovaDenoiseRequest {
    bool enabled = false;
    std::vector<std::string> aovs; // bound AOV names (husk names), denoised in place
    bool albedo = true, normal = true; // guides
    std::string engine;                // Karma's engine name ("optix", "oidn"; empty: none named)
    bool operator==(const HdNovaDenoiseRequest &) const = default;
};

// What a render denoises: the final frame (husk, nova_cli) or, in the
// viewport, the displayed beauty after `startPasses` passes.
struct HdNovaDenoiseConfig {
    HdNovaDenoiseRequest final;
    bool viewport = false;
    bool viewportDenoise = false;
    int startPasses = 4;
    // The denoiser to use, for final frames and the viewport: "optix",
    // "oidn", or empty for the automatic choice (nova:denoiser when it names
    // one, else the engine of Karma's image filter).
    std::string engine;
    // Open Image Denoise's quality: 0 fast, 1 balanced, 2 high; -1 by the
    // kind of render (high for final frames, balanced in the viewport).
    int quality = -1;
    // The viewport's guides (nova:denoiseAlbedo, nova:denoiseNormal; the
    // final frame's are in `final`).
    bool viewportAlbedo = true, viewportNormal = true;
    bool operator==(const HdNovaDenoiseConfig &) const = default;
};

// Karma's karma:global:imagefilter, e.g.
// [["denoise", {"engine": "optix", "use_albedo_input": true, "use_n_input": true, "aovs": ["C"]}]]
// (the Karma Render Settings LOP's Denoiser menu; its separate denoised AOVs
// are vars named <name>_denoise in the list). Disabled without a denoise entry.
HdNovaDenoiseRequest HdNovaParseKarmaImageFilter(std::string const &json);

// The "background" entry of Karma's image filters (docs/design/M11-P2.md):
// the Karma Render Settings LOP's "Add Holdout Shadows To Beauty" authors
// [["background", {"destination": "C", "foreground": "C", "shadows":
// "holdout_shadow", "shadowcatcherenable": true, "shadowcatcheralpha": a}]]:
// the destination's alpha takes the shadow AOV times a where the beauty is
// transparent (measured: alpha + (1 - alpha) a shadow, colour unchanged).
// The Background Plate's slap comp (karma:global:background_iprfilter:
// backgroundfile, backgroundscale, backgroundtranslate, extras) is `plate`:
// C + (1 - alpha) (plate (1 - shadows) + extras), alpha 1 (measured, D16).
struct HdNovaBackgroundFilter {
    bool shadowCatcher = false;
    std::string destination = "C", shadows;
    float alpha = 1.0f;
    bool plate = false;                // a background image to composite
    std::string plateFile;             // backgroundfile (relative: to the working directory, as Karma)
    float scale[2] = {1.0f, 1.0f};     // backgroundscale
    float translate[2] = {0.0f, 0.0f}; // backgroundtranslate
    std::vector<std::string> extras;   // AOVs added under the beauty (names)
};
HdNovaBackgroundFilter HdNovaParseKarmaBackgroundFilter(std::string const &json);

// The Background Plate's image (stored values, rows top-down, RGB) and its
// placement over the frame (docs/design/M11-P2.md D16).
struct HdNovaPlate {
    int width = 0, height = 0;
    std::vector<float> rgb;
    float scale[2] = {1.0f, 1.0f}, translate[2] = {0.0f, 0.0f};
    // The stored values are sRGB-encoded (an integer image: 8-bit .pic,
    // .rat, PNG ...): the composite decodes them (Karma, measured).
    bool srgb = false;
    // The texel under NDC (x, y) (y up), as the Background Plate's shader
    // places it: (ndc - 0.5 - translate) / scale + 0.5, nearest, decoded if
    // srgb; black outside the image.
    void lookup(float x, float y, float out[3]) const;
};
// Reads `file` (Houdini .pic and .rat through imaketx, with their stored
// values); null (with `error`) if it cannot.
std::shared_ptr<const HdNovaPlate> HdNovaLoadPlate(HdNovaBackgroundFilter const &filter, std::string &error);

// Splits Nova's nova:denoiseAovs ("C diffuse", commas allowed).
std::vector<std::string> HdNovaSplitAovNames(std::string const &names);

// Denoises `buffers` (float or half, 3 or 4 channels, the film's size) in
// place: the film's beauty is the AOV model's beauty layer, every buffer an
// extra layer with its kernels; the guides are the film's first-hit albedo
// and normal. `engine`: "optix" (NVIDIA OptiX), "oidn" (Intel Open Image
// Denoise) or empty for the automatic choice, OptiX where it can run and
// Open Image Denoise elsewhere; an engine that cannot run gives way to the
// other with one warning. `quality`: Open Image Denoise's filter quality (0
// fast, 1 balanced, 2 high; OptiX has one). False (with one warning per
// process) when no denoiser can run or it fails; the buffers are then
// unchanged.
bool HdNovaDenoiseBuffers(nova::Film const &film, std::vector<HdNovaRenderBuffer *> const &buffers, bool albedo,
                          bool normal, std::string const &engine = std::string(), int quality = 2);

PXR_NAMESPACE_CLOSE_SCOPE
