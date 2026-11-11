#include "textureCache.h"

#include "colorSpace.h"
#include "diagnostics.h"
#include "pathUtil.h"
#include "textureConvert.h"

#include <OpenImageIO/imageio.h>

#include <pxr/usd/ar/packageUtils.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <system_error>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

static_assert(sizeof(nova::Spectrum) == 3 * sizeof(float), "textures are read straight into Spectrum pixels");

int64_t lastWriteTime(std::string const &path) {
    // A file in a package changes with the package.
    const std::string file = ArIsPackageRelativePath(path) ? ArSplitPackageRelativePathOuter(path).first : path;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(HdNovaPath(file), ec);
    return ec ? 0 : int64_t(t.time_since_epoch().count());
}

std::shared_ptr<nova::Image> readImage(std::string const &path) {
    // A file in a package (.usdz) is read from a copy extracted into the
    // default texture cache directory, a .rat file (Houdini's own format, as
    // its HDRIs in $HFS/houdini/pic/hdri, which OIIO cannot read) from the EXR
    // imaketx converts it to there; `path` still names it and picks its color
    // space.
    std::string error;
    const std::string local =
        HdNovaReadableTexture(path, HdNovaTextureOptions{}, HdNovaTextureColorSpaceIsNamed(path), error);
    if (local.empty()) {
        HD_NOVA_WARN("cannot open texture '%s': %s", path.c_str(), error.c_str());
        return nullptr;
    }
    auto in = OIIO::ImageInput::open(local);
    if (!in) {
        HD_NOVA_WARN("cannot open texture '%s': %s", path.c_str(), OIIO::geterror().c_str());
        return nullptr;
    }
    const OIIO::ImageSpec &spec = in->spec();
    const int w = spec.width, h = spec.height, channels = spec.nchannels;
    if (w <= 0 || h <= 0 || channels <= 0) {
        HD_NOVA_WARN("texture '%s' is empty", path.c_str());
        return nullptr;
    }

    // Straight into the pixels: RGB, or the first channel copied to all three
    // (1- and 2-channel images are linear, see HdNovaTextureColorSpace).
    auto image = std::make_shared<nova::Image>();
    image->width = w;
    image->height = h;
    image->pixels.resize(size_t(w) * size_t(h));
    float *dst = &image->pixels[0].x;
    const bool ok = channels >= 3
                        ? in->read_image(0, 0, 0, 3, OIIO::TypeDesc::FLOAT, dst)
                        : in->read_image(0, 0, 0, 1, OIIO::TypeDesc::FLOAT, dst, sizeof(nova::Spectrum));
    if (!ok) {
        HD_NOVA_WARN("cannot read texture '%s': %s", path.c_str(), in->geterror().c_str());
        return nullptr;
    }
    if (channels < 3)
        for (nova::Spectrum &c : image->pixels)
            c.y = c.z = c.x;

    HdNovaConvertToSceneLinear(path, HdNovaTextureColorSpace(path, spec), &image->pixels[0].x, image->pixels.size(), 3);

    // NaN and infinite texels (e.g. an unclipped sun above the half-float
    // range) would make every lookup and light sample near them invalid.
    size_t invalid = 0;
    for (nova::Spectrum &c : image->pixels) {
        for (float *v : {&c.x, &c.y, &c.z}) {
            if (!std::isfinite(*v)) {
                *v = 0.0f;
                ++invalid;
            }
        }
    }
    if (invalid > 0)
        HD_NOVA_WARN("texture '%s' has %zu NaN or infinite values, set to 0", path.c_str(), invalid);
    return image;
}

} // namespace

std::shared_ptr<const nova::Image> HdNovaTextureCache::Load(std::string const &path) {
    std::lock_guard<std::mutex> lock(_mutex);
    const int64_t writeTime = lastWriteTime(path);
    Entry &entry = _entries[path];
    if (entry.writeTime == writeTime) {
        if (auto image = entry.image.lock())
            return image;
        if (entry.failed)
            return nullptr; // already reported; retried once the file changes
    }

    std::shared_ptr<const nova::Image> image = readImage(path);
    entry.image = image;
    entry.writeTime = writeTime;
    entry.failed = !image;
    // Forget files nobody uses any more (failures are kept to stay quiet).
    for (auto it = _entries.begin(); it != _entries.end();)
        it = it->second.image.expired() && !it->second.failed && it->first != path ? _entries.erase(it)
                                                                                   : std::next(it);
    return image;
}

PXR_NAMESPACE_CLOSE_SCOPE
