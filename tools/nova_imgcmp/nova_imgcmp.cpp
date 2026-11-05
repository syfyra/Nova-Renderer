// nova_imgcmp: compares a noisy test render with a noisy reference render
// (Karma CPU) statistically, and reports HDR-FLIP and relMSE.
//
// Compare (exit 0 = pass, 1 = fail, 2 = usage / IO error):
//   nova_imgcmp --ref R.exr --ref-sigma RS.exr --test T1.exr --test2 T2.exr
//               [--block 8] [--alpha-fwer 1e-3] [--k-global 4]
//               [--block-rel-tol 0.02] [--global-rel-tol 0.003] [--abs-tol 0.002]
//               [--max-flip F] [--rgb-only] [--flip-map flip.png] [--json out.json]
//               [--region name,cx,cy,radius,relTol[,blockRelTol] ...]
//               [--pixel-tol abs,rel[,maxBadFraction]]
// Build a reference from two independent halves (different seeds):
//   nova_imgcmp --make-ref A.exr B.exr --out R.exr --out-sigma RS.exr [--float]
// (half float, or 32-bit float with --float).
//
// --pixel-tol replaces the statistical tests for deterministic images (no
// noise: unlit pattern probes at the pixel centers): every pixel and RGB
// channel of T = (T1 + T2) / 2 must satisfy |T - R| <= abs + rel * |R|, except
// for at most maxBadFraction of the pixels (default 0).
//
// Statistics. T1 and T2 are independent renders (different seeds) of the
// test renderer, A and B of the reference renderer. Per pixel and channel
//   T = (T1 + T2) / 2, var(T) ~ (T1 - T2)^2 / 4,
//   R = (A + B) / 2,   var(R) ~ sigma^2 with sigma = |A - B| / 2.
// Both renderers draw every pixel's samples independently (filter importance
// sampling, no splatting), so block means have var = sum(var) / n^2.
// Block test:  |T_b - R_b| <= k_b * s_b + blockRelTol * |R_b| + absTol * mu
// Global test: |T - R|     <= kGlobal * s + globalRelTol * |R|   (per channel)
// k_b is the Sidak-corrected two-sided normal quantile for all block x
// channel tests at family-wise error rate alphaFwer, widened to a Student t
// quantile for the Welch-Satterthwaite degrees of freedom of each block. mu
// is the reference's mean over pixels and colour channels.
//
// Regions (repeatable) mark circles (pixels, row 0 = top) where the test
// renderer is known to differ from the reference by up to relTol, e.g. a
// documented BSDF difference to Karma. Blocks whose center lies inside use
// blockRelTol (default relTol: differences of angle-dependent models peak at
// grazing angles); the global test covers the pixels outside all regions,
// and each region's mean gets its own test with relTol.

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "flip/FLIP.h" // last: it defines the macros Max and Min
#undef Max
#undef Min

namespace {

struct Image {
    int width = 0, height = 0, channels = 0; // channels: 3 (RGB) or 4 (RGBA)
    std::vector<float> px;                   // interleaved, row 0 = top (file order)
    float at(int x, int y, int c) const { return px[(size_t(y) * width + x) * channels + c]; }
};

// R, G, B (and A if present) by channel name; else the first channels.
bool readImage(const std::string &path, Image &img) {
    auto in = OIIO::ImageInput::open(path);
    if (!in) {
        std::fprintf(stderr, "nova_imgcmp: cannot open %s: %s\n", path.c_str(), OIIO::geterror().c_str());
        return false;
    }
    const OIIO::ImageSpec &spec = in->spec();
    const int nc = spec.nchannels;
    std::vector<float> all(size_t(spec.width) * spec.height * nc);
    if (!in->read_image(0, 0, 0, nc, OIIO::TypeDesc::FLOAT, all.data())) {
        std::fprintf(stderr, "nova_imgcmp: cannot read %s: %s\n", path.c_str(), in->geterror().c_str());
        return false;
    }
    int ch[4] = {0, std::min(1, nc - 1), std::min(2, nc - 1), -1};
    for (int i = 0; i < nc; ++i) {
        const std::string &n = spec.channelnames[size_t(i)];
        if (n == "R" || n == "r") ch[0] = i;
        if (n == "G" || n == "g") ch[1] = i;
        if (n == "B" || n == "b") ch[2] = i;
        if (n == "A" || n == "a") ch[3] = i;
    }
    img.width = spec.width;
    img.height = spec.height;
    img.channels = ch[3] >= 0 ? 4 : 3;
    img.px.resize(size_t(img.width) * img.height * img.channels);
    for (size_t p = 0; p < size_t(img.width) * img.height; ++p)
        for (int c = 0; c < img.channels; ++c)
            img.px[p * img.channels + c] = all[p * nc + size_t(ch[c])];
    return true;
}

bool writeImage(const std::string &path, int w, int h, int nc, const float *data, OIIO::TypeDesc type,
                const std::vector<std::pair<std::string, std::string>> &attribs = {}) {
    auto out = OIIO::ImageOutput::create(path);
    if (!out)
        return false;
    OIIO::ImageSpec spec(w, h, nc, type);
    if (nc == 4) {
        spec.channelnames = {"R", "G", "B", "A"};
        spec.alpha_channel = 3;
    }
    if (type == OIIO::TypeDesc::HALF || type == OIIO::TypeDesc::FLOAT)
        spec.attribute("compression", "zip");
    for (const auto &[k, v] : attribs)
        spec.attribute(k, v);
    return out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, data) && out->close();
}

// Inverse of the standard normal CDF (Acklam's rational approximation,
// relative error below 1.2e-9).
double normalQuantile(double p) {
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                               1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                               6.680131188771972e+01,  -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                               -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                               3.754408661907416e+00};
    const double lo = 0.02425, hi = 1.0 - lo;
    if (p < lo) {
        const double q = std::sqrt(-2.0 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > hi) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    const double q = p - 0.5, r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

// Student t quantile from the normal quantile z (Cornish-Fisher expansion,
// Abramowitz & Stegun 26.7.5); good to a few percent for nu >= 5.
double tQuantile(double z, double nu) {
    if (!(nu < 1e6))
        return z;
    nu = std::max(nu, 3.0);
    const double z2 = z * z, z3 = z2 * z, z5 = z3 * z2, z7 = z5 * z2;
    const double g1 = (z3 + z) / 4.0;
    const double g2 = (5.0 * z5 + 16.0 * z3 + 3.0 * z) / 96.0;
    const double g3 = (3.0 * z7 + 19.0 * z5 + 17.0 * z3 - 15.0 * z) / 384.0;
    return z + g1 / nu + g2 / (nu * nu) + g3 / (nu * nu * nu);
}

struct Region {
    std::string name;
    double cx = 0, cy = 0, radius = 0, relTol = 0, blockRelTol = 0;
    bool contains(double x, double y) const { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius; }
};

struct Args {
    std::vector<Region> regions;
    std::string ref, refSigma, test, test2, json, flipMap;
    std::vector<float> refConst; // analytic reference: constant RGB(A), zero variance
    std::string makeA, makeB, out, outSigma;
    std::vector<std::pair<std::string, std::string>> attribs;
    int block = 8;
    double alphaFwer = 1e-3, kGlobal = 4.0, blockRelTol = 0.02, globalRelTol = 0.003, absTol = 0.002;
    double maxFlip = -1.0;
    bool rgbOnly = false;
    bool floatRef = false;
    double pixelAbs = -1.0, pixelRel = 0.0, pixelBadFraction = 0.0; // --pixel-tol (pixelAbs < 0: off)
};

bool parseArgs(int argc, char **argv, Args &a) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        const char *v = nullptr;
        if (s == "--ref" && (v = next())) a.ref = v;
        else if (s == "--ref-sigma" && (v = next())) a.refSigma = v;
        else if (s == "--ref-const" && (v = next())) {
            std::stringstream ss(v);
            std::string item;
            while (std::getline(ss, item, ','))
                a.refConst.push_back(float(std::atof(item.c_str())));
        }
        else if (s == "--test" && (v = next())) a.test = v;
        else if (s == "--test2" && (v = next())) a.test2 = v;
        else if (s == "--json" && (v = next())) a.json = v;
        else if (s == "--flip-map" && (v = next())) a.flipMap = v;
        else if (s == "--make-ref" && (v = next())) { a.makeA = v; if ((v = next())) a.makeB = v; }
        else if (s == "--out" && (v = next())) a.out = v;
        else if (s == "--out-sigma" && (v = next())) a.outSigma = v;
        else if (s == "--attrib" && (v = next())) {
            const std::string kv = v;
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) a.attribs.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        else if (s == "--block" && (v = next())) a.block = std::max(1, std::atoi(v));
        else if (s == "--alpha-fwer" && (v = next())) a.alphaFwer = std::atof(v);
        else if (s == "--k-global" && (v = next())) a.kGlobal = std::atof(v);
        else if (s == "--block-rel-tol" && (v = next())) a.blockRelTol = std::atof(v);
        else if (s == "--global-rel-tol" && (v = next())) a.globalRelTol = std::atof(v);
        else if (s == "--abs-tol" && (v = next())) a.absTol = std::atof(v);
        else if (s == "--max-flip" && (v = next())) a.maxFlip = std::atof(v);
        else if (s == "--rgb-only") a.rgbOnly = true;
        else if (s == "--float") a.floatRef = true;
        else if (s == "--pixel-tol" && (v = next())) {
            std::stringstream ss(v);
            std::string item;
            std::vector<double> f;
            while (std::getline(ss, item, ','))
                f.push_back(std::atof(item.c_str()));
            if (f.size() < 2 || f.size() > 3 || f[0] < 0.0) {
                std::fprintf(stderr, "nova_imgcmp: --pixel-tol expects abs,rel[,maxBadFraction]\n");
                return false;
            }
            a.pixelAbs = f[0];
            a.pixelRel = f[1];
            a.pixelBadFraction = f.size() == 3 ? f[2] : 0.0;
        }
        else if (s == "--region" && (v = next())) {
            std::stringstream ss(v);
            std::string item;
            std::vector<std::string> f;
            while (std::getline(ss, item, ','))
                f.push_back(item);
            if (f.size() != 5 && f.size() != 6) {
                std::fprintf(stderr, "nova_imgcmp: --region expects name,cx,cy,radius,relTol[,blockRelTol]\n");
                return false;
            }
            const double rel = std::atof(f[4].c_str());
            a.regions.push_back({f[0], std::atof(f[1].c_str()), std::atof(f[2].c_str()), std::atof(f[3].c_str()), rel,
                                 f.size() == 6 ? std::atof(f[5].c_str()) : rel});
        }
        else {
            std::fprintf(stderr, "nova_imgcmp: bad argument '%s'\n", s.c_str());
            return false;
        }
    }
    return !a.makeA.empty() ? (!a.makeB.empty() && !a.out.empty() && !a.outSigma.empty())
                            : ((!a.ref.empty() && !a.refSigma.empty()) || a.refConst.size() >= 3) && !a.test.empty() &&
                                  !a.test2.empty();
}

bool sameSize(const Image &a, const Image &b) { return a.width == b.width && a.height == b.height; }

int makeReference(const Args &a) {
    Image A, B;
    if (!readImage(a.makeA, A) || !readImage(a.makeB, B))
        return 2;
    if (!sameSize(A, B) || A.channels != B.channels) {
        std::fprintf(stderr, "nova_imgcmp: reference halves differ in size or channels\n");
        return 2;
    }
    std::vector<float> mean(A.px.size()), sigma(A.px.size());
    for (size_t i = 0; i < A.px.size(); ++i) {
        mean[i] = 0.5f * (A.px[i] + B.px[i]);
        sigma[i] = 0.5f * std::fabs(A.px[i] - B.px[i]);
    }
    const OIIO::TypeDesc type = a.floatRef ? OIIO::TypeDesc::FLOAT : OIIO::TypeDesc::HALF;
    const bool ok = writeImage(a.out, A.width, A.height, A.channels, mean.data(), type, a.attribs) &&
                    writeImage(a.outSigma, A.width, A.height, A.channels, sigma.data(), type, a.attribs);
    return ok ? 0 : 2;
}

// Deterministic comparison (--pixel-tol).
int pixelCompare(const Args &a, const Image &R, const Image &T1, const Image &T2) {
    const int W = R.width, H = R.height;
    size_t bad = 0, nonFinite = 0;
    double worst = -1.0, worstRef = 0.0, worstTest = 0.0;
    int wx = 0, wy = 0, wc = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            bool pixelBad = false;
            for (int c = 0; c < 3; ++c) {
                const double r = R.at(x, y, c), t = 0.5 * (double(T1.at(x, y, c)) + T2.at(x, y, c));
                if (!std::isfinite(r) || !std::isfinite(t)) {
                    ++nonFinite;
                    continue;
                }
                const double allowed = a.pixelAbs + a.pixelRel * std::fabs(r), diff = std::fabs(t - r);
                pixelBad |= diff > allowed;
                const double ratio = allowed > 0.0 ? diff / allowed : (diff > 0.0 ? 1e9 : 0.0);
                if (ratio > worst) {
                    worst = ratio;
                    wx = x, wy = y, wc = c;
                    worstRef = r;
                    worstTest = t;
                }
            }
            bad += pixelBad;
        }
    const double fraction = double(bad) / (double(W) * H);
    const bool pass = nonFinite == 0 && fraction <= a.pixelBadFraction;
    std::ostringstream js;
    js.precision(7);
    js << "{\n  \"pass\": " << (pass ? "true" : "false") << ",\n  \"mode\": \"pixel\",\n"
       << "  \"size\": [" << W << ", " << H << "],\n"
       << "  \"pixel_tol\": [" << a.pixelAbs << ", " << a.pixelRel << ", " << a.pixelBadFraction << "],\n"
       << "  \"pixels_failed\": " << bad << ", \"failed_fraction\": " << fraction << ",\n"
       << "  \"worst_pixel\": {\"x\": " << wx << ", \"y\": " << wy << ", \"channel\": " << wc << ", \"ref\": "
       << worstRef << ", \"test\": " << worstTest << ", \"diff_over_allowed\": " << worst << "},\n"
       << "  \"non_finite\": " << nonFinite << "\n}\n";
    std::fputs(js.str().c_str(), stdout);
    if (!a.json.empty())
        std::ofstream(a.json) << js.str();
    return pass ? 0 : 1;
}

double luminance(const float *p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; }

} // namespace

int main(int argc, char **argv) {
    Args a;
    if (!parseArgs(argc, argv, a)) {
        std::fprintf(stderr, "usage: nova_imgcmp --ref R.exr --ref-sigma RS.exr --test T1.exr --test2 T2.exr [options]\n"
                             "       nova_imgcmp --make-ref A.exr B.exr --out R.exr --out-sigma RS.exr\n");
        return 2;
    }
    if (!a.makeA.empty())
        return makeReference(a);

    Image R, RS, T1, T2;
    if (!readImage(a.test, T1) || !readImage(a.test2, T2))
        return 2;
    if (!a.refConst.empty()) {
        // Analytic reference: a constant image without variance.
        R.width = RS.width = T1.width;
        R.height = RS.height = T1.height;
        R.channels = RS.channels = a.refConst.size() >= 4 ? 4 : 3;
        R.px.resize(size_t(R.width) * R.height * R.channels);
        RS.px.assign(R.px.size(), 0.0f);
        for (size_t i = 0; i < R.px.size(); ++i)
            R.px[i] = a.refConst[i % size_t(R.channels)];
    } else if (!readImage(a.ref, R) || !readImage(a.refSigma, RS)) {
        return 2;
    }
    if (!sameSize(R, RS) || !sameSize(R, T1) || !sameSize(R, T2)) {
        std::fprintf(stderr, "nova_imgcmp: image sizes differ (ref %dx%d, test %dx%d)\n", R.width, R.height, T1.width,
                     T1.height);
        return 2;
    }
    if (a.pixelAbs >= 0.0)
        return pixelCompare(a, R, T1, T2);
    // Two renders with the same seed are identical: their difference would
    // claim zero variance and every block test would be meaningless.
    if (T1.px == T2.px) {
        std::fprintf(stderr, "nova_imgcmp: --test and --test2 are identical (were different seeds used?)\n");
        return 2;
    }
    // Alpha is compared when every image has it, unless --rgb-only.
    const int C = (!a.rgbOnly && R.channels == 4 && RS.channels == 4 && T1.channels == 4 && T2.channels == 4) ? 4 : 3;
    const int W = R.width, H = R.height, Bs = a.block;
    const int bw = (W + Bs - 1) / Bs, bh = (H + Bs - 1) / Bs, nb = bw * bh;
    const double nPix = double(W) * H;

    // Per-block sums: means, variances, and sum of squared per-pixel variances
    // (Welch-Satterthwaite degrees of freedom; each per-pixel variance has 1).
    std::vector<double> rM(size_t(nb) * C, 0.0), tM(rM), t1M(rM), t2M(rM), rV(rM), tV(rM), rV2(rM), tV2(rM);
    std::vector<double> nBlock(size_t(nb), 0.0);
    double rSum[4] = {}, tSum[4] = {}, rVar[4] = {}, tVar[4] = {}, mu = 0.0, relMse = 0.0, nOutside = 0.0;
    const size_t nr = a.regions.size();
    std::vector<double> regR(nr * 4, 0.0), regT(regR), regVR(regR), regVT(regR), regN(nr, 0.0);
    auto regionOf = [&](double x, double y) {
        for (size_t r = 0; r < nr; ++r)
            if (a.regions[r].contains(x, y))
                return int(r);
        return -1;
    };
    size_t nonFinite = 0;
    std::vector<float> refRgb(size_t(W) * H * 3), testRgb(refRgb.size()), t1Rgb(refRgb.size()), t2Rgb(refRgb.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t b = size_t(y / Bs) * bw + size_t(x / Bs), p = size_t(y) * W + x;
            nBlock[b] += 1.0;
            const int region = regionOf(x + 0.5, y + 0.5);
            if (region < 0)
                nOutside += 1.0;
            else
                regN[size_t(region)] += 1.0;
            for (int c = 0; c < C; ++c) {
                double r = R.at(x, y, c), s = RS.at(x, y, c), v1 = T1.at(x, y, c), v2 = T2.at(x, y, c);
                if (!std::isfinite(r) || !std::isfinite(s) || !std::isfinite(v1) || !std::isfinite(v2)) {
                    ++nonFinite;
                    r = s = v1 = v2 = 0.0;
                }
                const double t = 0.5 * (v1 + v2), vt = 0.25 * (v1 - v2) * (v1 - v2), vr = s * s;
                const size_t i = b * C + size_t(c);
                rM[i] += r; tM[i] += t; t1M[i] += v1; t2M[i] += v2;
                rV[i] += vr; tV[i] += vt; rV2[i] += vr * vr; tV2[i] += vt * vt;
                if (region < 0) {
                    rSum[c] += r; tSum[c] += t; rVar[c] += vr; tVar[c] += vt;
                } else {
                    const size_t ri = size_t(region) * 4 + size_t(c);
                    regR[ri] += r; regT[ri] += t; regVR[ri] += vr; regVT[ri] += vt;
                }
                if (c < 3) {
                    mu += r / (3.0 * nPix);
                    relMse += (t - r) * (t - r) / (r * r + 1e-2) / (3.0 * nPix);
                    refRgb[p * 3 + size_t(c)] = float(std::max(r, 0.0));
                    testRgb[p * 3 + size_t(c)] = float(std::max(t, 0.0));
                    t1Rgb[p * 3 + size_t(c)] = float(std::max(v1, 0.0));
                    t2Rgb[p * 3 + size_t(c)] = float(std::max(v2, 0.0));
                }
            }
        }

    // Block tests.
    const double m = double(nb) * C;
    const double zSidak = normalQuantile(1.0 - 0.5 * (1.0 - std::pow(1.0 - a.alphaFwer, 1.0 / m)));
    int failed = 0;
    double worstRatio = -1.0, biasNum = 0.0;
    int worstB = -1, worstC = 0;
    std::vector<double> rhos;
    for (int b = 0; b < nb; ++b)
        for (int c = 0; c < C; ++c) {
            const size_t i = size_t(b) * C + size_t(c);
            const double n = nBlock[size_t(b)], n2 = n * n;
            const double rb = rM[i] / n, tb = tM[i] / n, t1b = t1M[i] / n, t2b = t2M[i] / n;
            const double vR = rV[i] / n2, vT = tV[i] / n2;
            // Welch-Satterthwaite: nu = (sum v)^2 / sum(v^2 / 1), per renderer, then combined.
            const double nuR = rV2[i] > 0.0 ? rV[i] * rV[i] / rV2[i] : 1e9;
            const double nuT = tV2[i] > 0.0 ? tV[i] * tV[i] / tV2[i] : 1e9;
            const double v = vR + vT;
            const double nu = v > 0.0 ? v * v / (vR * vR / nuR + vT * vT / nuT + 1e-300) : 1e9;
            const double k = tQuantile(zSidak, nu);
            const int bx = (b % bw) * Bs, by = (b / bw) * Bs;
            const int blockRegion = regionOf(std::min(bx + 0.5 * Bs, 0.5 * (bx + W)), std::min(by + 0.5 * Bs, 0.5 * (by + H)));
            const double relTol = blockRegion >= 0 ? std::max(a.blockRelTol, a.regions[size_t(blockRegion)].blockRelTol)
                                                   : a.blockRelTol;
            const double allowed = k * std::sqrt(v) + relTol * std::fabs(rb) + a.absTol * mu;
            const double diff = std::fabs(tb - rb);
            if (diff > allowed)
                ++failed;
            const double ratio = allowed > 0.0 ? diff / allowed : (diff > 0.0 ? 1e9 : 0.0);
            if (ratio > worstRatio) {
                worstRatio = ratio;
                worstB = b;
                worstC = c;
            }
            // Informational: noise-free squared relative bias, since
            // E[(T1 - R)(T2 - R)] = bias^2 + var(R) for independent T1, T2, R.
            const double floor2 = (0.05 * mu) * (0.05 * mu);
            biasNum += ((t1b - rb) * (t2b - rb) - vR) / (rb * rb + floor2);
            if (vT > 0.0)
                rhos.push_back(std::sqrt(vR / vT));
        }
    const double relBias = std::sqrt(std::max(0.0, biasNum / m));
    double rhoMedian = 0.0;
    if (!rhos.empty()) {
        std::nth_element(rhos.begin(), rhos.begin() + rhos.size() / 2, rhos.end());
        rhoMedian = rhos[rhos.size() / 2];
    }

    // Global tests per channel (the pixels outside the regions).
    bool globalOk = true;
    double gRel[4] = {}, gZ[4] = {};
    const double nG = std::max(nOutside, 1.0);
    for (int c = 0; c < C; ++c) {
        const double rm = rSum[c] / nG, tm = tSum[c] / nG;
        const double s = std::sqrt((rVar[c] + tVar[c]) / (nG * nG));
        gRel[c] = rm != 0.0 ? (tm - rm) / std::fabs(rm) : 0.0;
        gZ[c] = s > 0.0 ? (tm - rm) / s : 0.0;
        globalOk &= std::fabs(tm - rm) <= a.kGlobal * s + a.globalRelTol * std::fabs(rm) + 1e-6 * mu;
    }
    // Region tests: each region's mean against its tolerance.
    std::ostringstream regionsJson;
    regionsJson.setf(std::ios::fixed);
    regionsJson.precision(5);
    bool regionsOk = true;
    for (size_t r = 0; r < nr; ++r) {
        const double n = std::max(regN[r], 1.0);
        bool ok = true;
        regionsJson << (r ? ",\n    " : "\n    ") << "{\"name\": \"" << a.regions[r].name << "\", \"pixels\": " << regN[r]
                    << ", \"rel_diff\": [";
        for (int c = 0; c < C; ++c) {
            const size_t ri = r * 4 + size_t(c);
            const double rm = regR[ri] / n, tm = regT[ri] / n, s = std::sqrt((regVR[ri] + regVT[ri]) / (n * n));
            ok &= std::fabs(tm - rm) <= a.kGlobal * s + a.regions[r].relTol * std::fabs(rm) + a.absTol * mu;
            regionsJson << (c ? ", " : "") << (rm != 0.0 ? (tm - rm) / std::fabs(rm) : 0.0);
        }
        regionsJson << "], \"pass\": " << (ok ? "true" : "false") << "}";
        regionsOk &= ok;
    }

    // HDR-FLIP on RGB (skipped for an all-black reference: FLIP's automatic
    // exposure range is undefined then and FLIP::evaluate calls exit()).
    double flipMean = -1.0, flipPair = -1.0, maxLum = 0.0;
    for (size_t p = 0; p < size_t(W) * H; ++p)
        maxLum = std::max(maxLum, luminance(&refRgb[p * 3]));
    if (maxLum > 0.0) {
        FLIP::Parameters params; // 67 PPD, ACES tone mapper, automatic exposures
        float meanError = 0.0f;
        float *map = nullptr;
        FLIP::evaluate(refRgb.data(), testRgb.data(), W, H, /*useHDR*/ true, params,
                       /*applyMagmaMapToOutput*/ !a.flipMap.empty(), /*computeMeanFLIPError*/ true, meanError, &map);
        flipMean = meanError;
        if (!a.flipMap.empty() && map)
            writeImage(a.flipMap, W, H, 3, map, OIIO::TypeDesc::UINT8);
        delete[] map;
        // Noise floor of the test renderer itself: FLIP between its two
        // halves. T - R has a quarter of the variance of T1 - T2 (plus the
        // reference's), so an unbiased test gives flip_mean < flip_test_pair.
        FLIP::Parameters pairParams = params; // same exposure range as above
        float pairError = 0.0f;
        float *pairMap = nullptr;
        FLIP::evaluate(t1Rgb.data(), t2Rgb.data(), W, H, true, pairParams, false, true, pairError, &pairMap);
        delete[] pairMap;
        flipPair = pairError;
    }

    const bool flipOk = a.maxFlip < 0.0 || (flipMean >= 0.0 && flipMean <= a.maxFlip);
    const bool pass = nonFinite == 0 && failed == 0 && globalOk && regionsOk && flipOk;

    std::ostringstream js;
    js.setf(std::ios::fixed);
    auto arr = [&](const double *v, int prec) {
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(prec);
        o << "[";
        for (int c = 0; c < C; ++c)
            o << (c ? ", " : "") << v[c];
        o << "]";
        return o.str();
    };
    double rMean[4], tMean[4];
    for (int c = 0; c < C; ++c) {
        rMean[c] = rSum[c] / nPix;
        tMean[c] = tSum[c] / nPix;
    }
    // Worst block position in pixels of the file (row 0 = top).
    const size_t wi = worstB >= 0 ? size_t(worstB) * C + size_t(worstC) : 0;
    const double wn = worstB >= 0 ? nBlock[size_t(worstB)] : 1.0;
    js.precision(6);
    js << "{\n  \"pass\": " << (pass ? "true" : "false") << ",\n"
       << "  \"size\": [" << W << ", " << H << "], \"block\": " << Bs << ", \"channels\": " << C << ",\n"
       << "  \"mean_ref\": " << arr(rMean, 6) << ",\n  \"mean_test\": " << arr(tMean, 6) << ",\n"
       << "  \"global_rel_diff\": " << arr(gRel, 5) << ",\n  \"global_z\": " << arr(gZ, 3) << ",\n"
       << "  \"global_pass\": " << (globalOk ? "true" : "false") << ",\n"
       << "  \"blocks_failed\": " << failed << ", \"block_tests\": " << int(m) << ", \"k_block_normal\": " << zSidak << ",\n"
       << "  \"worst_block\": {\"x\": " << (worstB >= 0 ? (worstB % bw) * Bs : 0) << ", \"y\": "
       << (worstB >= 0 ? (worstB / bw) * Bs : 0) << ", \"channel\": " << worstC << ", \"ref\": " << rM[wi] / wn
       << ", \"test\": " << tM[wi] / wn << ", \"diff_over_allowed\": " << worstRatio << "},\n"
       << "  \"rel_bias_rms\": " << relBias << ",\n  \"ref_noise_ratio\": " << rhoMedian << ",\n"
       << "  \"relmse\": " << relMse << ",\n  \"flip_mean\": " << flipMean << ", \"flip_test_pair\": " << flipPair << ",\n"
       << "  \"regions\": [" << regionsJson.str() << (nr ? "\n  " : "") << "],\n"
       << "  \"non_finite\": " << nonFinite << "\n}\n";
    std::fputs(js.str().c_str(), stdout);
    if (!a.json.empty())
        std::ofstream(a.json) << js.str();
    return pass ? 0 : 1;
}
