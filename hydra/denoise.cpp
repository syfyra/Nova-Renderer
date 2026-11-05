#include "denoise.h"

#include "diagnostics.h"
#include "renderBuffer.h"

#include "core/denoise/denoiser.h"

#include <pxr/base/js/json.h>

#include <algorithm>
#include <atomic>
#include <cctype>

PXR_NAMESPACE_OPEN_SCOPE

HdNovaDenoiseRequest HdNovaParseKarmaImageFilter(std::string const &json) {
    HdNovaDenoiseRequest out;
    if (json.empty())
        return out;
    const JsValue root = JsParseString(json);
    if (!root.IsArray())
        return out;
    for (JsValue const &filter : root.GetJsArray()) {
        if (!filter.IsArray() || filter.GetJsArray().size() < 1 || !filter.GetJsArray()[0].IsString() ||
            filter.GetJsArray()[0].GetString() != "denoise")
            continue;
        out.enabled = true;
        out.aovs = {"C"};
        if (filter.GetJsArray().size() < 2 || !filter.GetJsArray()[1].IsObject())
            return out;
        JsObject const &options = filter.GetJsArray()[1].GetJsObject();
        const auto flag = [&](char const *key, bool fallback) {
            const auto it = options.find(key);
            return it != options.end() && it->second.IsBool() ? it->second.GetBool() : fallback;
        };
        out.albedo = flag("use_albedo_input", true);
        out.normal = flag("use_n_input", true);
        if (const auto engine = options.find("engine"); engine != options.end() && engine->second.IsString())
            out.engine = engine->second.GetString();
        if (const auto aovs = options.find("aovs"); aovs != options.end() && aovs->second.IsArray()) {
            out.aovs.clear();
            for (JsValue const &name : aovs->second.GetJsArray())
                if (name.IsString())
                    out.aovs.push_back(name.GetString());
        }
        return out;
    }
    return out;
}

std::vector<std::string> HdNovaSplitAovNames(std::string const &names) {
    std::vector<std::string> out;
    std::string current;
    for (char c : names + " ") {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            if (!current.empty())
                out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    return out;
}

bool HdNovaDenoiseBuffers(nova::Film const &film, std::vector<HdNovaRenderBuffer *> const &buffers, bool albedo,
                          bool normal) {
    if (buffers.empty() || film.width <= 0 || film.height <= 0)
        return true;
    nova::Denoiser &denoiser = nova::sharedDenoiser();
    static std::atomic<bool> warned{false};
    if (!denoiser.available()) {
        if (!warned.exchange(true))
            HD_NOVA_WARN("the OptiX denoiser is unavailable (%s); images stay undenoised", denoiser.message().c_str());
        return false;
    }
    const int w = film.width, h = film.height;
    const size_t pixels = size_t(w) * size_t(h);
    // The beauty and the guides, rows top-down (the film's row 0 is the bottom).
    std::vector<float> beauty(pixels * 4), beautyOut(pixels * 4), albedoImage, normalImage;
    if (albedo)
        albedoImage.resize(pixels * 3);
    if (normal)
        normalImage.resize(pixels * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t src = size_t(y) * size_t(w) + size_t(x), dst = size_t(h - 1 - y) * size_t(w) + size_t(x);
            const float inv = film.count[src] ? 1.0f / float(film.count[src]) : 0.0f;
            const nova::Spectrum c = film.colorSum[src] * inv;
            float *b = beauty.data() + dst * 4;
            b[0] = c.x;
            b[1] = c.y;
            b[2] = c.z;
            b[3] = film.alphaSum[src] * inv;
            if (albedo) {
                albedoImage[dst * 3 + 0] = film.albedo[src].x;
                albedoImage[dst * 3 + 1] = film.albedo[src].y;
                albedoImage[dst * 3 + 2] = film.albedo[src].z;
            }
            if (normal) {
                normalImage[dst * 3 + 0] = film.normal[src].x;
                normalImage[dst * 3 + 1] = film.normal[src].y;
                normalImage[dst * 3 + 2] = film.normal[src].z;
            }
        }
    std::vector<std::vector<float>> inputs, outputs;
    std::vector<HdNovaRenderBuffer *> targets;
    for (HdNovaRenderBuffer *rb : buffers) {
        std::vector<float> rgba;
        if (rb && rb->ReadRgba(rgba) && rgba.size() == pixels * 4) {
            inputs.push_back(std::move(rgba));
            outputs.emplace_back(pixels * 4);
            targets.push_back(rb);
        }
    }
    if (targets.empty())
        return true;
    std::vector<nova::Denoiser::Layer> layers;
    layers.push_back({beauty.data(), beautyOut.data()});
    for (size_t i = 0; i < targets.size(); ++i)
        layers.push_back({inputs[i].data(), outputs[i].data()});
    if (!denoiser.denoise(w, h, layers.data(), int(layers.size()), albedo ? albedoImage.data() : nullptr,
                          normal ? normalImage.data() : nullptr)) {
        if (!warned.exchange(true))
            HD_NOVA_WARN("the OptiX denoiser failed (%s); images stay undenoised", denoiser.message().c_str());
        return false;
    }
    for (size_t i = 0; i < targets.size(); ++i)
        targets[i]->WriteRgba(outputs[i].data());
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
