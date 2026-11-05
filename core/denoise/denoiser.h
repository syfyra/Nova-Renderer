#pragma once

// The NVIDIA OptiX denoiser (docs/design/M7.md D1, docs/research/m7/denoising.md).
//
// HDK- and CUDA-free interface: the driver's nvcuda.dll and nvoptix.dll are
// loaded at run time, so Nova loads and renders on machines without an
// NVIDIA driver; there the denoiser reports itself unavailable. The AOV
// model denoises the beauty and further layers (LPE AOVs) with the beauty's
// kernels; alpha is copied, not denoised; the guides are the first-hit
// albedo and the world normal. Whole frames when the GPU's free memory
// allows, else tiles with OptiX's full overlap.

#include <memory>
#include <string>

namespace nova {

class Denoiser {
public:
    // One image of width x height RGBA float pixels, rows top-down.
    struct Layer {
        const float *input = nullptr;
        float *output = nullptr; // may equal input
    };

    Denoiser();
    ~Denoiser();
    Denoiser(const Denoiser &) = delete;
    Denoiser &operator=(const Denoiser &) = delete;

    // Whether the denoiser can run; the first call creates the CUDA and
    // OptiX contexts (NOVA_DENOISE_DISABLE=1 makes it unavailable).
    bool available();
    // Denoises layers[0] (the beauty) and the further layers. albedo and
    // normal are RGB float guides (rows top-down) or null. False on failure
    // (message() says why); the outputs are then unchanged. Thread-safe.
    bool denoise(int width, int height, const Layer *layers, int layerCount, const float *albedo,
                 const float *normal);
    // Why the denoiser is unavailable or the last call failed.
    std::string message() const;
    // Tests and memory pressure: the largest tile edge (0: whole frames
    // while the free GPU memory allows).
    void setMaxTile(int pixels);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// The process's denoiser: the OptiX context is created once and kept.
Denoiser &sharedDenoiser();

} // namespace nova
