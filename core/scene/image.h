#pragma once

#include "core/math/spectrum.h"

#include <vector>

namespace nova {

// Linear RGB image owned by the host side, e.g. a dome light's texture.
// Row 0 is the top row (image file order).
struct Image {
    int width = 0;
    int height = 0;
    std::vector<Spectrum> pixels; // width * height, row-major
};

} // namespace nova
