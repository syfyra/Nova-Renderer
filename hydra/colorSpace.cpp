#include "colorSpace.h"

#include "diagnostics.h"

#include <OpenColorIO/OpenColorIO.h>
#include <OpenImageIO/strutil.h>

#include <cmath>
#include <cstdlib>
#include <initializer_list>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

namespace OCIO = OCIO_NAMESPACE;

// Nova's names for the two generic encodings OIIO readers and the automatic
// rules report. Both are aliases in Houdini's config and in the ACES
// cg/studio configs.
constexpr const char *kLinearRec709 = "lin_rec709";
constexpr const char *kSrgbTexture = "srgb_tx";

// The OCIO config Houdini and husk point $OCIO at (null without one). Its
// file rules give the texture conventions Karma follows (Houdini's config:
// png, jpg, tga, tif sRGB; exr linear; "*ACEScg*" names ACEScg), and its
// scene_linear role is Nova's rendering space.
OCIO::ConstConfigRcPtr ocioConfig() {
    static const OCIO::ConstConfigRcPtr config = []() -> OCIO::ConstConfigRcPtr {
        const char *env = std::getenv("OCIO");
        if (!env || !*env)
            return nullptr;
        try {
            return OCIO::Config::CreateFromEnv();
        } catch (OCIO::Exception const &e) {
            HD_NOVA_WARN("cannot load the OCIO config '%s' (%s); textures are not color managed", env, e.what());
            return nullptr;
        }
    }();
    return config;
}

bool isOneOf(std::string const &name, std::initializer_list<const char *> names) {
    for (const char *n : names)
        if (OIIO::Strutil::iequals(name, n))
            return true;
    return false;
}

// OIIO's and MaterialX's generic color space names as Nova's names.
std::string canonicalName(std::string const &name) {
    if (isOneOf(name, {"linear", "lin_srgb", "lin_rec709", "lin_rec709_srgb", "Linear Rec.709 (sRGB)", "scene_linear"}))
        return kLinearRec709;
    if (isOneOf(name, {"sRGB", "srgb_tx", "srgb_texture", "sRGB - Texture", "sRGB Encoded Rec.709 (sRGB)"}))
        return kSrgbTexture;
    return name;
}

bool isKnown(std::string const &cs) {
    const OCIO::ConstConfigRcPtr config = ocioConfig();
    return cs == kLinearRec709 || cs == kSrgbTexture || (config && config->getColorSpace(cs.c_str()));
}

float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

// The space an explicit hint names ("" for raw); false for none, "auto" or
// an unknown name (then `unknown` is set).
bool hintColorSpace(std::string const &hint, std::string &cs, bool &unknown) {
    unknown = false;
    if (hint.empty() || OIIO::Strutil::iequals(hint, "auto"))
        return false;
    if (isOneOf(hint, {"raw", "data", "none", "Raw", "Utility - Raw"})) {
        cs.clear();
        return true;
    }
    cs = canonicalName(hint);
    unknown = !isKnown(cs);
    return !unknown;
}

// The space a real file rule of the OCIO config gives `path` (a path that
// only matches the Default rule has none).
bool fileRuleColorSpace(std::string const &path, std::string &cs) {
    if (OCIO::ConstConfigRcPtr config = ocioConfig()) {
        try {
            if (!config->filepathOnlyMatchesDefaultRule(path.c_str())) {
                cs = canonicalName(config->getColorSpaceFromFilepath(path.c_str()));
                return true;
            }
        } catch (OCIO::Exception const &) {
            // a config without file rules
        }
    }
    return false;
}

} // namespace

bool HdNovaTextureColorSpaceIsNamed(std::string const &path, std::string const &hint) {
    std::string cs;
    bool unknown;
    return hintColorSpace(hint, cs, unknown) || fileRuleColorSpace(path, cs);
}

std::string HdNovaTextureColorSpace(std::string const &path, OIIO::ImageSpec const &spec, std::string const &hint) {
    std::string cs;
    bool unknown;
    if (hintColorSpace(hint, cs, unknown))
        return cs;
    if (unknown)
        HD_NOVA_WARN("texture '%s' names the unknown color space '%s'; using the automatic rules", path.c_str(),
                     hint.c_str());
    if (spec.nchannels < 3)
        return {};
    if (fileRuleColorSpace(path, cs))
        return cs;
    // Metadata naming a known space; others (e.g. "Gamma2.2" from a PNG's
    // gAMA chunk) are skipped, as Karma does.
    if (const std::string meta = canonicalName(spec.get_string_attribute("oiio:ColorSpace")); !meta.empty())
        if (isKnown(meta))
            return meta;
    // Display formats and .tx files (Houdini's imaketx writes them
    // sRGB-encoded without metadata, also as half float from float sources,
    // and Karma decodes them) are sRGB; otherwise Karma's "sRGB for 8-bit
    // images, linear other formats/depths".
    // A file in a package ("a.usdz[tex/b.png]"): the packaged file's extension.
    std::string name = path;
    while (!name.empty() && name.back() == ']')
        name.pop_back();
    const size_t dot = name.rfind('.'), slash = name.find_last_of("/\\[");
    const std::string ext = dot != std::string::npos && (slash == std::string::npos || dot > slash)
                                ? OIIO::Strutil::lower(name.substr(dot))
                                : std::string();
    if (isOneOf(ext, {".png", ".jpg", ".jpeg", ".tga", ".tif", ".tiff", ".tx"}))
        return kSrgbTexture;
    if (spec.format == OIIO::TypeDesc::UINT8 || spec.format == OIIO::TypeDesc::INT8)
        return kSrgbTexture;
    return {};
}

HdNovaColorTransform HdNovaGetColorTransform(std::string const &cs) {
    HdNovaColorTransform t;
    if (cs.empty())
        return t;
    const OCIO::ConstConfigRcPtr config = ocioConfig();
    const bool generic = cs == kLinearRec709 || cs == kSrgbTexture;
    if (!config || (generic && !config->getColorSpace(cs.c_str()))) {
        // Without OCIO (nova_cli, usdview) scene_linear is Linear Rec.709.
        if (cs == kSrgbTexture)
            t.kind = HdNovaColorTransform::Kind::Nonlinear;
        return t;
    }
    try {
        const OCIO::ConstProcessorRcPtr processor = config->getProcessor(cs.c_str(), OCIO::ROLE_SCENE_LINEAR);
        if (processor->isNoOp())
            return t;
        const OCIO::ConstCPUProcessorRcPtr cpu = processor->getDefaultCPUProcessor();
        // The columns from the unit vectors; linear if it maps a test color
        // and its multiples like the matrix.
        for (int c = 0; c < 3; ++c) {
            float e[3] = {0.0f, 0.0f, 0.0f};
            e[c] = 1.0f;
            cpu->applyRGB(e);
            for (int r = 0; r < 3; ++r)
                t.matrix[r * 3 + c] = e[r];
        }
        bool linear = true;
        for (float scale : {0.01f, 1.0f, 8.0f}) {
            float v[3] = {0.3f * scale, 0.7f * scale, 0.11f * scale};
            float m[3];
            for (int r = 0; r < 3; ++r)
                m[r] = t.matrix[r * 3] * v[0] + t.matrix[r * 3 + 1] * v[1] + t.matrix[r * 3 + 2] * v[2];
            cpu->applyRGB(v);
            for (int r = 0; r < 3; ++r)
                linear = linear && std::fabs(v[r] - m[r]) <= 1e-4f * (std::fabs(m[r]) + 1e-3f);
        }
        t.kind = linear ? HdNovaColorTransform::Kind::Matrix : HdNovaColorTransform::Kind::Nonlinear;
    } catch (OCIO::Exception const &e) {
        HD_NOVA_WARN("cannot convert from color space '%s' (%s); using it as linear", cs.c_str(), e.what());
        t = HdNovaColorTransform{};
    }
    return t;
}

void HdNovaConvertToSceneLinear(std::string const &path, std::string const &cs, float *pixels, size_t count,
                                size_t stride) {
    if (cs.empty() || count == 0)
        return;
    const OCIO::ConstConfigRcPtr config = ocioConfig();
    const bool generic = cs == kLinearRec709 || cs == kSrgbTexture;
    if (!config || (generic && !config->getColorSpace(cs.c_str()))) {
        if (cs == kSrgbTexture) {
            for (size_t i = 0; i < count; ++i)
                for (int c = 0; c < 3; ++c)
                    pixels[i * stride + size_t(c)] = srgbToLinear(pixels[i * stride + size_t(c)]);
        } else if (cs != kLinearRec709) {
            HD_NOVA_WARN("texture '%s' is in color space '%s', which needs an OCIO config; using it as linear",
                         path.c_str(), cs.c_str());
        }
        return;
    }
    try {
        const OCIO::ConstProcessorRcPtr processor = config->getProcessor(cs.c_str(), OCIO::ROLE_SCENE_LINEAR);
        if (processor->isNoOp())
            return;
        const ptrdiff_t chan = sizeof(float), x = ptrdiff_t(stride * sizeof(float));
        OCIO::PackedImageDesc image(pixels, long(count), 1, OCIO::CHANNEL_ORDERING_RGB, OCIO::BIT_DEPTH_F32, chan, x,
                                    x * ptrdiff_t(count));
        processor->getDefaultCPUProcessor()->apply(image);
    } catch (OCIO::Exception const &e) {
        HD_NOVA_WARN("cannot convert texture '%s' from color space '%s' (%s); using it as linear", path.c_str(),
                     cs.c_str(), e.what());
    }
}

std::string HdNovaSceneLinearTransformId(std::string const &cs) {
    if (cs.empty())
        return "none";
    // The same branches as HdNovaConvertToSceneLinear.
    const OCIO::ConstConfigRcPtr config = ocioConfig();
    const bool generic = cs == kLinearRec709 || cs == kSrgbTexture;
    if (!config || (generic && !config->getColorSpace(cs.c_str())))
        return cs == kSrgbTexture ? "builtin-srgb" : "builtin-linear";
    try {
        // Hashes the processor's ops: differs whenever the pixels would.
        return std::string("ocio-") + config->getProcessor(cs.c_str(), OCIO::ROLE_SCENE_LINEAR)->getCacheID();
    } catch (OCIO::Exception const &) {
        return "ocio-error"; // the conversion leaves the pixels as they are
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
