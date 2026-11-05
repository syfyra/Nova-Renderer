#pragma once

// Interval execution of a program's bytecode (docs/design/M8.md D7): every
// row holds a range [lo, hi] containing every value the row takes when the
// globals and primvars lie in their given ranges. Volume majorants come from
// it: the extinction bound of a majorant cell is the program's scattering
// plus absorption over the cell's field ranges. Operations without a rule
// (textures, ray queries, most procedurals) give [-inf, inf], and so do
// results that would be NaN; the caller treats an unbounded result as "no
// bound".

#include "core/shading/program.h"

#include <cstddef>
#include <limits>

namespace nova::shading {

struct Interval {
    float lo = -std::numeric_limits<float>::infinity();
    float hi = std::numeric_limits<float>::infinity();

    static Interval point(float v) { return {v, v}; }
    static Interval unbounded() { return {}; }
    bool bounded() const { return lo > -std::numeric_limits<float>::infinity() && hi < std::numeric_limits<float>::infinity(); }
    bool contains(float v) const { return lo <= v && v <= hi; }
};

// Runs `count` instructions on interval rows R (the program's rowCount); U is
// the material's uniform block (exact values).
void executeInterval(const Instr *code, size_t count, Interval *R, const float *U);

} // namespace nova::shading
