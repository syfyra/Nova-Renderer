#pragma once

#include <cstdint>

namespace nova {

// How direct light is sampled. MIS combines light and BSDF samples with the
// power heuristic; the other two use one strategy alone, for debugging and
// consistency tests. Delta lights are always light-sampled and lights without
// a sampling distribution (constant domes) always BSDF-sampled.
enum class LightStrategy : uint32_t { MIS, LightOnly, BsdfOnly };

} // namespace nova
