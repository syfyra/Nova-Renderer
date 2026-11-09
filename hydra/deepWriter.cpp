#include "deepWriter.h"

#include <OpenImageIO/deepdata.h>
#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <filesystem>
#include <numeric>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

void setMatrix(OIIO::ImageSpec &spec, char const *name, nova::Mat4 const &m) {
    float v[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            v[i * 4 + j] = float(m.m[i][j]);
    spec.attribute(name, OIIO::TypeMatrix44, v);
}

} // namespace

bool HdNovaWriteDeepImage(nova::Film const &film, HdNovaDeepOutput const &out, std::string &error) {
    if (film.deep.empty()) {
        error = "no deep samples were rendered";
        return false;
    }
    nova::DeepPlane const &plane = film.deep[0];
    const int w = film.width, h = film.height;
    const bool colourOpacity = out.opacitySize >= 3;
    // Channels: R G B A [RA GA BA] Z extras... ZBack.
    std::vector<std::string> names = {"R", "G", "B", "A"};
    std::vector<OIIO::TypeDesc> types = {OIIO::TypeFloat, OIIO::TypeFloat, OIIO::TypeFloat, OIIO::TypeHalf};
    if (colourOpacity)
        for (char const *n : {"RA", "GA", "BA"}) {
            names.push_back(n);
            types.push_back(OIIO::TypeHalf);
        }
    const int zChannel = int(names.size());
    names.push_back("Z");
    types.push_back(OIIO::TypeFloat);
    const int firstExtra = int(names.size());
    const size_t extraCount = plane.stride - 5;
    for (size_t e = 0; e < extraCount; ++e) {
        names.push_back(e < out.extraChannels.size() ? out.extraChannels[e] : "extra" + std::to_string(e));
        types.push_back(OIIO::TypeFloat);
    }
    const int zBackChannel = int(names.size());
    names.push_back("ZBack");
    types.push_back(OIIO::TypeFloat);
    const int nch = int(names.size());

    OIIO::ImageSpec spec(w, h, nch, OIIO::TypeFloat);
    spec.channelnames = names;
    spec.channelformats = types;
    spec.alpha_channel = 3;
    spec.z_channel = zChannel;
    spec.deep = true;
    spec.x = out.originX;
    spec.y = out.originY;
    spec.full_x = 0;
    spec.full_y = 0;
    spec.full_width = out.fullWidth > 0 ? out.fullWidth : w;
    spec.full_height = out.fullHeight > 0 ? out.fullHeight : h;
    spec.attribute("compression", "zips");
    spec.attribute("Software", "Nova");
    if (out.hasCamera) {
        setMatrix(spec, "worldToCamera", out.worldToCamera);
        setMatrix(spec, "worldToNDC", out.worldToNdc);
        spec.attribute("clipNear", out.clipNear);
        spec.attribute("clipFar", out.clipFar);
    }

    OIIO::DeepData deep;
    deep.init(int64_t(w) * h, nch, types, names);
    // Per pixel its records front to back. Each holds the sums over its
    // camera samples; with T the transmittance in front of a record, its
    // alpha is coverage / T and its colour and extras sum / (N T), so
    // compositing "over" gives the pixel's mean.
    std::vector<size_t> order;
    // A pixel's records front to back, up to the first full coverage.
    const auto sortPixel = [&](size_t src) {
        const std::vector<float> &list = plane.pixels[src];
        const uint32_t count = film.count[src];
        const size_t records = count ? list.size() / plane.stride : 0;
        order.resize(records);
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return list[a * plane.stride] < list[b * plane.stride]; });
        float T = 1.0f;
        size_t n = 0;
        for (; n < records && T > 1e-7f; ++n)
            T -= list[order[n] * plane.stride + 4] / float(count);
        order.resize(n);
    };
    std::vector<unsigned int> samples(size_t(w) * size_t(h), 0u);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            sortPixel(size_t(y) * size_t(w) + size_t(x)); // film rows are bottom-up, image rows top-down
            samples[size_t(h - 1 - y) * size_t(w) + size_t(x)] = unsigned(order.size());
        }
    deep.set_all_samples(samples);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t src = size_t(y) * size_t(w) + size_t(x);
            const int64_t dst = int64_t(h - 1 - y) * w + x;
            sortPixel(src);
            if (order.empty())
                continue;
            const std::vector<float> &list = plane.pixels[src];
            const float inv = 1.0f / float(film.count[src]);
            float T = 1.0f;
            int n = 0;
            for (size_t r : order) {
                const float *rec = &list[r * plane.stride];
                const float coverage = rec[4] * inv;
                const float s = inv / T;
                const float alpha = std::min(1.0f, coverage / T);
                deep.set_deep_value(dst, 0, n, rec[1] * s);
                deep.set_deep_value(dst, 1, n, rec[2] * s);
                deep.set_deep_value(dst, 2, n, rec[3] * s);
                deep.set_deep_value(dst, 3, n, alpha);
                if (colourOpacity)
                    for (int c = 4; c < 7; ++c)
                        deep.set_deep_value(dst, c, n, alpha);
                deep.set_deep_value(dst, zChannel, n, rec[0]);
                for (size_t e = 0; e < extraCount; ++e)
                    deep.set_deep_value(dst, firstExtra + int(e), n, rec[5 + e] * s);
                deep.set_deep_value(dst, zBackChannel, n, rec[0]);
                T = std::max(T - coverage, 1e-7f);
                ++n;
            }
        }

    std::error_code ec;
    const std::filesystem::path path = std::filesystem::u8path(out.path);
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    auto output = OIIO::ImageOutput::create(out.path);
    if (!output || !output->supports("deepdata")) {
        error = "no deep image writer for " + out.path;
        return false;
    }
    if (!output->open(out.path, spec) || !output->write_deep_image(deep)) {
        error = output->geterror();
        return false;
    }
    output->close();
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
