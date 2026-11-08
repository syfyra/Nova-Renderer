#include "denoise.h"

#include "diagnostics.h"
#include "renderBuffer.h"
#include "textureConvert.h"

#include "core/denoise/denoiser.h"

#include <OpenImageIO/imageio.h>
#include <pxr/base/js/json.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>

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

HdNovaBackgroundFilter HdNovaParseKarmaBackgroundFilter(std::string const &json) {
    HdNovaBackgroundFilter out;
    if (json.empty())
        return out;
    JsValue root = JsParseString(json);
    // background_iprfilter holds one filter, imagefilter a list of them.
    if (root.IsArray() && !root.GetJsArray().empty() && root.GetJsArray()[0].IsString())
        root = JsValue(JsArray{root});
    if (!root.IsArray())
        return out;
    for (JsValue const &filter : root.GetJsArray()) {
        if (!filter.IsArray() || filter.GetJsArray().size() < 2 || !filter.GetJsArray()[0].IsString() ||
            filter.GetJsArray()[0].GetString() != "background" || !filter.GetJsArray()[1].IsObject())
            continue;
        JsObject const &o = filter.GetJsArray()[1].GetJsObject();
        const auto text = [&](char const *key, std::string const &fallback) {
            const auto it = o.find(key);
            return it != o.end() && it->second.IsString() ? it->second.GetString() : fallback;
        };
        out.destination = text("destination", "C");
        out.shadows = text("shadows", "");
        if (const auto it = o.find("shadowcatcherenable"); it != o.end() && it->second.IsBool())
            out.shadowCatcher = it->second.GetBool() && !out.shadows.empty();
        if (const auto it = o.find("shadowcatcheralpha"); it != o.end() && it->second.IsReal())
            out.alpha = float(it->second.GetReal());
        else if (it != o.end() && it->second.IsInt())
            out.alpha = float(it->second.GetInt());
        out.plateFile = text("backgroundfile", "");
        out.plate = !out.plateFile.empty();
        const auto pair = [&](char const *key, float *v) {
            const auto it = o.find(key);
            if (it == o.end() || !it->second.IsArray() || it->second.GetJsArray().size() != 2)
                return;
            for (int i = 0; i < 2; ++i) {
                JsValue const &e = it->second.GetJsArray()[size_t(i)];
                if (e.IsReal())
                    v[i] = float(e.GetReal());
                else if (e.IsInt())
                    v[i] = float(e.GetInt());
            }
        };
        pair("backgroundscale", out.scale);
        pair("backgroundtranslate", out.translate);
        if (const auto it = o.find("extras"); it != o.end() && it->second.IsArray()) {
            for (JsValue const &e : it->second.GetJsArray())
                if (e.IsString())
                    out.extras.push_back(e.GetString());
        } else {
            out.extras = HdNovaSplitAovNames(text("extras", ""));
        }
        return out;
    }
    return out;
}

void HdNovaPlate::lookup(float x, float y, float out[3], bool decode) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (width <= 0 || height <= 0)
        return;
    const float u = (x - 0.5f - translate[0]) / scale[0] + 0.5f;
    const float v = (y - 0.5f - translate[1]) / scale[1] + 0.5f;
    if (!(u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f))
        return;
    const int px = std::min(int(u * float(width)), width - 1);
    const int py = std::min(int((1.0f - v) * float(height)), height - 1); // rows top-down
    const float *t = rgb.data() + (size_t(py) * size_t(width) + size_t(px)) * 3;
    for (int c = 0; c < 3; ++c) {
        const float s = t[c];
        out[c] = !decode       ? s
                 : s <= 0.04045f ? s / 12.92f
                                 : std::pow((s + 0.055f) / 1.055f, 2.4f);
    }
}

std::shared_ptr<const HdNovaPlate> HdNovaLoadPlate(HdNovaBackgroundFilter const &filter, std::string &error) {
    std::error_code ec;
    const std::filesystem::path file = std::filesystem::absolute(
        std::filesystem::path(std::u8string(filter.plateFile.begin(), filter.plateFile.end())), ec);
    const std::u8string utf8 = file.u8string();
    // .pic and .rat through imaketx (their stored values), others as they are.
    const std::string readable =
        HdNovaReadableTexture(std::string(utf8.begin(), utf8.end()), HdNovaTextureOptions{}, true, error);
    if (readable.empty())
        return nullptr;
    auto in = OIIO::ImageInput::open(readable);
    if (!in) {
        error = OIIO::geterror();
        return nullptr;
    }
    const OIIO::ImageSpec &spec = in->spec();
    auto plate = std::make_shared<HdNovaPlate>();
    plate->width = spec.width;
    plate->height = spec.height;
    std::vector<float> pixels(size_t(spec.width) * size_t(spec.height) * size_t(spec.nchannels));
    if (spec.nchannels < 1 || !in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, pixels.data())) {
        error = in->geterror();
        return nullptr;
    }
    plate->rgb.resize(size_t(spec.width) * size_t(spec.height) * 3);
    for (size_t i = 0, n = size_t(spec.width) * size_t(spec.height); i < n; ++i)
        for (int c = 0; c < 3; ++c)
            plate->rgb[i * 3 + size_t(c)] = pixels[i * size_t(spec.nchannels) + size_t(std::min(c, spec.nchannels - 1))];
    for (int i = 0; i < 2; ++i) {
        plate->scale[i] = filter.scale[i] != 0.0f ? filter.scale[i] : 1.0f;
        plate->translate[i] = filter.translate[i];
    }
    return plate;
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
