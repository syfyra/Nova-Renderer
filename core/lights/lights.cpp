#include "core/lights/lights.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nova {

namespace {

// The world direction of the light's local -Z axis as an emission normal:
// cross(X, Y) of the transformed axes, negated, and flipped by mirroring
// transforms. Equal to the inverse-transpose for invertible transforms, and
// still the plane's normal when the Z scale is 0 (no inverse exists).
Vec3f emissionNormal(const Mat4 &xf) {
    const Vec3f x = xf.transformDir({1.0f, 0.0f, 0.0f}), y = xf.transformDir({0.0f, 1.0f, 0.0f});
    const Vec3f z = xf.transformDir({0.0f, 0.0f, 1.0f});
    const Vec3f n = cross(x, y);
    const float det = dot(n, z);
    return normalize(det < 0.0f ? n : -n);
}

void setupShaping(const LightDesc &desc, LightData &d) {
    d.shaped = desc.shaped;
    const Vec3f axis = desc.transform.transformDir({0.0f, 0.0f, -1.0f});
    d.axis = dot(axis, axis) > 0.0f ? normalize(axis) : emissionNormal(desc.transform);
    // Karma: a cone angle of exactly 90 degrees (the ShapingAPI fallback,
    // delivered for every light with the API applied) disables the cone, and
    // 180 degrees or more cut nothing.
    const bool cone = desc.shaped && std::fabs(desc.coneAngleDeg - 90.0f) > 1e-4f && desc.coneAngleDeg < 180.0f;
    if (cone) {
        const float angle = std::max(desc.coneAngleDeg, 0.0f) * (kPi / 180.0f);
        const float softness = std::clamp(desc.coneSoftness, 0.0f, 1.0f);
        d.cosConeCut = std::cos(angle);
        d.cosConeStart = std::cos(angle * (1.0f - softness));
    }
    d.focus = nova::fmax(desc.focus, 0.0f); // negative values are ignored
    d.focusTint = desc.focusTint;
}

bool validImage(const std::shared_ptr<const Image> &image) {
    return image && image->width > 0 && image->height > 0 &&
           image->pixels.size() == size_t(image->width) * size_t(image->height);
}

void attachTexture(const Image &image, LightData &d) {
    d.texels = image.pixels.data();
    d.texWidth = uint32_t(image.width);
    d.texHeight = uint32_t(image.height);
}

} // namespace

namespace {

LightData buildLightData(const LightDesc &desc) {
    LightData d;
    d.type = desc.type;
    d.diffuse = desc.diffuse;
    d.specular = desc.specular;
    d.cameraVisible = desc.cameraVisible;
    d.illumBackground = desc.illumBackground;
    d.castsShadows = desc.castsShadows;
    const Spectrum base = desc.color * (desc.intensity * std::exp2(desc.exposure));
    const Mat4 &xf = desc.transform;
    d.position = xf.transformAffine({0.0f, 0.0f, 0.0f});

    switch (desc.type) {
    case LightType::Distant: {
        // UsdLux distant lights emit along their local -Z axis. UsdLux clips
        // the angle to 360 degrees; Karma to 180 (wider lights render like a
        // hemisphere), which Nova follows.
        d.direction = normalize(xf.transformDir({0.0f, 0.0f, 1.0f}));
        const float angle = std::clamp(desc.angleDeg, 0.0f, 180.0f);
        // Exactly 1 for a hemisphere (1 - cos(90 deg) rounds to 0.99999994).
        const float omc = angle >= 180.0f ? 1.0f : nova::fmin(oneMinusCos(0.5f * angle * (kPi / 180.0f)), 1.0f);
        d.oneMinusCosThetaMax = omc;
        if (omc <= 0.0f) {
            d.delta = true;
            d.emission = base;
        } else {
            // UsdLux size factor pi * sin^2 of the half-angle: a facing
            // surface then receives exactly intensity * 2^exposure.
            d.emission = desc.normalize ? base / (kPi * omc * (2.0f - omc)) : base;
        }
        return d;
    }
    case LightType::Dome: {
        d.emission = base;
        // Only the rotation orients the dome: an orthonormal frame, kept
        // right- or left-handed like the transform.
        const Vec3f x = normalize(xf.transformDir({1.0f, 0.0f, 0.0f}));
        Vec3f y = xf.transformDir({0.0f, 1.0f, 0.0f});
        y = normalize(y - x * dot(y, x));
        Vec3f z = cross(x, y);
        if (dot(z, xf.transformDir({0.0f, 0.0f, 1.0f})) < 0.0f)
            z = -z;
        d.domeX = x;
        d.domeY = y;
        d.domeZ = z;
        if (validImage(desc.texture))
            attachTexture(*desc.texture, d);
        return d;
    }
    case LightType::Sphere: {
        if (desc.treatAsPoint) {
            // Radiant intensity; with normalize, the limit of a normalized
            // sphere: L pi r^2 / (4 pi r^2).
            d.delta = true;
            d.emission = desc.normalize ? base * 0.25f : base;
            break;
        }
        // Uniform scale assumed; the mean axis scale stands in otherwise.
        const float scale = (length(xf.transformDir({1.0f, 0.0f, 0.0f})) + length(xf.transformDir({0.0f, 1.0f, 0.0f})) +
                             length(xf.transformDir({0.0f, 0.0f, 1.0f}))) / 3.0f;
        d.radius = std::fabs(desc.radius) * scale;
        d.area = 4.0f * kPi * d.radius * d.radius;
        break;
    }
    case LightType::Rect:
        d.axisU = xf.transformDir({0.5f * desc.width, 0.0f, 0.0f});
        d.axisV = xf.transformDir({0.0f, 0.5f * desc.height, 0.0f});
        d.normal = emissionNormal(xf);
        d.area = 4.0f * length(cross(d.axisU, d.axisV));
        d.rect = RectFrame::make(d.position - d.axisU - d.axisV, d.axisU * 2.0f, d.axisV * 2.0f);
        d.twoSided = desc.twoSided;
        d.rectFlip = desc.rectFlip;
        if (validImage(desc.texture))
            attachTexture(*desc.texture, d); // texMeanLuminance comes from the scene's per-image cache
        break;
    case LightType::Disk:
        d.axisU = xf.transformDir({desc.radius, 0.0f, 0.0f});
        d.axisV = xf.transformDir({0.0f, desc.radius, 0.0f});
        d.normal = emissionNormal(xf);
        d.area = kPi * length(cross(d.axisU, d.axisV));
        d.twoSided = desc.twoSided;
        break;
    case LightType::Cylinder: {
        d.axisU = xf.transformDir({0.5f * desc.length, 0.0f, 0.0f});
        // A circular cross-section of the mean radial scale, perpendicular to the axis.
        const float radial = 0.5f * (length(xf.transformDir({0.0f, desc.radius, 0.0f})) +
                                     length(xf.transformDir({0.0f, 0.0f, desc.radius})));
        const Frame frame = Frame::fromNormal(normalize(d.axisU));
        d.radius = radial;
        d.axisV = frame.t * radial;
        d.axisW = frame.b * radial;
        d.area = kTwoPi * radial * 2.0f * length(d.axisU);
        break;
    }
    }

    setupShaping(desc, d);
    if (d.delta)
        return d;
    // Degenerate shapes emit nothing.
    if (!(d.area > 0.0f) || !std::isfinite(d.area)) {
        d.emission = {0.0f, 0.0f, 0.0f};
        d.area = 0.0f;
        return d;
    }
    d.emission = desc.normalize ? base / d.area : base;
    return d;
}

} // namespace

LightData makeLightData(const LightDesc &desc) {
    LightData d = buildLightData(desc);
    d.selection = std::isfinite(desc.samplingQuality) && desc.samplingQuality > 0.0f ? desc.samplingQuality : 0.0f;
    d.equiangular = desc.equiangular && (isAreaLight(d.type) || d.delta) && d.type != LightType::Distant;
    d.outsideTree = desc.outsideTree;
    // Invalid input (NaN or infinite intensity, exposure overflow, a
    // vanishing normalized area) emits nothing instead of poisoning every
    // path that samples the light; lightPower() gives such lights power 0.
    if (!isFinite(d.emission))
        d.emission = {0.0f, 0.0f, 0.0f};
    return d;
}

LightFilterData makeLightFilterData(const LightFilterDesc &desc) {
    LightFilterData f;
    f.kind = desc.kind;
    f.shape = desc.shape;
    // World -> space with row vectors (q = P * M), as light_filter.h applies it.
    const Mat4 &m = desc.spaceFromWorld;
    const auto row = [&](int i) { return Vec3f{float(m.m[i][0]), float(m.m[i][1]), float(m.m[i][2])}; };
    f.x = row(0);
    f.y = row(1);
    f.z = row(2);
    f.t = row(3);
    const auto positive = [](float v) { return std::isfinite(v) && v > 0.0f ? v : 0.0f; };
    float size = 0.0f;
    switch (desc.shape) {
    case BlockerShape::Sphere:
        size = positive(desc.radius);
        f.half = {size, 0.0f, 0.0f};
        break;
    case BlockerShape::Capsule:
        size = positive(desc.radius);
        f.half = {size, 0.5f * positive(desc.height), 0.0f};
        break;
    default: // box: the corner rounding and the falloff scale with its smallest half extent (Karma, measured)
        f.half = {0.5f * positive(desc.width), 0.5f * positive(desc.height), 0.5f * positive(desc.depth)};
        size = std::min({f.half.x, f.half.y, f.half.z});
        f.rounded = std::clamp(positive(desc.roundedCorner), 0.0f, 1.0f) * size;
        break;
    }
    f.invSize = size > 0.0f ? 1.0f / size : 0.0f; // an empty shape blocks nothing
    f.falloff = std::clamp(positive(desc.falloff), 0.0f, 1.0f);
    f.invert = desc.invert ? 1u : 0u;
    f.blocked = desc.tint * (desc.intensity * std::exp2(desc.exposure));
    if (!isFinite(f.blocked))
        f.blocked = {0.0f, 0.0f, 0.0f};
    return f;
}

float textureMeanLuminance(const Image &texture) {
    if (texture.pixels.empty())
        return 0.0f;
    double sum = 0.0;
    for (const Spectrum &c : texture.pixels) {
        const float l = luminance(c);
        sum += std::isfinite(l) ? nova::fmax(l, 0.0f) : 0.0f;
    }
    return float(sum / double(texture.pixels.size()));
}

bool buildDomeDistribution(const Image &texture, Distribution2D &out) {
    const int w = texture.width, h = texture.height;
    if (w <= 0 || h <= 0 || texture.pixels.size() != size_t(w) * size_t(h)) {
        out.build(nullptr, 0, 0);
        return false;
    }
    auto lum = [&](int x, int y) {
        const float l = luminance(texture.pixels[size_t(y) * size_t(w) + size_t(x)]);
        return std::isfinite(l) ? nova::fmax(l, 0.0f) : 0.0f;
    };

    // Solid-angle weighted mean luminance: sum(lum sin) / sum(sin).
    double weighted = 0.0, weights = 0.0;
    for (int y = 0; y < h; ++y) {
        const double s = std::sin(kPi * (double(y) + 0.5) / double(h));
        for (int x = 0; x < w; ++x) {
            weighted += double(lum(x, y)) * s;
            weights += s;
        }
    }
    const float mean = weights > 0.0 ? float(weighted / weights) : 0.0f;

    // Sampling grid: at most 2048 cells wide (about f x f texels per cell).
    // Each cell holds the brightest texel that the bilinear lookup reads
    // anywhere inside the cell, i.e. its texels plus a one-texel border, so
    // light sampling also covers the soft fringe around bright features
    // without spreading them further.
    const int f = (w + 2047) / 2048;
    const int gw = (w + f - 1) / f, gh = (h + f - 1) / f;
    // Texels [first, last] read for coordinates in [c / cells, (c + 1) / cells].
    auto footprint = [](int c, int cells, int texels, int &first, int &last) {
        first = int(std::floor(double(c) * texels / cells - 0.5));
        last = int(std::floor(double(c + 1) * texels / cells - 0.5)) + 1;
    };
    std::vector<int> cellX0(static_cast<size_t>(gw)), cellX1(static_cast<size_t>(gw));
    for (int gx = 0; gx < gw; ++gx)
        footprint(gx, gw, w, cellX0[size_t(gx)], cellX1[size_t(gx)]);
    std::vector<float> rowMax(size_t(h) * size_t(gw), 0.0f); // per texel row, over each cell's u footprint
    for (int y = 0; y < h; ++y) {
        for (int gx = 0; gx < gw; ++gx) {
            float m = 0.0f;
            for (int x = cellX0[size_t(gx)]; x <= cellX1[size_t(gx)]; ++x)
                m = nova::fmax(m, lum(((x % w) + w) % w, y)); // u wraps around
            rowMax[size_t(y) * size_t(gw) + size_t(gx)] = m;
        }
    }
    std::vector<float> func(size_t(gw) * size_t(gh));
    bool any = false;
    for (int gy = 0; gy < gh; ++gy) {
        int y0, y1;
        footprint(gy, gh, h, y0, y1);
        y0 = std::max(y0, 0); // v clamps
        y1 = std::min(y1, h - 1);
        const float sinTheta = std::sin(kPi * (float(gy) + 0.5f) / float(gh));
        for (int gx = 0; gx < gw; ++gx) {
            float m = 0.0f;
            for (int y = y0; y <= y1; ++y)
                m = nova::fmax(m, rowMax[size_t(y) * size_t(gw) + size_t(gx)]);
            const float v = nova::fmax(m - mean, 0.0f) * sinTheta;
            func[size_t(gy) * size_t(gw) + size_t(gx)] = v;
            any |= v > 0.0f;
        }
    }
    if (!any) {
        out.build(nullptr, 0, 0);
        return false;
    }
    out.build(func.data(), uint32_t(gw), uint32_t(gh));
    return true;
}

float lightPower(const LightData &light, float sceneRadius) {
    float power = 0.0f;
    switch (light.type) {
    case LightType::Distant: {
        // Irradiance on a surface facing the light, times the scene's cross-section.
        float irradiance = luminance(light.emission);
        if (!light.delta) {
            const float omc = light.oneMinusCosThetaMax;
            irradiance *= kPi * omc * (2.0f - omc);
        }
        power = irradiance * kPi * sceneRadius * sceneRadius;
        break;
    }
    case LightType::Dome:
        if (light.portalCount > 0) {
            // Through its portals (docs/design/M11-P9b.md): the flux of their
            // area under the dome's mean radiance (texMeanLuminance).
            power = luminance(light.emission) * light.texMeanLuminance * kPi * light.portalArea;
            break;
        }
        // Only the part the distribution covers is light-sampled: its
        // radiance integral over the sphere is 2 pi^2 times the (u, v) integral.
        power = luminance(light.emission) * 2.0f * kPi * kPi * light.distribution.integral * kPi * sceneRadius *
                sceneRadius;
        break;
    case LightType::Volume: // from the volume's emission bounds (Scene)
        power = light.power;
        break;
    default:
        if (light.delta) {
            // Point light; an active cone limits the emitted solid angle.
            const float cosMid = light.cosConeCut >= -1.0f ? 0.5f * (light.cosConeCut + light.cosConeStart) : -1.0f;
            power = luminance(light.emission) * kTwoPi * (1.0f - cosMid);
        } else {
            power = luminance(light.emission) * kPi * light.area * (light.twoSided ? 2.0f : 1.0f);
            if (light.type == LightType::Rect && light.texels)
                power *= light.texMeanLuminance;
        }
        break;
    }
    // A negative intensity subtracts light: select it by the magnitude, the
    // emission keeps its sign.
    // Selected by the larger multiplier: a light with diffuse 0 still lights
    // specular reflections.
    const float multiplier = nova::fmax(std::fabs(light.diffuse), std::fabs(light.specular));
    return std::isfinite(power) ? std::fabs(power) * multiplier : 0.0f;
}

} // namespace nova
