#include "core/film/film.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace nova {

uint8_t utilityWidth(UtilityKind kind) {
    switch (kind) {
    case UtilityKind::FacingN:
    case UtilityKind::FacingNg:
    case UtilityKind::HitDist:
    case UtilityKind::Face:
    case UtilityKind::Ao: // the unoccluded sum, written to every channel
    case UtilityKind::OutlineId:
    case UtilityKind::OutlineThickness:
    case UtilityKind::Mask:
    case UtilityKind::HitStack:
    case UtilityKind::Time:
    case UtilityKind::Near:
    case UtilityKind::Far:
    case UtilityKind::DPdz:
    case UtilityKind::Level:
    case UtilityKind::PixelSamples:
    case UtilityKind::Z:
    case UtilityKind::ObjectId:
    case UtilityKind::BakeAlpha:
        return 1;
    case UtilityKind::Primvar:
    case UtilityKind::Outline:
    case UtilityKind::OutlineContour:
    case UtilityKind::OutlineCrease:
    case UtilityKind::OutlineDepth:
    case UtilityKind::Beauty:
        return 4;
    default:
        return 3;
    }
}

namespace {

// The text of `key`'s string value in a JSON object ('"mode":"zmin"').
std::string jsonValue(const std::string &text, const char *key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = text.find(k);
    if (p == std::string::npos)
        return {};
    p = text.find(':', p + k.size());
    if (p == std::string::npos)
        return {};
    p = text.find('"', p);
    if (p == std::string::npos)
        return {};
    const size_t e = text.find('"', p + 1);
    return e == std::string::npos ? std::string() : text.substr(p + 1, e - p - 1);
}

bool keyed(SampleFilter f) {
    return f == SampleFilter::Min || f == SampleFilter::Max || f == SampleFilter::ZMin || f == SampleFilter::ZMax;
}

// Floats per pixel of a plane's second state.
size_t auxWidth(const UtilityPlane &p) {
    const SampleFilter f = p.request.filter;
    if (f == SampleFilter::Var || f == SampleFilter::Mse)
        return p.width;
    return keyed(f) ? 1 : 0;
}

void clearPlane(UtilityPlane &p) {
    const bool center = p.request.filter == SampleFilter::Center;
    std::fill(p.data.begin(), p.data.end(), center && p.request.kind == UtilityKind::Face ? -1.0f : 0.0f);
    std::fill(p.aux.begin(), p.aux.end(), keyed(p.request.filter) ? std::numeric_limits<float>::quiet_NaN() : 0.0f);
}

// Min/max order a sample by its magnitude: |v| of a scalar, the length of a
// vector (measured: Karma keeps (0, 0, 1) over (0.5, 0.5, 0.5) as the
// maximum and -3 over 1); the beauty by its colour.
float magnitude(const float *v, int width) {
    if (width == 1)
        return std::fabs(v[0]);
    const int n = width == 4 ? 3 : width;
    float s = 0.0f;
    for (int c = 0; c < n; ++c)
        s += v[c] * v[c];
    return std::sqrt(s);
}

} // namespace

SampleFilter parseSampleFilter(const std::string &text, bool primIdChannel, std::string *warning) {
    // ["name",{...}]: the first quoted string is the filter.
    const size_t q0 = text.find('"');
    const size_t q1 = q0 == std::string::npos ? q0 : text.find('"', q0 + 1);
    if (q1 == std::string::npos)
        return SampleFilter::Center;
    const std::string name = text.substr(q0 + 1, q1 - q0 - 1);
    const std::string args = text.substr(q1 + 1);
    const std::string mode = jsonValue(args, "mode");
    const auto warn = [&](const std::string &w) {
        if (warning)
            *warning = w;
    };
    if (name == "point") // Nova's: Karma renders it as ubox (measured), Nova as the centre's first hit
        return SampleFilter::Center;
    if (name == "ubox")
        return SampleFilter::Box;
    if (name == "accumulate")
        return SampleFilter::Accumulate;
    if (name == "minmax") {
        if (mode == "min")
            return SampleFilter::Min;
        if (mode == "max")
            return SampleFilter::Max;
        if (mode == "zmin")
            return SampleFilter::ZMin;
        if (mode == "zmax")
            return SampleFilter::ZMax;
        if (mode == "last")
            return SampleFilter::Last;
        // The object coverage modes need a var named PrimId; without it
        // Karma reports an error and averages, with it omin/omax pick the
        // nearest/farthest sample and the others still average (measured).
        if (mode == "omin" || mode == "omax" || mode == "edge" || mode == "ocover" || mode == "idcover") {
            if (primIdChannel && (mode == "omin" || mode == "omax"))
                return mode == "omin" ? SampleFilter::ZMin : SampleFilter::ZMax;
            if (!primIdChannel)
                warn("minmax mode " + mode + " needs a render var named PrimId (as in Karma): averaged");
            return SampleFilter::Box;
        }
        warn("unknown minmax mode '" + mode + "': the minimum (as in Karma)");
        return SampleFilter::Min;
    }
    if (name == "stats") {
        if (mode == "var")
            return SampleFilter::Var;
        if (mode == "mse")
            return SampleFilter::Mse;
        if (mode == "even")
            return SampleFilter::Even;
        if (mode == "odd")
            return SampleFilter::Odd;
        warn("unknown stats mode '" + mode + "': averaged");
        return SampleFilter::Box;
    }
    warn("unknown sample filter '" + name + "': averaged");
    return SampleFilter::Box;
}

void addFilteredSample(UtilityPlane &plane, size_t pixel, uint32_t sampleIndex, const float *v, bool hit, float z) {
    const int w = plane.width;
    float *d = plane.data.data() + pixel * size_t(w);
    const UtilityRequest &r = plane.request;
    switch (r.filter) {
    case SampleFilter::Center:
        break;
    case SampleFilter::Even:
    case SampleFilter::Odd:
        if ((sampleIndex & 1u) != (r.filter == SampleFilter::Odd ? 1u : 0u))
            break;
        [[fallthrough]];
    case SampleFilter::Box:
    case SampleFilter::Accumulate:
        if (hit)
            for (int c = 0; c < w; ++c)
                d[c] += v[c];
        break;
    case SampleFilter::Var:
    case SampleFilter::Mse:
        if (hit) {
            float *sq = plane.aux.data() + pixel * size_t(w);
            for (int c = 0; c < w; ++c) {
                d[c] += v[c];
                sq[c] += v[c] * v[c];
            }
        }
        break;
    case SampleFilter::Min:
    case SampleFilter::Max: {
        float value[4] = {r.defaultValue, r.defaultValue, r.defaultValue, r.defaultValue};
        const float *s = hit ? v : value;
        const float key = magnitude(s, w);
        float &k = plane.aux[pixel];
        if (std::isnan(k) || (r.filter == SampleFilter::Min ? key < k : key > k)) {
            k = key;
            std::copy_n(s, w, d);
        }
        break;
    }
    case SampleFilter::ZMin:
    case SampleFilter::ZMax: {
        if (!hit)
            break;
        float &k = plane.aux[pixel];
        if (std::isnan(k) || (r.filter == SampleFilter::ZMin ? z < k : z > k)) {
            k = z;
            std::copy_n(v, w, d);
        }
        break;
    }
    case SampleFilter::Last:
        for (int c = 0; c < w; ++c)
            d[c] = hit ? v[c] : 0.0f;
        break;
    }
}

void resolveFilteredSample(const UtilityPlane &plane, size_t pixel, uint32_t count, float *out) {
    const int w = plane.width;
    const float *d = plane.data.data() + pixel * size_t(w);
    const UtilityRequest &r = plane.request;
    const float n = float(count);
    switch (r.filter) {
    case SampleFilter::Box:
        for (int c = 0; c < w; ++c)
            out[c] = r.defaultValue + (count ? d[c] / n : 0.0f);
        return;
    case SampleFilter::Accumulate:
        for (int c = 0; c < w; ++c)
            out[c] = r.defaultValue + d[c];
        return;
    case SampleFilter::Even:
    case SampleFilter::Odd: {
        const uint32_t k = r.filter == SampleFilter::Even ? (count + 1) / 2 : count / 2;
        for (int c = 0; c < w; ++c)
            out[c] = k ? d[c] / float(k) : 0.0f;
        return;
    }
    case SampleFilter::Var:
    case SampleFilter::Mse: {
        const float *sq = plane.aux.data() + pixel * size_t(w);
        for (int c = 0; c < w; ++c) {
            float var = 0.0f;
            if (count >= 2)
                var = std::max(0.0f, (sq[c] - d[c] * d[c] / n) / (n - 1.0f));
            out[c] = r.filter == SampleFilter::Mse ? var / n : var;
        }
        return;
    }
    case SampleFilter::Min:
    case SampleFilter::Max:
    case SampleFilter::ZMin:
    case SampleFilter::ZMax:
        if (std::isnan(plane.aux[pixel])) {
            std::fill_n(out, w, r.defaultValue);
            return;
        }
        [[fallthrough]];
    default:
        std::copy_n(d, w, out);
        return;
    }
}

void DeepPlane::resize(size_t pixelCount) {
    pixels.assign(pixelCount, {});
}

void DeepPlane::add(size_t pixel, float z, const Spectrum &c, float alpha, const float *extras) {
    std::vector<float> &list = pixels[pixel];
    const size_t extraCount = stride - 5;
    if (request.compression > 0)
        for (size_t i = 0; i < list.size(); i += stride)
            if (std::fabs(list[i] - z) <= request.zbias) {
                list[i + 1] += c.x;
                list[i + 2] += c.y;
                list[i + 3] += c.z;
                list[i + 4] += alpha;
                for (size_t e = 0; e < extraCount; ++e)
                    list[i + 5 + e] += extras[e] * alpha;
                return;
            }
    list.push_back(z);
    list.push_back(c.x);
    list.push_back(c.y);
    list.push_back(c.z);
    list.push_back(alpha);
    for (size_t e = 0; e < extraCount; ++e)
        list.push_back(extras[e] * alpha);
}

void Film::setUtility(const std::vector<UtilityRequest> &requests) {
    utility.clear();
    for (const UtilityRequest &r : requests) {
        UtilityPlane p;
        p.request = r;
        p.width = utilityWidth(r.kind);
        p.data.resize(pixelCount() * p.width);
        p.aux.resize(pixelCount() * auxWidth(p));
        clearPlane(p);
        utility.push_back(std::move(p));
    }
}

void Film::setCrypto(const std::vector<CryptoRequest> &requests) {
    crypto.clear();
    for (const CryptoRequest &r : requests) {
        CryptoPlane p;
        p.request = r;
        p.resize(pixelCount());
        crypto.push_back(std::move(p));
    }
}

void Film::setDeep(const DeepRequest *request) {
    deep.clear();
    if (!request)
        return;
    DeepPlane p;
    p.request = *request;
    p.stride = 5;
    for (const UtilityRequest &e : request->extras)
        p.stride += utilityWidth(e.kind);
    p.resize(pixelCount());
    deep.push_back(std::move(p));
}

bool Film::wantsSampleRecords() const {
    if (!deep.empty())
        return true;
    for (const UtilityPlane &p : utility)
        if (p.request.filter != SampleFilter::Center && p.request.kind != UtilityKind::Lpe &&
            p.request.kind != UtilityKind::Ao)
            return true;
    return false;
}

bool Film::wantsLpeSamples() const {
    for (const UtilityPlane &p : utility)
        if (p.request.kind == UtilityKind::Lpe && p.request.filter != SampleFilter::Center)
            return true;
    return false;
}

UtilityPlane *Film::findUtility(UtilityKind kind, const std::string &name) {
    for (UtilityPlane &p : utility)
        if (p.request.kind == kind && p.request.name == name)
            return &p;
    return nullptr;
}

void Film::resize(int w, int h) {
    width = std::max(w, 0);
    height = std::max(h, 0);
    const size_t n = pixelCount();
    colorSum.resize(n);
    alphaSum.resize(n);
    count.resize(n);
    depth.resize(n);
    normal.resize(n);
    albedo.resize(n);
    primId.resize(n);
    instanceId.resize(n);
    lpeSum.resize(n * lpeOutputs);
    lpeLit.resize(n * lpeShadowOutputs);
    for (UtilityPlane &p : utility) {
        p.data.resize(n * p.width);
        p.aux.resize(n * auxWidth(p));
    }
    for (CryptoPlane &p : crypto)
        p.resize(n);
    for (DeepPlane &p : deep)
        p.resize(n);
    clear();
}

void Film::setLpeOutputs(uint32_t outputs, uint32_t shadowOutputs) {
    lpeOutputs = outputs;
    lpeShadowOutputs = shadowOutputs;
    lpeSum.assign(pixelCount() * outputs, Spectrum{0.0f, 0.0f, 0.0f});
    lpeLit.assign(pixelCount() * shadowOutputs, Spectrum{0.0f, 0.0f, 0.0f});
}

void Film::clear() {
    samples = 0;
    std::fill(colorSum.begin(), colorSum.end(), Spectrum{0.0f, 0.0f, 0.0f});
    std::fill(alphaSum.begin(), alphaSum.end(), 0.0f);
    std::fill(count.begin(), count.end(), 0u);
    std::fill(depth.begin(), depth.end(), 0.0f);
    std::fill(normal.begin(), normal.end(), Vec3f{0.0f, 0.0f, 0.0f});
    std::fill(albedo.begin(), albedo.end(), Spectrum{0.0f, 0.0f, 0.0f});
    std::fill(primId.begin(), primId.end(), -1);
    std::fill(instanceId.begin(), instanceId.end(), -1);
    std::fill(lpeSum.begin(), lpeSum.end(), Spectrum{0.0f, 0.0f, 0.0f});
    std::fill(lpeLit.begin(), lpeLit.end(), Spectrum{0.0f, 0.0f, 0.0f});
    for (UtilityPlane &p : utility)
        clearPlane(p);
    for (CryptoPlane &p : crypto)
        p.resize(pixelCount());
    for (DeepPlane &p : deep)
        p.resize(pixelCount());
}

// --- Checkpoints -----------------------------------------------------------

namespace {

// Version 2: each plane's key holds its whole request (planeKey).
constexpr char kFilmMagic[8] = {'N', 'O', 'V', 'A', 'F', 'L', 'M', '2'};

struct Writer {
    std::vector<uint8_t> &out;
    void bytes(const void *p, size_t n) {
        const auto *b = static_cast<const uint8_t *>(p);
        out.insert(out.end(), b, b + n);
    }
    void u64(uint64_t v) { bytes(&v, sizeof(v)); }
    void f32(float v) { bytes(&v, sizeof(v)); }
    void str(const std::string &s) {
        u64(s.size());
        bytes(s.data(), s.size());
    }
    template <typename T>
    void array(const std::vector<T> &v) {
        u64(v.size());
        if (!v.empty())
            bytes(v.data(), v.size() * sizeof(T));
    }
};

// What tells planes of one kind apart (a checkpoint resumes only into the
// same planes): a utility plane's kind, filter, width, source name, LPE
// output, holdout prefixes and default value; a cryptomatte's source and
// primvar; the deep map's settings and extras.
void utilityKey(Writer &w, const UtilityRequest &r, uint8_t width) {
    w.u64(uint64_t(r.kind) | uint64_t(r.filter) << 8 | uint64_t(width) << 16 | uint64_t(r.holdouts) << 24);
    w.u64(uint64_t(int64_t(r.index)));
    w.f32(r.defaultValue);
    w.str(r.name);
}
std::vector<uint8_t> planeKey(const UtilityPlane &p) {
    std::vector<uint8_t> key;
    Writer w{key};
    utilityKey(w, p.request, p.width);
    return key;
}
std::vector<uint8_t> planeKey(const CryptoPlane &p) {
    std::vector<uint8_t> key;
    Writer w{key};
    w.u64(uint64_t(p.request.source));
    w.str(p.request.primvar);
    return key;
}
std::vector<uint8_t> planeKey(const DeepPlane &p) {
    std::vector<uint8_t> key;
    Writer w{key};
    const DeepRequest &r = p.request;
    w.u64(p.stride);
    w.f32(r.zbias);
    w.u64(uint64_t(int64_t(r.compression)) ^ uint64_t(r.hitDistance) << 62 ^ uint64_t(r.excludeHoldouts) << 63);
    w.u64(r.extras.size());
    for (const UtilityRequest &e : r.extras)
        utilityKey(w, e, utilityWidth(e.kind));
    return key;
}

struct Reader {
    const uint8_t *p;
    const uint8_t *end;
    bool ok = true;
    bool bytes(void *dst, size_t n) {
        if (!ok || size_t(end - p) < n)
            return ok = false;
        std::memcpy(dst, p, n);
        p += n;
        return true;
    }
    uint64_t u64() {
        uint64_t v = 0;
        bytes(&v, sizeof(v));
        return v;
    }
    // Reads an array whose size must equal `expected` (the film's layout).
    template <typename T>
    bool array(std::vector<T> &v, size_t expected) {
        if (u64() != expected || !ok)
            return ok = false;
        v.resize(expected);
        return expected == 0 || bytes(v.data(), expected * sizeof(T));
    }
    // Reads a plane's key: true if it equals `key` (planeKey).
    bool key(const std::vector<uint8_t> &key) {
        std::vector<uint8_t> k;
        return array(k, key.size()) && k == key;
    }
};

} // namespace

void Film::save(std::vector<uint8_t> &out) const {
    out.clear();
    Writer w{out};
    w.bytes(kFilmMagic, sizeof(kFilmMagic));
    w.u64(uint64_t(width));
    w.u64(uint64_t(height));
    w.u64(samples);
    w.array(colorSum);
    w.array(alphaSum);
    w.array(count);
    w.array(depth);
    w.array(normal);
    w.array(albedo);
    w.array(primId);
    w.array(instanceId);
    w.u64(lpeOutputs);
    w.u64(lpeShadowOutputs);
    w.array(lpeSum);
    w.array(lpeLit);
    w.u64(utility.size());
    for (const UtilityPlane &p : utility) {
        w.array(planeKey(p));
        w.array(p.data);
        w.array(p.aux);
    }
    w.u64(crypto.size());
    for (const CryptoPlane &p : crypto) {
        w.array(planeKey(p));
        w.array(p.ids);
        w.array(p.weights);
    }
    w.u64(deep.size());
    for (const DeepPlane &p : deep) {
        w.array(planeKey(p));
        for (const std::vector<float> &list : p.pixels)
            w.array(list);
    }
}

bool Film::load(const uint8_t *data, size_t size) {
    Reader r{data, data + size};
    char magic[8];
    if (!r.bytes(magic, sizeof(magic)) || std::memcmp(magic, kFilmMagic, sizeof(magic)) != 0)
        return false;
    if (r.u64() != uint64_t(width) || r.u64() != uint64_t(height) || !r.ok)
        return false;
    // Into a copy: a mismatch half way leaves this film as it was.
    Film f = *this;
    f.samples = uint32_t(r.u64());
    const size_t n = pixelCount();
    r.array(f.colorSum, n);
    r.array(f.alphaSum, n);
    r.array(f.count, n);
    r.array(f.depth, n);
    r.array(f.normal, n);
    r.array(f.albedo, n);
    r.array(f.primId, n);
    r.array(f.instanceId, n);
    if (r.u64() != lpeOutputs || r.u64() != lpeShadowOutputs)
        return false;
    r.array(f.lpeSum, lpeSum.size());
    r.array(f.lpeLit, lpeLit.size());
    if (r.u64() != utility.size())
        return false;
    for (UtilityPlane &p : f.utility) {
        if (!r.key(planeKey(p)))
            return false;
        r.array(p.data, p.data.size());
        r.array(p.aux, p.aux.size());
    }
    if (r.u64() != crypto.size())
        return false;
    for (CryptoPlane &p : f.crypto) {
        if (!r.key(planeKey(p)))
            return false;
        r.array(p.ids, p.ids.size());
        r.array(p.weights, p.weights.size());
    }
    if (r.u64() != deep.size())
        return false;
    for (DeepPlane &p : f.deep) {
        if (!r.key(planeKey(p)))
            return false;
        for (std::vector<float> &list : p.pixels) {
            const uint64_t k = r.u64();
            if (!r.ok || k % p.stride != 0 || k > size_t(r.end - r.p) / sizeof(float))
                return false;
            list.resize(k);
            if (k && !r.bytes(list.data(), k * sizeof(float)))
                return false;
        }
    }
    if (!r.ok || r.p != r.end)
        return false;
    *this = std::move(f);
    return true;
}

} // namespace nova
