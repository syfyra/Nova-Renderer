#pragma once

// Houdini's Principled Shader 2.0 natively (docs/design/M5.md D3, 3.3): its
// USD inputs become a MaterialX network around Standard Surface, which the
// translator lowers like any other. The rules follow the shader's behaviour
// (docs/research/m5/vex-materials.md 2):
//   * every texturable value is value * lerp(1, texture, clamp(I, 0, 1)) *
//     clamp(I, 1, 1000) (I = the tint intensity, 1 for floats), floats read
//     a channel (0: luminance with VEX's 0.299/0.587/0.114, 1-3: r, g, b);
//   * the base colour is also multiplied by the Cd (else displayColor)
//     primvar ("Use Point Color", on by default);
//   * texture coordinates are the uv primvar, else st, transformed by the
//     inverse of uvtrans/uvrot/uvscale (scale, rotate, translate);
//   * reflectivity scales the facing reflectance only (grazing stays 1): an
//     effective IOR where nothing refracts; the reflect tint and sheen tint
//     blend towards base / luminance(base);
//   * the diffuse roughness is the specular roughness (VEX diffuse(N, Ng,
//     rough)), anisotropy turns by pi * anisodir (negative: 90 degrees);
//   * metal is Schlick from the base colour to white, the sheen Disney's
//     (1 - |l.h|)^5 (the Standard Surface model's houdiniPrincipled field);
//   * the coat is Disney's clearcoat: IOR 1.5 with a quarter of its facing
//     reflectance times `coat`, roughness 0.1 + 0.45 coatrough;
//   * SSS: albedo = base colour, mean free path = ssscolor * sssdist.
// Displacement: the texture's (value + offset) * scale along N, or for
// "vectordisp" texel * scale as a uv-tangent vector (channel order xyz or
// xzy, no offset); or the disp/vdisp inputs; noise displacement is not
// supported (warning). Normal maps honour Normal Space ("-1 to 1": signed
// texels) and the flips; the decal wrap is a black border.

#include "networkSource.h"

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// Rewrites a "vex" network whose surface (and displacement) shaders are the
// Principled Shader's into the MaterialX network Nova lowers. False when the
// surface terminal is not the Principled Shader.
bool HdNovaRewritePrincipled(HdNovaNetworkSource const &src, HdNovaNetworkSource *out,
                             std::vector<std::string> *warnings);

PXR_NAMESPACE_CLOSE_SCOPE
