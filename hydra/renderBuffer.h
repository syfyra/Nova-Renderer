#pragma once

#include "aov.h"

#include "core/film/film.h"
#include "core/integrator/wavefront.h"

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/pxr.h>

#include <atomic>
#include <cstdint>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaRenderParam;

class HdNovaRenderBuffer final : public HdRenderBuffer {
public:
    HdNovaRenderBuffer(SdfPath const &id, HdNovaRenderParam *renderParam);
    ~HdNovaRenderBuffer() override = default;

    // The render thread writes directly into bound buffers, so it is stopped
    // before a buffer is reallocated or destroyed.
    bool Allocate(GfVec3i const &dimensions, HdFormat format, bool multiSampled) override;
    void Finalize(HdRenderParam *renderParam) override;

    unsigned int GetWidth() const override { return _width; }
    unsigned int GetHeight() const override { return _height; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return false; }

    void *Map() override {
        ++_mappers;
        _mapRequest.store(true);
        return _data.data();
    }
    void Unmap() override { --_mappers; }
    bool IsMapped() const override { return _mappers.load() != 0; }

    // The render thread writes final pixel values, so there is nothing to resolve.
    void Resolve() override {}
    bool IsConverged() const override { return _converged.load(); }

    // --- Nova API (render thread, framebuffer lock held) --------------------
    void SetConverged(bool converged) { _converged.store(converged); }
    // Whether a reader mapped the buffer (or asked for its extra planes)
    // since the last call. AOVs other than the beauty are copied from the
    // film after a pass only then (docs/design/M6.md D11).
    bool TakeMapRequest() { return _mapRequest.exchange(false); }
    // An int buffer whose image is float (husk:format float, Karma's ids):
    // the buffer reports and holds float32, which husk then writes as is.
    void ReportFloatIds() {
        if (_format == HdFormatInt32)
            _format = HdFormatFloat32;
    }
    // Fills every pixel with `clearValue`; a scalar (husk passes the
    // RenderVar's clear value as a float, also for vector buffers) fills
    // every channel.
    void Clear(VtValue const &clearValue);
    // A cryptomatte binding: ceil(rank / 2) extra RGBA planes for the
    // (id, coverage) pairs, which husk asks for through GetResource().
    void SetCrypto(HdNovaAovSource const &source);
    VtValue GetResource(bool multiSampled) const override;
    // Copies the film's pixels inside `region` into this buffer. The
    // first-hit AOVs (depth, normal, albedo, ids) only change in passes that
    // give pixels their first sample, so they are copied only with
    // `firstHitAovs`. After IPR proxy passes (`fillStride` > 1) a pixel
    // without samples shows the rendered pixel of its fillStride block
    // (blocks aligned to fillX0, fillY0; docs/design/M7.md D6).
    void WriteFromFilm(nova::Film const &film, nova::TileRect const &region, HdNovaAovSource const &source,
                       bool firstHitAovs, int fillStride = 1, int fillX0 = 0, int fillY0 = 0);

    // The buffer as RGBA float, rows top-down (RGB buffers: alpha 1), for the
    // denoiser; false for formats it cannot take (ints, fewer than 3 channels).
    bool ReadRgba(std::vector<float> &rgba) const;
    // Writes RGBA float, rows top-down (3-channel buffers take RGB).
    void WriteRgba(const float *rgba);
    // The buffer's pixels as floats, all its channels, in its own (the
    // film's) pixel order; false for int formats. Karma's bake image
    // filters (docs/design/M11-P8.md) run on these.
    bool ReadFloats(std::vector<float> &values, int &channels) const;
    void WriteFloats(const float *values);

    static TfToken const &AlbedoToken();

protected:
    void _Deallocate() override;

private:
    void _WritePixel(size_t index, const float *values, int count);
    void _WritePixel(size_t index, const float *values, int count, HdFormat component, int componentCount);
    void _WritePixelInt(size_t index, int value, bool asFloat);

    HdNovaRenderParam *_renderParam;
    unsigned int _width = 0;
    unsigned int _height = 0;
    HdFormat _format = HdFormatInvalid;
    size_t _pixelSize = 0;
    std::vector<uint8_t> _data;
    std::atomic<int> _mappers{0};
    std::atomic<bool> _converged{false};
    mutable std::atomic<bool> _mapRequest{false};
    // Cryptomatte (SetCrypto): the layer name, rank, sidecar, the names'
    // source and the extra planes (bottom-up RGBA rows, as the buffer).
    std::string _cryptoLayer, _cryptoSidecar, _cryptoSource;
    int _cryptoRank = 0;
    std::vector<std::vector<float>> _cryptoPlanes;
};

PXR_NAMESPACE_CLOSE_SCOPE
