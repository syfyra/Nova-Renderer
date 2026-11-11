#pragma once

// The device code embedded in nova_core (docs/design/M10.md D2), generated
// by core/gpu/embed.cmake: the OptiX programs (OptiX-IR, core/gpu/programs.cu)
// and the wavefront kernels (a fatbin, core/gpu/kernels.cu).

#include <cstddef>

namespace nova::gpu {

extern const unsigned long long kNovaProgramsIr_words[];
extern const std::size_t kNovaProgramsIr_size;
extern const unsigned long long kNovaKernels_words[];
extern const std::size_t kNovaKernels_size;
// In the tests only (tests/CMakeLists.txt): the kernels as GPUs before the
// RTX 50 series get them, assembled for the test machine's GPU.
extern const unsigned long long kNovaKernelsOld_words[];
extern const std::size_t kNovaKernelsOld_size;

} // namespace nova::gpu
