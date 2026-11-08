#pragma once

// Deep camera maps (Karma's karma:deep products, docs/design/M11-P6.md 6):
// the film's per-sample records as a deep scanline OpenEXR, laid out as
// Karma writes them: R, G, B (float), A (half), RA, GA, BA (half, with
// dcmofsize 3), Z (float), one float channel per extra var value, ZBack.
// Samples are sorted front to back; their alphas are the coverage of the
// pixel's camera samples they hold, so compositing them "over" gives the
// pixel's flat beauty and alpha.

#include "core/film/film.h"
#include "core/math/mat.h"

#include <pxr/pxr.h>

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

struct HdNovaDeepOutput {
    std::string path;
    int opacitySize = 3; // karma:product:dcmofsize: 1 (A) or 3 (A, RA, GA, BA)
    // The extra vars' channel names (one per float of DeepRequest::extras).
    std::vector<std::string> extraChannels;
    // Header metadata, as Karma's: the camera matrices (row-vector
    // convention) and the clipping range.
    bool hasCamera = false;
    nova::Mat4 worldToCamera, worldToNdc;
    float clipNear = 0.0f, clipFar = 0.0f;
    // Where the film lies in the image: its origin (pixels from the top
    // left, rows top-down) and the full image size.
    int originX = 0, originY = 0, fullWidth = 0, fullHeight = 0;
};

// Writes the film's deep plane; false (and `error`) if it cannot.
bool HdNovaWriteDeepImage(nova::Film const &film, HdNovaDeepOutput const &out, std::string &error);

PXR_NAMESPACE_CLOSE_SCOPE
