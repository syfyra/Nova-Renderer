#pragma once

// Texture color spaces, shared by light and surface textures
// (docs/research/m3/textures.md 4): which space a file is in (Karma's
// rules) and how it reaches Nova's rendering space, OCIO's scene_linear role
// (Linear Rec.709 in Houdini's config, ACEScg in ACES configs).

#include <OpenImageIO/imageio.h>

#include <pxr/pxr.h>

#include <cstddef>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

// The color space of a texture file; empty means no conversion (data, or
// already scene-linear in Houdini's default config). `hint` is the authored
// source color space: empty or "auto" for Karma's automatic rules, "raw"
// (also "data", "none") for none, else a color space name. Automatic rules:
// 1- and 2-channel images are linear; then the OCIO config's file rules
// (only real ones: a path that only matches the Default rule has none), the
// file's metadata if it names a known space, and the bit depth (8-bit data
// is sRGB, anything else linear).
std::string HdNovaTextureColorSpace(std::string const &path, OIIO::ImageSpec const &spec,
                                    std::string const &hint = std::string());

// True when the hint or a real file rule decides the color space of `path`,
// not the file's own encoding (metadata, format, bit depth). Such a `.rat`
// file is read with its stored values (HdNovaReadableTexture).
bool HdNovaTextureColorSpaceIsNamed(std::string const &path, std::string const &hint = std::string());

// How a color space reaches scene_linear.
struct HdNovaColorTransform {
    enum class Kind { Identity, Matrix, Nonlinear };
    Kind kind = Kind::Identity;
    float matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // Matrix: rgb' = M rgb (row-major)
};
HdNovaColorTransform HdNovaGetColorTransform(std::string const &colorSpace);

// Converts `count` pixels of `stride` floats (RGB in the first three) from
// `colorSpace` to scene_linear in place. `path` names the texture in
// warnings.
void HdNovaConvertToSceneLinear(std::string const &path, std::string const &colorSpace, float *pixels, size_t count,
                                size_t stride);

// An id of the exact transform HdNovaConvertToSceneLinear applies to
// `colorSpace` (the OCIO config and its scene_linear role included), so files
// holding converted pixels are not reused under another rendering space.
std::string HdNovaSceneLinearTransformId(std::string const &colorSpace);

PXR_NAMESPACE_CLOSE_SCOPE
