#include "renderBuffer.h"

#include "diagnostics.h"
#include "renderParam.h"

#include <UT/UT_HUSDExtraAOVResource.h>

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/imaging/hd/tokens.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

HdNovaRenderBuffer::HdNovaRenderBuffer(SdfPath const &id, HdNovaRenderParam *renderParam)
    : HdRenderBuffer(id), _renderParam(renderParam) {}

TfToken const &HdNovaRenderBuffer::AlbedoToken() {
    static const TfToken token("albedo");
    return token;
}

void HdNovaRenderBuffer::Finalize(HdRenderParam *renderParam) {
    if (_renderParam)
        _renderParam->StopRenderForEdit();
    HdRenderBuffer::Finalize(renderParam); // deallocates
}

bool HdNovaRenderBuffer::Allocate(GfVec3i const &dimensions, HdFormat format, bool /*multiSampled*/) {
    if (_renderParam)
        _renderParam->StopRenderForEdit();
    _Deallocate();
    if (dimensions[0] < 0 || dimensions[1] < 0 || dimensions[2] != 1 || format == HdFormatInvalid) {
        HD_NOVA_WARN("unsupported render buffer %s (%dx%dx%d)", GetId().GetText(), dimensions[0], dimensions[1],
                dimensions[2]);
        return false;
    }
    _width = unsigned(dimensions[0]);
    _height = unsigned(dimensions[1]);
    _format = format;
    _pixelSize = HdDataSizeOfFormat(format);
    _data.assign(size_t(_width) * _height * _pixelSize, 0);
    _converged = false;
    return true;
}

void HdNovaRenderBuffer::SetCrypto(HdNovaAovSource const &source) {
    _cryptoLayer = source.aovName;
    _cryptoSidecar = source.cryptoSidecar;
    _cryptoRank = source.cryptoRank;
    _cryptoSource = source.crypto.source == nova::CryptoSource::Object     ? "__name"
                    : source.crypto.source == nova::CryptoSource::Material ? "__materialname"
                                                                           : source.crypto.primvar;
    _cryptoPlanes.assign(size_t((_cryptoRank + 1) / 2), std::vector<float>(size_t(_width) * _height * 4, 0.0f));
}

namespace {

std::string jsonString(std::string const &s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

} // namespace

VtValue HdNovaRenderBuffer::GetResource(bool multiSampled) const {
    _mapRequest.store(true);
    if (_cryptoPlanes.empty())
        return HdRenderBuffer::GetResource(multiSampled);
    // husk writes the extra planes after this buffer's own part and merges
    // the metadata into the product's header (docs/research/m6/cryptomatte.md 4).
    auto extra = std::make_shared<UT_HUSDExtraAOVResource>();
    extra->myMap = [this](int i) -> void * {
        return const_cast<float *>(_cryptoPlanes[size_t(i)].data());
    };
    extra->myUnmap = [](int) {};
    char digits[16];
    for (size_t k = 0; k < _cryptoPlanes.size(); ++k) {
        std::snprintf(digits, sizeof(digits), "%02d", int(k));
        extra->addPlane(_cryptoLayer + digits, int(HdFormatFloat32Vec4));
    }
    // The manifest: the names of the ids in the image.
    std::set<uint32_t> ids;
    for (std::vector<float> const &plane : _cryptoPlanes)
        for (size_t i = 0; i + 3 < plane.size(); i += 4)
            for (int pair = 0; pair < 2; ++pair)
                if (plane[i + size_t(2 * pair + 1)] > 0.0f) {
                    uint32_t id;
                    std::memcpy(&id, &plane[i + size_t(2 * pair)], sizeof(id));
                    ids.insert(id);
                }
    std::string manifest = "{";
    for (const uint32_t id : ids) {
        const std::string name = _renderParam ? _renderParam->GetCryptoNames().Find(_cryptoSource, id) : std::string();
        if (name.empty())
            continue;
        std::snprintf(digits, sizeof(digits), "%08x", id);
        manifest += (manifest.size() > 1 ? "," : "") + jsonString(name) + ":" + jsonString(digits);
    }
    manifest += "}";
    std::snprintf(digits, sizeof(digits), "%08x", nova::murmurHash3(_cryptoLayer.data(), _cryptoLayer.size()));
    const std::string key = "cryptomatte/" + std::string(digits, 7) + "/";
    extra->addMetadata(key + "name", _cryptoLayer);
    extra->addMetadata(key + "hash", "MurmurHash3_32");
    extra->addMetadata(key + "conversion", "uint32_to_float32");
    extra->addMetadata(key + "manifest", manifest);
    if (!_cryptoSidecar.empty()) // husk writes the file and keeps only the name
        extra->addMetadata(key + "manif_file", _cryptoSidecar);
    HdAovSettingsMap map;
    map[TfToken("extra_aov_resource")] = VtValue(std::static_pointer_cast<void>(extra));
    return VtValue(map);
}

void HdNovaRenderBuffer::_Deallocate() {
    _width = _height = 0;
    _format = HdFormatInvalid;
    _pixelSize = 0;
    _data.clear();
    _data.shrink_to_fit();
    _mappers = 0;
    _converged = false;
    _cryptoPlanes.clear();
    _cryptoRank = 0;
}

void HdNovaRenderBuffer::_WritePixel(size_t index, const float *values, int count) {
    _WritePixel(index, values, count, HdGetComponentFormat(_format), int(HdGetComponentCount(_format)));
}

void HdNovaRenderBuffer::_WritePixel(size_t index, const float *values, int count, HdFormat component,
                                     int componentCount) {
    uint8_t *dst = _data.data() + index * _pixelSize;
    const int n = std::min(count, componentCount);
    for (int c = 0; c < n; ++c) {
        switch (component) {
        case HdFormatFloat32:
            reinterpret_cast<float *>(dst)[c] = values[c];
            break;
        case HdFormatFloat16:
            reinterpret_cast<GfHalf *>(dst)[c] = GfHalf(values[c]);
            break;
        case HdFormatUNorm8:
            dst[c] = uint8_t(std::clamp(values[c], 0.0f, 1.0f) * 255.0f + 0.5f);
            break;
        case HdFormatSNorm8:
            reinterpret_cast<int8_t *>(dst)[c] = int8_t(std::clamp(values[c], -1.0f, 1.0f) * 127.0f);
            break;
        case HdFormatInt32:
            reinterpret_cast<int32_t *>(dst)[c] = int32_t(values[c]);
            break;
        default:
            break;
        }
    }
}

bool HdNovaRenderBuffer::ReadRgba(std::vector<float> &rgba) const {
    const HdFormat component = HdGetComponentFormat(_format);
    const int n = int(HdGetComponentCount(_format));
    if ((component != HdFormatFloat32 && component != HdFormatFloat16) || n < 3 || _data.empty())
        return false;
    const size_t w = _width, h = _height;
    rgba.resize(w * h * 4);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) {
            const uint8_t *p = _data.data() + (y * w + x) * _pixelSize;
            float *out = rgba.data() + ((h - 1 - y) * w + x) * 4;
            for (int c = 0; c < 4; ++c)
                out[c] = c >= n                         ? 1.0f
                         : component == HdFormatFloat32 ? reinterpret_cast<const float *>(p)[c]
                                                        : float(reinterpret_cast<const GfHalf *>(p)[c]);
        }
    return true;
}

bool HdNovaRenderBuffer::ReadFloats(std::vector<float> &values, int &channels) const {
    const HdFormat component = HdGetComponentFormat(_format);
    channels = int(HdGetComponentCount(_format));
    if ((component != HdFormatFloat32 && component != HdFormatFloat16) || _data.empty())
        return false;
    const size_t count = size_t(_width) * _height;
    values.resize(count * size_t(channels));
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *p = _data.data() + i * _pixelSize;
        for (int c = 0; c < channels; ++c)
            values[i * size_t(channels) + size_t(c)] = component == HdFormatFloat32
                                                           ? reinterpret_cast<const float *>(p)[c]
                                                           : float(reinterpret_cast<const GfHalf *>(p)[c]);
    }
    return true;
}

void HdNovaRenderBuffer::WriteFloats(const float *values) {
    const HdFormat component = HdGetComponentFormat(_format);
    const int n = int(HdGetComponentCount(_format));
    const size_t count = size_t(_width) * _height;
    for (size_t i = 0; i < count; ++i)
        _WritePixel(i, values + i * size_t(n), n, component, n);
}

void HdNovaRenderBuffer::WriteRgba(const float *rgba) {
    const HdFormat component = HdGetComponentFormat(_format);
    const int n = int(HdGetComponentCount(_format));
    const size_t w = _width, h = _height;
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x)
            _WritePixel(y * w + x, rgba + ((h - 1 - y) * w + x) * 4, std::min(n, 4), component, n);
}

void HdNovaRenderBuffer::_WritePixelInt(size_t index, int value, bool asFloat) {
    if (HdGetComponentFormat(_format) == HdFormatInt32 && asFloat) {
        const float f = float(value);
        std::memcpy(_data.data() + index * _pixelSize, &f, sizeof(float));
    } else if (HdGetComponentFormat(_format) == HdFormatInt32) {
        std::memcpy(_data.data() + index * _pixelSize, &value, sizeof(int32_t));
    } else {
        const float f = float(value);
        _WritePixel(index, &f, 1);
    }
}

void HdNovaRenderBuffer::Clear(VtValue const &clearValue) {
    const size_t count = size_t(_width) * _height;
    if (count == 0 || clearValue.IsEmpty()) // an empty clear value means "do not clear"
        return;

    // husk passes the clear value in the RenderVar's data type (color4h,
    // half, ...), Solaris mostly as float vectors.
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int n = 0;
    const auto take = [&](auto const &vec, int size) {
        for (int c = 0; c < size; ++c)
            v[c] = float(vec[c]);
        n = size;
    };
    if (clearValue.IsHolding<GfVec4f>())
        take(clearValue.UncheckedGet<GfVec4f>(), 4);
    else if (clearValue.IsHolding<GfVec4d>())
        take(clearValue.UncheckedGet<GfVec4d>(), 4);
    else if (clearValue.IsHolding<GfVec4h>())
        take(clearValue.UncheckedGet<GfVec4h>(), 4);
    else if (clearValue.IsHolding<GfVec3f>())
        take(clearValue.UncheckedGet<GfVec3f>(), 3);
    else if (clearValue.IsHolding<GfVec3d>())
        take(clearValue.UncheckedGet<GfVec3d>(), 3);
    else if (clearValue.IsHolding<GfVec3h>())
        take(clearValue.UncheckedGet<GfVec3h>(), 3);
    else if (clearValue.IsHolding<GfVec2f>())
        take(clearValue.UncheckedGet<GfVec2f>(), 2);
    else if (clearValue.IsHolding<GfVec2d>())
        take(clearValue.UncheckedGet<GfVec2d>(), 2);
    else if (clearValue.IsHolding<GfVec2h>())
        take(clearValue.UncheckedGet<GfVec2h>(), 2);
    else if (clearValue.IsHolding<float>())
        v[0] = clearValue.UncheckedGet<float>(), n = 1;
    else if (clearValue.IsHolding<double>())
        v[0] = float(clearValue.UncheckedGet<double>()), n = 1;
    else if (clearValue.IsHolding<GfHalf>())
        v[0] = float(clearValue.UncheckedGet<GfHalf>()), n = 1;
    else if (clearValue.IsHolding<int>() || clearValue.IsHolding<int64_t>() || clearValue.IsHolding<unsigned int>()) {
        const int iv = clearValue.IsHolding<int>()       ? clearValue.UncheckedGet<int>()
                       : clearValue.IsHolding<int64_t>() ? int(clearValue.UncheckedGet<int64_t>())
                                                         : int(clearValue.UncheckedGet<unsigned int>());
        for (size_t i = 0; i < count; ++i)
            _WritePixelInt(i, iv, false);
        return;
    } else {
        HD_NOVA_WARN("unsupported clear value type '%s' for %s, clearing to zero",
                clearValue.GetTypeName().c_str(), GetId().GetText());
        std::fill(_data.begin(), _data.end(), uint8_t(0));
        return;
    }
    if (n == 1) // a scalar fills every channel
        for (int c = 1; c < 4; ++c)
            v[c] = v[0];
    _WritePixel(0, v, n == 1 ? 4 : n);
    for (size_t i = 1; i < count; ++i)
        std::memcpy(_data.data() + i * _pixelSize, _data.data(), _pixelSize);
}

void HdNovaRenderBuffer::WriteFromFilm(nova::Film const &film, nova::TileRect const &region,
                                       HdNovaAovSource const &source, bool firstHitAovs, int fillStride,
                                       int fillX0, int fillY0) {
    if (int(_width) != film.width || int(_height) != film.height || film.samples == 0)
        return;

    using Kind = HdNovaAovKind;
    const Kind kind = source.kind;
    if (kind == Kind::None)
        return; // keeps the clear value
    if (kind == Kind::Lpe && (source.output < 0 || uint32_t(source.output) >= film.lpeOutputs))
        return; // the expression failed: keeps the clear value
    // Accumulated kinds change every pass, the others after the first.
    const bool perPass = kind == Kind::Beauty || kind == Kind::Lpe || kind == Kind::Crypto || kind == Kind::SampleCount ||
                         (kind == Kind::Utility && (source.utility.kind == nova::UtilityKind::Ao || source.perSample ||
                                                    source.utility.filter != nova::SampleFilter::Center));
    if (kind == Kind::Crypto && (source.cryptoIndex < 0 || size_t(source.cryptoIndex) >= film.crypto.size()))
        return;
    if (!perPass && !firstHitAovs)
        return; // unchanged since the first pass
    const nova::UtilityPlane *plane = kind == Kind::Utility && source.utilityIndex >= 0 &&
                                              size_t(source.utilityIndex) < film.utility.size()
                                          ? &film.utility[size_t(source.utilityIndex)]
                                          : nullptr;
    if (kind == Kind::Utility && !plane)
        return;

    const HdFormat component = HdGetComponentFormat(_format);
    const int componentCount = int(HdGetComponentCount(_format));
    for (int y = region.y0; y < region.y1; ++y) {
        for (int x = region.x0; x < region.x1; ++x) {
            const size_t i = size_t(y) * size_t(film.width) + size_t(x);
            // Sums are normalized by the pixel's own count (docs/design/M7.md
            // D5). A pixel without samples shows its proxy block's pixel
            // (D6), or keeps the buffer's value.
            size_t src = i;
            if (film.count[i] == 0) {
                if (fillStride <= 1)
                    continue;
                const int bx = fillX0 + (x - fillX0) / fillStride * fillStride;
                const int by = fillY0 + (y - fillY0) / fillStride * fillStride;
                src = size_t(by) * size_t(film.width) + size_t(bx);
                if (film.count[src] == 0)
                    continue;
            }
            const float inv = 1.0f / float(film.count[src]);
            switch (kind) {
            case Kind::Beauty: {
                const nova::Spectrum c = film.colorSum[src] * inv;
                float alpha = film.alphaSum[src] * inv;
                if (source.catcherOutput >= 0 && uint32_t(source.catcherSlot) < film.lpeShadowOutputs) {
                    // Karma's shadow catcher filter (denoise.h): the holdout
                    // shadow where the beauty is transparent.
                    const nova::Spectrum u = film.lpe(uint32_t(source.catcherOutput))[src];
                    const nova::Spectrum l = film.lpeLitSum(uint32_t(source.catcherSlot))[src];
                    const float all = u.x + u.y + u.z, lit = l.x + l.y + l.z;
                    const float shadow = all > 0.0f ? std::clamp(1.0f - lit / all, 0.0f, 1.0f) : 0.0f;
                    alpha += (1.0f - alpha) * shadow * source.catcherAlpha;
                }
                const float v[4] = {c.x, c.y, c.z, alpha};
                _WritePixel(i, v, 4, component, componentCount);
                break;
            }
            case Kind::Lpe: {
                // Filtered like the beauty; 4 channels take the beauty's alpha.
                nova::Spectrum c = film.lpe(uint32_t(source.output))[src] * inv;
                if (source.shadowSlot >= 0 && uint32_t(source.shadowSlot) < film.lpeShadowOutputs) {
                    // The occluded fraction (Karma's shadow AOVs).
                    const nova::Spectrum u = film.lpe(uint32_t(source.output))[src];
                    const nova::Spectrum l = film.lpeLitSum(uint32_t(source.shadowSlot))[src];
                    const auto fraction = [](float lit, float all) { return all > 0.0f ? 1.0f - lit / all : 0.0f; };
                    c = nova::Spectrum{fraction(l.x, u.x), fraction(l.y, u.y), fraction(l.z, u.z)};
                }
                if (source.invert)
                    c = nova::Spectrum{1.0f - c.x, 1.0f - c.y, 1.0f - c.z};
                const float v[4] = {c.x, c.y, c.z, film.alphaSum[src] * inv};
                _WritePixel(i, v, 4, component, componentCount);
                break;
            }
            case Kind::Depth:
                _WritePixel(i, &film.depth[src], 1, component, componentCount);
                break;
            // Float images (Karma's ids) keep their clear value on the
            // background; Hydra's int ids get -1 there.
            case Kind::PrimId:
                if (!source.intAsFloat || film.primId[src] >= 0)
                    _WritePixelInt(i, film.primId[src], source.intAsFloat);
                break;
            case Kind::InstanceId:
                if (!source.intAsFloat || film.instanceId[src] >= 0)
                    _WritePixelInt(i, film.instanceId[src], source.intAsFloat);
                break;
            case Kind::Normal:
                _WritePixel(i, &film.normal[src].x, 3, component, componentCount);
                break;
            case Kind::Albedo:
                _WritePixel(i, &film.albedo[src].x, 3, component, componentCount);
                break;
            case Kind::Crypto: {
                // The pairs by coverage, descending; the preview colour over all.
                const nova::CryptoPlane &cp = film.crypto[size_t(source.cryptoIndex)];
                std::pair<float, uint32_t> pairs[nova::kCryptoSlots];
                int n = 0;
                for (int s = 0; s < nova::kCryptoSlots; ++s) {
                    const float w = cp.weights[src * nova::kCryptoSlots + size_t(s)];
                    if (w > 0.0f)
                        pairs[n++] = {w * inv, cp.ids[src * nova::kCryptoSlots + size_t(s)]};
                }
                std::sort(pairs, pairs + n, [](auto const &a, auto const &b) { return a.first > b.first; });
                nova::Vec3f preview{0.0f, 0.0f, 0.0f};
                for (int s = 0; s < n; ++s)
                    preview = preview + nova::cryptoColor(pairs[s].second) * pairs[s].first;
                _WritePixel(i, &preview.x, 3, component, componentCount);
                for (int r = 0; r < _cryptoRank && size_t(r / 2) < _cryptoPlanes.size(); ++r) {
                    std::vector<float> &plane = _cryptoPlanes[size_t(r / 2)];
                    if (plane.size() < (i + 1) * 4)
                        break;
                    float *e = plane.data() + i * 4 + size_t(2 * (r % 2));
                    e[0] = r < n ? nova::cryptoFloat(pairs[r].second) : 0.0f;
                    e[1] = r < n ? pairs[r].first : 0.0f;
                }
                break;
            }
            case Kind::SampleCount:
                // Nova counts every sample (Karma only those that hit geometry).
                _WritePixelInt(i, int(film.count[src]), source.intAsFloat || component != HdFormatInt32);
                break;
            case Kind::Utility: {
                const float *v = plane->data.data() + src * plane->width;
                if (source.perSample) {
                    // A bake's maps: coverage averages (docs/design/M11-P8.md).
                    float o[4];
                    for (int c = 0; c < plane->width && c < 4; ++c)
                        o[c] = v[c] * inv;
                    _WritePixel(i, o, std::min(int(plane->width), 4), component, componentCount);
                } else if (plane->request.filter != nova::SampleFilter::Center &&
                           plane->request.kind != nova::UtilityKind::Ao) {
                    // Karma's sample filters (docs/design/M11-P6.md 2): every
                    // pixel gets the filter's value, the background its
                    // default; an LPE plane takes the beauty's alpha.
                    float o[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    nova::resolveFilteredSample(*plane, src, film.count[src], o);
                    int n = plane->width;
                    if (plane->request.kind == nova::UtilityKind::Lpe) {
                        o[3] = film.alphaSum[src] * inv;
                        n = 4;
                    }
                    _WritePixel(i, o, n, component, componentCount);
                } else if (plane->request.kind == nova::UtilityKind::Ao) {
                    const float a = v[0] * inv;
                    const float o[4] = {a, a, a, 1.0f};
                    _WritePixel(i, o, 4, component, componentCount);
                } else if (plane->request.kind == nova::UtilityKind::Face) {
                    // As the prim ids: float images keep the clear value on
                    // the background, int buffers get -1.
                    if (!source.intAsFloat || v[0] >= 0.0f)
                        _WritePixelInt(i, int(v[0]), source.intAsFloat || component != HdFormatInt32);
                } else {
                    _WritePixel(i, v, plane->width, component, componentCount);
                }
                break;
            }
            default:
                break;
            }
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
