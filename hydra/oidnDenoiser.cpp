#include "oidnDenoiser.h"

#include <OpenImageDenoise/oidn.h>

#include <cstring>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

PXR_NAMESPACE_OPEN_SCOPE

struct HdNovaOidnDenoiser::Impl {
    mutable std::mutex mutex;
    OIDNDevice device = nullptr;
    bool tried = false;
    std::string message, deviceName;

    // The device's pending error, if any (clears it).
    bool failed() {
        const char *text = nullptr;
        if (oidnGetDeviceError(device, &text) == OIDN_ERROR_NONE)
            return false;
        message = text ? text : "unknown error";
        return true;
    }

    bool create() {
        if (tried)
            return device != nullptr;
        tried = true;
#ifdef _WIN32
        // The library is delay-loaded (cmake/NovaHoudini.cmake): a call into
        // it where the DLL is missing would end the process, so ask first.
        if (!LoadLibraryW(L"OpenImageDenoise.dll")) {
            message = "OpenImageDenoise.dll cannot be loaded";
            return false;
        }
#endif
        device = oidnNewDevice(OIDN_DEVICE_TYPE_DEFAULT);
        if (!device) {
            const char *text = nullptr;
            oidnGetDeviceError(nullptr, &text);
            message = text ? text : "no Open Image Denoise device";
            return false;
        }
        oidnCommitDevice(device);
        if (failed()) {
            oidnReleaseDevice(device);
            device = nullptr;
            return false;
        }
        switch (oidnGetDeviceInt(device, "type")) {
        case OIDN_DEVICE_TYPE_CPU:
            deviceName = "CPU";
            break;
        case OIDN_DEVICE_TYPE_CUDA:
            deviceName = "CUDA";
            break;
        default:
            deviceName = "GPU";
            break;
        }
        return true;
    }
};

HdNovaOidnDenoiser::HdNovaOidnDenoiser() : _m(std::make_unique<Impl>()) {}

HdNovaOidnDenoiser::~HdNovaOidnDenoiser() {
    if (_m->device)
        oidnReleaseDevice(_m->device);
}

HdNovaOidnDenoiser &HdNovaOidnDenoiser::Shared() {
    static HdNovaOidnDenoiser denoiser;
    return denoiser;
}

bool HdNovaOidnDenoiser::Available() {
    std::lock_guard lock(_m->mutex);
    return _m->create();
}

std::string HdNovaOidnDenoiser::Message() const {
    std::lock_guard lock(_m->mutex);
    return _m->message;
}

std::string HdNovaOidnDenoiser::DeviceName() const {
    std::lock_guard lock(_m->mutex);
    return _m->deviceName;
}

bool HdNovaOidnDenoiser::Denoise(int width, int height, nova::Denoiser::Layer const *layers, int layerCount,
                                 float const *albedo, float const *normal, int quality) {
    std::lock_guard lock(_m->mutex);
    if (!_m->create())
        return false;
    if (width <= 0 || height <= 0 || layerCount <= 0)
        return true;
    const size_t w = size_t(width), h = size_t(height), pixels = w * h;
    const size_t rgba = pixels * 4 * sizeof(float), rgb = pixels * 3 * sizeof(float);
    // The library's own buffers: a GPU device cannot read the host's memory.
    OIDNDevice device = _m->device;
    OIDNBuffer color = oidnNewBuffer(device, rgba), output = oidnNewBuffer(device, rgba);
    // A normal guide needs the albedo guide (the library's rule).
    OIDNBuffer albedoBuffer = albedo ? oidnNewBuffer(device, rgb) : nullptr;
    OIDNBuffer normalBuffer = albedo && normal ? oidnNewBuffer(device, rgb) : nullptr;
    OIDNFilter filter = oidnNewFilter(device, "RT");
    const auto release = [&] {
        if (filter)
            oidnReleaseFilter(filter);
        for (OIDNBuffer b : {color, output, albedoBuffer, normalBuffer})
            if (b)
                oidnReleaseBuffer(b);
    };
    if (_m->failed() || !color || !output || !filter) {
        release();
        return false;
    }
    if (albedoBuffer)
        oidnWriteBuffer(albedoBuffer, 0, rgb, albedo);
    if (normalBuffer)
        oidnWriteBuffer(normalBuffer, 0, rgb, normal);
    const size_t wide = 4 * sizeof(float), narrow = 3 * sizeof(float);
    oidnSetFilterImage(filter, "color", color, OIDN_FORMAT_FLOAT3, w, h, 0, wide, wide * w);
    oidnSetFilterImage(filter, "output", output, OIDN_FORMAT_FLOAT3, w, h, 0, wide, wide * w);
    if (albedoBuffer)
        oidnSetFilterImage(filter, "albedo", albedoBuffer, OIDN_FORMAT_FLOAT3, w, h, 0, narrow, narrow * w);
    if (normalBuffer)
        oidnSetFilterImage(filter, "normal", normalBuffer, OIDN_FORMAT_FLOAT3, w, h, 0, narrow, narrow * w);
    oidnSetFilterBool(filter, "hdr", true);
    oidnSetFilterInt(filter, "quality",
                     quality <= 0 ? OIDN_QUALITY_FAST : quality == 1 ? OIDN_QUALITY_BALANCED : OIDN_QUALITY_HIGH);
    oidnCommitFilter(filter);
    if (_m->failed()) {
        release();
        return false;
    }
    // Every layer through the same filter; nothing is written back before all of them succeeded.
    std::vector<std::vector<float>> results(size_t(layerCount), std::vector<float>(pixels * 4));
    for (int i = 0; i < layerCount; ++i) {
        oidnWriteBuffer(color, 0, rgba, layers[i].input);
        oidnExecuteFilter(filter);
        if (_m->failed()) {
            release();
            return false;
        }
        oidnReadBuffer(output, 0, rgba, results[size_t(i)].data());
        if (_m->failed()) {
            release();
            return false;
        }
    }
    release();
    for (int i = 0; i < layerCount; ++i) {
        const float *in = layers[i].input, *denoised = results[size_t(i)].data();
        float *out = layers[i].output;
        for (size_t p = 0; p < pixels; ++p) {
            const float alpha = in[p * 4 + 3]; // copied, not denoised (the input may be the output)
            out[p * 4 + 0] = denoised[p * 4 + 0];
            out[p * 4 + 1] = denoised[p * 4 + 1];
            out[p * 4 + 2] = denoised[p * 4 + 2];
            out[p * 4 + 3] = alpha;
        }
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
