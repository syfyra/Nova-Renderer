#pragma once

// IES photometric profiles (IESNA LM-63) for UsdLuxShapingAPI's
// shaping:ies:file, as Karma renders them (measured with a light over a
// ground, docs/bug-audit-2026-11.md section 22):
//   - the light's emission is multiplied by the candela of the direction it
//     leaves in, the file's value times the file's candela multiplier (lamp
//     count, lumens, ballast and ballast-lamp factors are not applied);
//   - the vertical angle is measured from the light's -Z axis (0: the
//     nadir), the horizontal angle from its -X axis towards -Y;
//   - beyond the file's vertical angles its first or last row holds;
//   - shaping:ies:normalize divides by the mean candela over the file's
//     vertical range (by solid angle): a profile of one value is no change;
//   - shaping:ies:angleScale s reads the table at theta / s for s > 0 and at
//     180 - (180 - theta) / -s for s < 0 (0: unscaled); normalize still
//     divides by the unscaled mean.
// Host-side: the kernels read the resampled table (lights.h, iesFactor).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nova {

struct IesProfile {
    // Candela on a regular grid: `height` rows of vertical angles from 0 (the
    // nadir) to 180 degrees, `width` columns of horizontal angles from 0 to
    // 360 degrees (the last column is the first again); width 1: the same
    // all around. candela[row * width + column].
    uint32_t width = 0, height = 0;
    std::vector<float> candela;
    float mean = 0.0f;       // over the file's vertical range, by solid angle: what normalize divides by
    float sphereMean = 0.0f; // over the whole sphere: the light's power against an unshaped one
    float maxCandela = 0.0f;
    int photometricType = 1; // 1: C (read as such whatever the file says), 2: B, 3: A
};

// The profile of an LM-63 file's text; null with a message in `error` for
// text that is none.
std::shared_ptr<const IesProfile> parseIesProfile(const std::string &text, std::string *error = nullptr);

// The table at a vertical and a horizontal angle in degrees, as the kernels
// read it (bilinear): for tests and the power estimate.
float iesCandela(const IesProfile &profile, float vertical, float horizontal);

} // namespace nova
