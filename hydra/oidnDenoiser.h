#pragma once

// Intel Open Image Denoise, Nova's second denoiser (docs/design/M7.md D1 as
// revised for 0.2.0): the library Houdini ships (2.3, with its CPU and CUDA
// devices), so it lives in the delegate, not in the Houdini-free core. It
// denoises on machines without an NVIDIA GPU, where the OptiX denoiser is
// unavailable, and wherever nova:denoiser or Karma's Denoiser menu asks for
// it.
//
// The interface is the OptiX denoiser's (core/denoise/denoiser.h): RGBA float
// layers, rows in any order, alpha copied, the guides the first-hit albedo
// and the world normal. Every layer is filtered with the "RT" filter and the
// same guides. OpenImageDenoise.dll is delay-loaded: a render that does not
// denoise with it never loads it.

#include "core/denoise/denoiser.h"

#include <pxr/pxr.h>

#include <memory>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaOidnDenoiser {
public:
    HdNovaOidnDenoiser();
    ~HdNovaOidnDenoiser();
    HdNovaOidnDenoiser(HdNovaOidnDenoiser const &) = delete;
    HdNovaOidnDenoiser &operator=(HdNovaOidnDenoiser const &) = delete;

    // The process's denoiser: the device is created once and kept.
    static HdNovaOidnDenoiser &Shared();

    // Whether it can run; the first call creates the device (the fastest the
    // library supports here: an NVIDIA GPU through CUDA, else the CPU).
    bool Available();
    // Denoises every layer. `quality`: the library's filter quality, 0 fast,
    // 1 balanced, 2 (and above) high. False on failure (Message() says why);
    // the outputs are then unchanged. Thread-safe.
    bool Denoise(int width, int height, nova::Denoiser::Layer const *layers, int layerCount, float const *albedo,
                 float const *normal, int quality);
    // Why it is unavailable or the last call failed.
    std::string Message() const;
    // "CPU" or "CUDA" (empty before the device exists).
    std::string DeviceName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _m;
};

PXR_NAMESPACE_CLOSE_SCOPE
