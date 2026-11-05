#pragma once

// Denoising in the delegate (docs/design/M7.md D2, D3): which bound AOVs a
// render denoises, from Nova's settings or Karma's image filter, and the
// denoise of render buffers with the film's guides.

#include "core/film/film.h"

#include <pxr/pxr.h>

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaRenderBuffer;

struct HdNovaDenoiseRequest {
    bool enabled = false;
    std::vector<std::string> aovs; // bound AOV names (husk names), denoised in place
    bool albedo = true, normal = true; // guides
    std::string engine;                // Karma's engine name ("optix", "oidn")
    bool operator==(const HdNovaDenoiseRequest &) const = default;
};

// What a render denoises: the final frame (husk, nova_cli) or, in the
// viewport, the displayed beauty after `startPasses` passes.
struct HdNovaDenoiseConfig {
    HdNovaDenoiseRequest final;
    bool viewport = false;
    bool viewportDenoise = false;
    int startPasses = 4;
    bool operator==(const HdNovaDenoiseConfig &) const = default;
};

// Karma's karma:global:imagefilter, e.g.
// [["denoise", {"engine": "optix", "use_albedo_input": true, "use_n_input": true, "aovs": ["C"]}]]
// (the Karma Render Settings LOP's Denoiser menu; its separate denoised AOVs
// are vars named <name>_denoise in the list). Disabled without a denoise entry.
HdNovaDenoiseRequest HdNovaParseKarmaImageFilter(std::string const &json);

// Splits Nova's nova:denoiseAovs ("C diffuse", commas allowed).
std::vector<std::string> HdNovaSplitAovNames(std::string const &names);

// Denoises `buffers` (float or half, 3 or 4 channels, the film's size) in
// place: the film's beauty is the AOV model's beauty layer, every buffer an
// extra layer with its kernels; the guides are the film's first-hit albedo
// and normal. False (with one warning per process) when the denoiser is
// unavailable or fails; the buffers are then unchanged.
bool HdNovaDenoiseBuffers(nova::Film const &film, std::vector<HdNovaRenderBuffer *> const &buffers, bool albedo,
                          bool normal);

PXR_NAMESPACE_CLOSE_SCOPE
