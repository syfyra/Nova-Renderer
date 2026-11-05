#pragma once

// Texture preparation (docs/research/m3/textures.md 2, 6.2): OIIO's
// TextureSystem filters correctly only linear, tiled, MIP-mapped files, and
// its own color transform is ignored for untiled files and quantizes 8-bit
// data. Anything else is converted once into a half-float, scene-linear,
// tiled (64), MIP-mapped EXR in a disk cache; `.rat` files (no OIIO reader)
// through Houdini's imaketx. A linear gamut change of an otherwise ready
// file stays a 3x3 matrix after the lookup. UDIM sets (<UDIM>, <UVTILE>)
// are converted per tile under one name.
//
// Cache names hash the source path, size and modification time, the color
// space, the transform to the rendering space (OCIO config) and a format
// version, so a changed source converts again and unchanged ones are reused
// across sessions and processes (written to a temporary file, then renamed).
//
// Files inside a package ("a.usdz[tex/b.png]") are extracted through Ar into
// the cache first. A direct (unconverted) UDIM set whose tiles were added or
// removed since OIIO inventoried it in this process is read through hard
// links (copies across volumes) in the cache under a fresh name, since OIIO
// scans a UDIM pattern only once.

#include "colorSpace.h"

#include <pxr/pxr.h>

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

// A texture a material uses: the file and how to read it.
struct HdNovaTextureRequest {
    std::string path;       // resolved; may contain <UDIM> or <UVTILE>
    std::string colorSpace; // "" or "auto": automatic; "raw"; or a color space name
    std::string layer;      // image of a multi-image file ("": the first)

    bool operator==(HdNovaTextureRequest const &o) const {
        return path == o.path && colorSpace == o.colorSpace && layer == o.layer;
    }
};

struct HdNovaTextureOptions {
    bool autoTx = true;          // nova:textureAutoTx
    std::string cacheDir;        // nova:textureCacheDir ("": $HOUDINI_TEMP_DIR/nova_tx, else %TEMP%/nova_tx)
    float blur = 0.0f;           // nova:textureBlur (fraction of the texture)
    float filterWidth = 0.25f;   // nova:textureFilterWidth (footprint scale, M3.9)
    bool abortOnMissing = false; // nova:abortOnMissingTexture
    int stats = 0;               // nova:textureStats

    bool operator==(HdNovaTextureOptions const &o) const {
        return autoTx == o.autoTx && cacheDir == o.cacheDir && blur == o.blur && filterWidth == o.filterWidth &&
               abortOnMissing == o.abortOnMissing && stats == o.stats;
    }
};

// The sizes and modification times of the file or the UDIM tiles (empty if
// none exists): a changed signature means the texture must be prepared again.
std::string HdNovaTextureSignature(std::string const &path);

struct HdNovaPreparedTexture {
    bool found = false;            // the file, or at least one UDIM tile, opens
    bool udim = false;
    bool converted = false;        // lookupPath is a file of the cache
    std::string lookupPath;        // what the TextureSystem reads (may contain <UDIM>)
    std::string colorSpace;        // the source's color space ("": none)
    HdNovaColorTransform transform; // still to apply after the lookup (Identity or Matrix)
    std::string error;             // why !found
};

// `path` may contain <UDIM> or <UVTILE>; `colorSpaceHint` as in
// HdNovaTextureColorSpace; `layer` names an image of a multi-image file.
HdNovaPreparedTexture HdNovaPrepareTexture(std::string const &path, std::string const &colorSpaceHint,
                                           std::string const &layer, HdNovaTextureOptions const &options);

// The cache directory the options select.
std::string HdNovaTextureCacheDir(HdNovaTextureOptions const &options);

// `path` itself, or for a file inside a package (Ar package-relative path,
// e.g. "a.usdz[tex/b.png]") a copy extracted once into the cache directory,
// named by the package's size and modification time. Empty (with `error`)
// when it cannot be extracted.
std::string HdNovaLocalizeTexture(std::string const &path, HdNovaTextureOptions const &options, std::string &error);

PXR_NAMESPACE_CLOSE_SCOPE
