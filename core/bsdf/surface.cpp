#include "core/bsdf/surface.h"

#include <algorithm>
#include <iterator>

namespace nova {

namespace {

namespace sf = surface_field;

SurfaceInputs zeroed(SurfaceModel model) {
    SurfaceInputs in{};
    std::fill(std::begin(in.v), std::end(in.v), 0.0f);
    in.model = model;
    return in;
}

} // namespace

// ND_standard_surface_surfaceshader (1.0.1) and _100, MaterialX 1.39.5
// (bxdf/standard_surface.mtlx).
SurfaceInputs standardSurfaceDefaults(bool version100) {
    SurfaceInputs in = zeroed(SurfaceModel::StandardSurface);
    in.set(sf::baseWeight, version100 ? 0.8f : 1.0f);
    in.set(sf::baseColor, spectrum(version100 ? 1.0f : 0.8f));
    in.set(sf::specularWeight, 1.0f);
    in.set(sf::specularColor, spectrum(1.0f));
    in.set(sf::specularRoughness, 0.2f);
    in.set(sf::specularIor, 1.5f);
    in.set(sf::transmissionColor, spectrum(1.0f));
    in.set(sf::subsurfaceColor, spectrum(1.0f));
    in.set(sf::subsurfaceRadius, spectrum(1.0f));
    in.set(sf::subsurfaceScale, 1.0f);
    in.set(sf::sheenColor, spectrum(1.0f));
    in.set(sf::sheenRoughness, 0.3f);
    in.set(sf::coatColor, spectrum(1.0f));
    in.set(sf::coatRoughness, 0.1f);
    in.set(sf::coatIor, 1.5f);
    in.set(sf::thinFilmIor, 1.5f);
    in.set(sf::emissionColor, spectrum(1.0f));
    in.set(sf::opacity, spectrum(1.0f));
    return in;
}

// ND_open_pbr_surface_surfaceshader (1.1.1; bxdf/open_pbr_surface.mtlx).
// subsurfaceRadius holds subsurface_radius_scale, subsurfaceScale
// subsurface_radius (their product is the mean free path in both models);
// emissionWeight is emission_luminance; thinFilmThickness is in micrometers.
SurfaceInputs openPbrDefaults() {
    SurfaceInputs in = zeroed(SurfaceModel::OpenPBR);
    in.set(sf::baseWeight, 1.0f);
    in.set(sf::baseColor, spectrum(0.8f));
    in.set(sf::specularWeight, 1.0f);
    in.set(sf::specularColor, spectrum(1.0f));
    in.set(sf::specularRoughness, 0.3f);
    in.set(sf::specularIor, 1.5f);
    in.set(sf::transmissionColor, spectrum(1.0f));
    in.set(sf::transmissionDispersion, 20.0f); // the Abbe number (scale 0 disables dispersion)
    in.set(sf::subsurfaceColor, spectrum(0.8f));
    in.set(sf::subsurfaceRadius, {1.0f, 0.5f, 0.25f});
    in.set(sf::subsurfaceScale, 1.0f);
    in.set(sf::sheenColor, spectrum(1.0f));
    in.set(sf::sheenRoughness, 0.5f);
    in.set(sf::coatColor, spectrum(1.0f));
    in.set(sf::coatIor, 1.6f);
    in.set(sf::coatDarkening, 1.0f);
    in.set(sf::thinFilmThickness, 0.5f);
    in.set(sf::thinFilmIor, 1.4f);
    in.set(sf::emissionColor, spectrum(1.0f));
    in.set(sf::opacity, spectrum(1.0f));
    return in;
}

// ND_surface_unlit: emission 1, emission_color 1, transmission 0,
// transmission_color 1, opacity 1.
SurfaceInputs unlitDefaults() {
    SurfaceInputs in = zeroed(SurfaceModel::Unlit);
    in.set(sf::emissionWeight, 1.0f);
    in.set(sf::emissionColor, spectrum(1.0f));
    in.set(sf::transmissionColor, spectrum(1.0f));
    in.set(sf::opacity, spectrum(1.0f));
    return in;
}

// UsdPreviewSurface 2.6 (bxdf/usd_preview_surface.mtlx): diffuseColor 0.18,
// roughness 0.5, ior 1.5, clearcoatRoughness 0.01, opacity 1, metallic
// workflow; transmissionWeight is the translator's share of the base that
// the "transparent" opacity mode refracts.
SurfaceInputs usdPreviewDefaults() {
    SurfaceInputs in = zeroed(SurfaceModel::UsdPreview);
    in.set(sf::baseWeight, 1.0f);
    in.set(sf::baseColor, spectrum(0.18f));
    in.set(sf::specularRoughness, 0.5f);
    in.set(sf::specularIor, 1.5f);
    in.set(sf::coatRoughness, 0.01f);
    in.set(sf::emissionWeight, 1.0f);
    in.set(sf::opacity, spectrum(1.0f));
    return in;
}

SurfaceInputs lambertDefaults(Spectrum color) {
    SurfaceInputs in = zeroed(SurfaceModel::Lambert);
    in.set(sf::baseWeight, 1.0f);
    in.set(sf::baseColor, color);
    in.set(sf::opacity, spectrum(1.0f));
    return in;
}

// Houdini's plastic with the values of Karma's material for geometry without
// one (measured on Karma CPU, docs/bug-audit-2026-11.md section 17): it
// reflects 0.4 of the display colour diffusely, and 1/30 in a Phong lobe of
// exponent 5 (plastic's roughness 0.2). The colour is white where the
// geometry has none.
SurfaceInputs plasticDefaults() {
    SurfaceInputs in = zeroed(SurfaceModel::Plastic);
    in.set(sf::baseWeight, 0.4f);
    in.set(sf::baseColor, spectrum(1.0f));
    in.set(sf::specularWeight, 1.0f / 30.0f);
    in.set(sf::specularColor, spectrum(1.0f));
    in.set(sf::specularRoughness, 0.2f);
    in.set(sf::opacity, spectrum(1.0f));
    return in;
}

// Karma Hair (kma_hair, karmaShaderNodes.json; docs/research/m9/curves-hair.md
// 3.1): base 1, baseColor 1, melanin 0.4, redness 0.1, thickness scale 1,
// tint 1, shift 0.03, ior 1.55, roughness 0.3 (azimuthal 0.3), TT / TRT
// variance scales 0.25 / 4, diffuse 0 of colour 1.
SurfaceInputs hairDefaults() {
    SurfaceInputs in = zeroed(SurfaceModel::Hair);
    in.set(sf::hairBase, 1.0f);
    in.set(sf::hairBaseColor, spectrum(1.0f));
    in.set(sf::hairMelanin, 0.4f);
    in.set(sf::hairMelaninRedness, 0.1f);
    in.set(sf::hairThicknessScale, 1.0f);
    in.set(sf::hairTint, spectrum(1.0f));
    in.set(sf::hairShift, 0.03f);
    in.set(sf::hairIor, 1.55f);
    in.set(sf::hairRoughness, 0.3f);
    in.set(sf::hairRoughnessAzimuthal, 0.3f);
    in.set(sf::hairTtScale, 0.25f);
    in.set(sf::hairTrtScale, 4.0f);
    in.set(sf::hairDiffuseColor, spectrum(1.0f));
    in.set(sf::opacity, spectrum(1.0f));
    return in;
}

} // namespace nova
