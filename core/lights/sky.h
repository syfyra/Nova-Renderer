#pragma once

// Karma's sky dome (the KarmaSkyDomeLight of the Karma Sky Dome Light and
// Karma Physical Sky LOPs, docs/design/M11-P3.md): the Hosek-Wilkie analytic
// sky (Hosek & Wilkie, "An Analytic Model for Full Spectral Sky-Dome
// Radiance", SIGGRAPH 2012; its CIE XYZ data in third_party/hosek_wilkie),
// with the scaling, horizon and ground conventions measured on Karma CPU.
// Nova renders it as a textured dome light: the sky is baked into a latlong
// image (bakeKarmaSky), so importance sampling, MIS, the light tree and the
// GPU path are those of every dome light.

#include "core/math/spectrum.h"
#include "core/math/vec.h"
#include "core/scene/image.h"

#include <memory>

namespace nova {

// The model's state for one turbidity, ground albedo and solar elevation:
// per CIE XYZ channel the nine coefficients of the radiance distribution and
// the overall radiance.
struct HosekSkyState {
    double coefficients[3][9] = {};
    double radiance[3] = {};
};

// Turbidity is clamped to the data's range [1, 10], the albedo (per X, Y, Z
// channel) to [0, 1] and the solar elevation (radians) to [0, pi/2].
HosekSkyState makeHosekSkyState(double turbidity, const double albedo[3], double solarElevation);

// CIE XYZ radiance of the model (W / (m^2 sr): Y * 683 lm/W is luminance) in
// a direction at cosTheta to the zenith (>= 0) and cosGamma to the sun.
void hosekSkyXYZ(const HosekSkyState &state, double cosTheta, double cosGamma, double xyz[3]);

// The inputs of a KarmaSkyDomeLight. Directions are in the light's frame,
// +Y up. Karma's conventions (measured, M11-P3.md section 2):
// - the sky is the model's XYZ times 2 pi / 683, converted to linear Rec.709
//   and then to the rendering space, times the light color;
// - the solar direction is (cos a sin z, sin a, -cos a cos z) for altitude a
//   and azimuth z (azimuth 0 looks down -Z, 90 down +X);
// - the ground albedo feeds the model per channel (R to X, G to Y, B to Z);
// - below the horizon the sky fades into the ground color: directions below
//   altitude min(horizonBlur, 1/64 radian) take the sky of that altitude,
//   blended with smoothstep from altitude -horizonBlur (ground) up to it;
// - the light's colour temperature and normalize do not change the sky.
struct KarmaSkyDesc {
    float solarAltitude = 45.0f;             // degrees, clamped to [0, 90]
    float solarAzimuth = 0.0f;               // degrees
    float turbidity = 3.0f;
    float horizonBlur = 0.5f;                // degrees
    Spectrum groundAlbedo{0.5f, 0.5f, 0.5f};
    Spectrum groundColor{0.0f, 0.0f, 0.0f};  // radiance below the horizon
    // Karma's computegroundcolor: true takes groundColor as it is (the
    // Physical Sky LOP computes it), false scales it by the light color.
    bool computeGroundColor = true;
    Spectrum lightColor{1.0f, 1.0f, 1.0f};   // color * intensity * 2^exposure
    // Linear Rec.709 to the rendering space (row-major), for the sky only.
    float skyToRendering[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

// Whether two descriptions give the same sky (exact comparison).
bool operator==(const KarmaSkyDesc &a, const KarmaSkyDesc &b);

// Radiance arriving from direction `dir` (any length, light frame).
Spectrum karmaSkyRadiance(const KarmaSkyDesc &desc, Vec3f dir);

// The sky as a latlong image of width x width / 2 texels in the dome
// mapping of core/lights/lights.h (texel centers; the horizon lies between
// two rows). Rows are evaluated in parallel.
std::shared_ptr<Image> bakeKarmaSky(const KarmaSkyDesc &desc, int width);

} // namespace nova
