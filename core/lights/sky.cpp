#include "core/lights/sky.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nova {

namespace {

// The authors' published data (unchanged, BSD 3-Clause): plain arrays
// datasetsXYZ[channel] (coefficients) and datasetsXYZRad[channel]
// (radiances), kept local to this file.
#include "third_party/hosek_wilkie/ArHosekSkyModelData_CIEXYZ.h"

constexpr double kPiD = 3.14159265358979323846;

// Quintic Bezier over the cube root of the solar elevation: control point k
// of a coefficient sits at cp[k * stride].
double bezier5(const double *cp, int stride, double x) {
    const double y = 1.0 - x;
    const double x2 = x * x, y2 = y * y;
    return y2 * y2 * y * cp[0] + 5.0 * y2 * y2 * x * cp[stride] + 10.0 * y2 * y * x2 * cp[2 * stride] +
           10.0 * y2 * x2 * x * cp[3 * stride] + 5.0 * y * x2 * x2 * cp[4 * stride] + x2 * x2 * x * cp[5 * stride];
}

// Interpolates `count` values of a dataset laid out as [albedo 0, 1][turbidity
// 1..10][6 control points][count]: Bezier in the elevation, linear in the
// turbidity and the albedo (the model's scheme).
void cook(const double *dataset, int count, double turbidity, double albedo, double elevation, double *out) {
    const double t = std::clamp(turbidity, 1.0, 10.0);
    const int lower = std::min(int(t), 10);
    const double frac = t - double(lower);
    const double a = std::clamp(albedo, 0.0, 1.0);
    const double x = std::cbrt(std::clamp(elevation, 0.0, 0.5 * kPiD) / (0.5 * kPiD));
    const int block = 6 * count;
    for (int i = 0; i < count; ++i)
        out[i] = 0.0;
    for (int alb = 0; alb < 2; ++alb) {
        const double wa = alb ? a : 1.0 - a;
        for (int k = 0; k < 2; ++k) {
            const int turb = lower + k;
            const double wt = k ? frac : 1.0 - frac;
            if (turb > 10 || wa * wt == 0.0)
                continue;
            const double *base = dataset + alb * 10 * block + (turb - 1) * block;
            for (int i = 0; i < count; ++i)
                out[i] += wa * wt * bezier5(base + i, count, x);
        }
    }
}

// CIE XYZ to linear Rec.709 (sRGB primaries, D65).
constexpr double kXyzToRec709[9] = {3.2409699419045226,  -1.5373831775700939, -0.4986107602930034,
                                    -0.9692436362808796, 1.8759675015077202,  0.0415550574071756,
                                    0.0556300796969937,  -0.2039769588889765, 1.0569715142428786};

// Everything a direction's radiance needs, prepared once per sky.
struct Sky {
    HosekSkyState state;
    double sun[3];
    double blur;     // radians of altitude
    double fadeTop;  // altitude (radians) and height (sine) where the sky starts to fade
    double toRgb[9]; // model XYZ to the rendering space, with Karma's 2 pi / 683
    Spectrum lightColor, ground;

    explicit Sky(const KarmaSkyDesc &desc) {
        const double albedo[3] = {desc.groundAlbedo.x, desc.groundAlbedo.y, desc.groundAlbedo.z};
        const double altitude = std::clamp(double(desc.solarAltitude), 0.0, 90.0) * (kPiD / 180.0);
        const double azimuth = double(desc.solarAzimuth) * (kPiD / 180.0);
        state = makeHosekSkyState(desc.turbidity, albedo, altitude);
        sun[0] = std::cos(altitude) * std::sin(azimuth);
        sun[1] = std::sin(altitude);
        sun[2] = -std::cos(altitude) * std::cos(azimuth);
        blur = std::isfinite(desc.horizonBlur) ? std::max(double(desc.horizonBlur), 0.0) * (kPiD / 180.0) : 0.0;
        fadeTop = std::min(1.0 / 64.0, blur);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                double v = 0.0;
                for (int k = 0; k < 3; ++k)
                    v += double(desc.skyToRendering[r * 3 + k]) * kXyzToRec709[k * 3 + c];
                toRgb[r * 3 + c] = v * (2.0 * kPiD / 683.0);
            }
        lightColor = desc.lightColor;
        ground = desc.computeGroundColor ? desc.groundColor : desc.groundColor * desc.lightColor;
    }

    Spectrum skyAt(double x, double y, double z) const {
        double xyz[3];
        hosekSkyXYZ(state, y, x * sun[0] + y * sun[1] + z * sun[2], xyz);
        auto row = [&](int r) {
            return float(toRgb[r * 3] * xyz[0] + toRgb[r * 3 + 1] * xyz[1] + toRgb[r * 3 + 2] * xyz[2]);
        };
        return Spectrum{row(0), row(1), row(2)} * lightColor;
    }

    Spectrum radiance(double x, double y, double z) const {
        const double len = std::sqrt(x * x + y * y + z * z);
        if (!(len > 0.0))
            return ground;
        x /= len, y /= len, z /= len;
        if (y >= fadeTop)
            return skyAt(x, y, z);
        // Below the fade: the sky of the fade's top height, blended into
        // the ground with smoothstep over the altitude.
        const double altitude = std::asin(std::clamp(y, -1.0, 1.0));
        double t = 0.0;
        if (fadeTop > -blur) {
            const double s = std::clamp((altitude + blur) / (fadeTop + blur), 0.0, 1.0);
            t = s * s * (3.0 - 2.0 * s);
        }
        if (t <= 0.0)
            return ground;
        const double h = std::sqrt(x * x + fadeTop * fadeTop + z * z);
        const Spectrum sky = skyAt(x / h, fadeTop / h, z / h);
        return ground + (sky - ground) * float(t);
    }
};

} // namespace

HosekSkyState makeHosekSkyState(double turbidity, const double albedo[3], double solarElevation) {
    HosekSkyState s;
    for (int c = 0; c < 3; ++c) {
        cook(datasetsXYZ[c], 9, turbidity, albedo[c], solarElevation, s.coefficients[c]);
        cook(datasetsXYZRad[c], 1, turbidity, albedo[c], solarElevation, &s.radiance[c]);
    }
    return s;
}

void hosekSkyXYZ(const HosekSkyState &state, double cosTheta, double cosGamma, double xyz[3]) {
    cosGamma = std::clamp(cosGamma, -1.0, 1.0);
    const double gamma = std::acos(cosGamma);
    const double rayleigh = cosGamma * cosGamma;
    const double zenith = std::sqrt(std::max(cosTheta, 0.0));
    for (int c = 0; c < 3; ++c) {
        const double *k = state.coefficients[c];
        const double mie = (1.0 + rayleigh) / std::pow(1.0 + k[8] * k[8] - 2.0 * k[8] * cosGamma, 1.5);
        const double horizon = 1.0 + k[0] * std::exp(k[1] / (cosTheta + 0.01));
        const double sun = k[2] + k[3] * std::exp(k[4] * gamma) + k[5] * rayleigh + k[6] * mie + k[7] * zenith;
        xyz[c] = horizon * sun * state.radiance[c];
    }
}

bool operator==(const KarmaSkyDesc &a, const KarmaSkyDesc &b) {
    auto same = [](Spectrum u, Spectrum v) { return u.x == v.x && u.y == v.y && u.z == v.z; };
    return a.solarAltitude == b.solarAltitude && a.solarAzimuth == b.solarAzimuth && a.turbidity == b.turbidity &&
           a.horizonBlur == b.horizonBlur && same(a.groundAlbedo, b.groundAlbedo) &&
           same(a.groundColor, b.groundColor) && a.computeGroundColor == b.computeGroundColor &&
           same(a.lightColor, b.lightColor) &&
           std::memcmp(a.skyToRendering, b.skyToRendering, sizeof(a.skyToRendering)) == 0;
}

Spectrum karmaSkyRadiance(const KarmaSkyDesc &desc, Vec3f dir) {
    return Sky(desc).radiance(dir.x, dir.y, dir.z);
}

std::shared_ptr<Image> bakeKarmaSky(const KarmaSkyDesc &desc, int width) {
    width = std::max(width, 4) & ~1;
    const int height = width / 2;
    auto image = std::make_shared<Image>();
    image->width = width;
    image->height = height;
    image->pixels.resize(size_t(width) * size_t(height));
    const Sky sky(desc);
    // Texel centers in the dome mapping: phi = pi (1 - 2u), theta = pi v,
    // direction (sin theta sin phi, cos theta, sin theta cos phi).
    tbb::parallel_for(tbb::blocked_range<int>(0, height), [&](const tbb::blocked_range<int> &rows) {
        for (int j = rows.begin(); j < rows.end(); ++j) {
            const double theta = kPiD * (double(j) + 0.5) / double(height);
            const double st = std::sin(theta), ct = std::cos(theta);
            Spectrum *row = image->pixels.data() + size_t(j) * size_t(width);
            for (int i = 0; i < width; ++i) {
                const double phi = kPiD * (1.0 - 2.0 * (double(i) + 0.5) / double(width));
                row[i] = sky.radiance(st * std::sin(phi), ct, st * std::cos(phi));
            }
        }
    });
    return image;
}

} // namespace nova
