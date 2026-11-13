#include "core/lights/ies.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace nova {

namespace {

constexpr double kPiD = 3.14159265358979323846;
// The grid: half a degree vertically, one degree horizontally.
constexpr uint32_t kRows = 361, kColumns = 361;

bool fail(std::string *error, const char *message) {
    if (error)
        *error = message;
    return false;
}

// The numbers of an LM-63 file after its TILT line: separated by blanks,
// line ends or commas.
bool readNumbers(const std::string &text, size_t from, std::vector<double> &out) {
    const char *p = text.c_str() + from, *end = text.c_str() + text.size();
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ','))
            ++p;
        if (p >= end)
            break;
        char *next = nullptr;
        const double v = std::strtod(p, &next);
        if (next == p) // not a number: the file's numbers end here (trailing text)
            break;
        if (!std::isfinite(v))
            return false;
        out.push_back(v);
        p = next;
    }
    return true;
}

// How Karma draws a table between its entries (measured with tables of one
// spike and of one step, docs/bug-audit-2026-11.md section 22): not by
// straight lines but smoothed, each entry spread over 1.63 of its steps to
// either side. An entry of 1000 among zeros is 790 at its own angle, 497 half
// a step away and 105 at its neighbours' angles, with the area of the
// straight lines' triangle; a step from 0 to 1000 is 110 at the angle of the
// last 0, 500 half-way and 896 at the first 1000. That is the straight
// lines' interpolation drawn with a triangle filter 0.63 steps wide to
// either side, in steps of the table: where the steps differ, each side of
// an entry has its own. kernel(x): the weight of an entry x steps away.
constexpr double kReach = 1.63;
double kernel(double x) {
    constexpr double h = 0.63;
    // The triangle of half-width 1 integrated twice: (x + 1)^3 - 2 x^3 + (x - 1)^3 over 6, each
    // term from where it starts.
    const auto g = [](double t) {
        const auto cube = [](double v) { return v > 0.0 ? v * v * v : 0.0; };
        return (cube(t + 1.0) - 2.0 * cube(t) + cube(t - 1.0)) / 6.0;
    };
    if (std::fabs(x) >= kReach)
        return 0.0;
    return std::max((g(x + h) - 2.0 * g(x) + g(x - h)) / (h * h), 0.0); // the cubes cancel to the last bits
}

// The file's table: candela[h][v] at its vertical and horizontal angles.
struct Table {
    std::vector<double> vertical, horizontal, candela;
    // How the horizontal angles stand for the whole circle: one angle (the
    // same all around); 0..360 (the last column is the first again); a part
    // that the rest mirrors, 0..90 (four quadrants), 0..180 (two halves) or
    // 90..270 (two halves about that plane); anything else is held at its ends.
    enum class Around { One, Whole, Mirrored, Held } around = Around::One;

    void classify() {
        const double first = horizontal.front(), last = horizontal.back();
        const auto is = [](double a, double b) { return std::fabs(a - b) < 1e-3; };
        around = horizontal.size() == 1                                                          ? Around::One
                 : is(first, 0.0) && is(last, 360.0)                                             ? Around::Whole
                 : (is(first, 0.0) && (is(last, 90.0) || is(last, 180.0))) || (is(first, 90.0) && is(last, 270.0))
                     ? Around::Mirrored
                     : Around::Held;
    }

    double at(size_t h, size_t v) const { return candela[h * vertical.size() + v]; }

    // Where an angle lies among rising angles, in steps: i + t between
    // entries i and i + 1, and on beyond the ends by the end's own step.
    static double steps(const std::vector<double> &angles, double a) {
        const size_t n = angles.size();
        if (n == 1)
            return 0.0;
        size_t i1 = size_t(std::upper_bound(angles.begin(), angles.end(), a) - angles.begin());
        i1 = std::clamp<size_t>(i1, 1, n - 1);
        const size_t i0 = i1 - 1;
        return double(i0) + (a - angles[i0]) / (angles[i1] - angles[i0]);
    }

    // The column of a step count that may lie beyond the file's columns.
    size_t column(long long j) const {
        const long long n = (long long)horizontal.size();
        if (around == Around::One)
            return 0;
        if (around == Around::Whole) { // n - 1 steps around
            const long long m = ((j % (n - 1)) + (n - 1)) % (n - 1);
            return size_t(m);
        }
        if (around == Around::Mirrored) { // mirrored at both ends
            const long long period = 2 * (n - 1);
            long long m = ((j % period) + period) % period;
            if (m > n - 1)
                m = period - m;
            return size_t(m);
        }
        return size_t(std::clamp<long long>(j, 0, n - 1));
    }

    // The horizontal angle within the file's range, by its symmetry.
    double folded(double phi) const {
        const double first = horizontal.front(), last = horizontal.back();
        double p = std::fmod(phi, 360.0);
        if (p < 0.0)
            p += 360.0;
        if (around == Around::Mirrored) {
            if (first == 0.0 && last < 135.0) { // 0..90
                p = std::fmod(p, 180.0);
                if (p > 90.0)
                    p = 180.0 - p;
            } else if (first == 0.0) { // 0..180
                if (p > 180.0)
                    p = 360.0 - p;
            } else { // 90..270
                if (p < 90.0)
                    p = 180.0 - p;
                else if (p > 270.0)
                    p = 540.0 - p;
            }
        }
        return std::clamp(p, first, last);
    }

    // The candela at a vertical and a horizontal angle (degrees), as Karma
    // draws the table: beyond the vertical angles the first or last row
    // holds (the rows beyond an end are that end's).
    double lookup(double theta, double phi) const {
        const long long nv = (long long)vertical.size();
        // Beyond the ends the steps go on by the end's own step.
        double u = steps(vertical, theta);
        const long long v0 = (long long)std::floor(u - kReach), v1 = (long long)std::ceil(u + kReach);
        const bool one = around == Around::One;
        const double w = one ? 0.0 : steps(horizontal, folded(phi));
        const long long h0 = one ? 0 : (long long)std::floor(w - kReach), h1 = one ? 0 : (long long)std::ceil(w + kReach);
        double sum = 0.0, weights = 0.0;
        for (long long v = v0; v <= v1; ++v) {
            const double kv = nv == 1 ? (v == 0 ? 1.0 : 0.0) : kernel(u - double(v));
            if (kv == 0.0)
                continue;
            const size_t row = size_t(std::clamp<long long>(v, 0, nv - 1));
            for (long long h = h0; h <= h1; ++h) {
                const double kh = one ? 1.0 : kernel(w - double(h));
                if (kh == 0.0)
                    continue;
                sum += kv * kh * at(column(h), row);
                weights += kv * kh;
            }
        }
        return weights > 0.0 ? sum / weights : 0.0;
    }
};

} // namespace

std::shared_ptr<const IesProfile> parseIesProfile(const std::string &text, std::string *error) {
    const auto none = [&](const char *message) {
        fail(error, message);
        return std::shared_ptr<const IesProfile>();
    };
    // The header ends with the TILT line.
    size_t tilt = std::string::npos;
    for (size_t at = 0; at < text.size();) {
        const size_t eol = std::min(text.find('\n', at), text.size());
        size_t begin = at;
        while (begin < eol && (text[begin] == ' ' || text[begin] == '\t'))
            ++begin;
        if (eol - begin >= 5 && (text[begin] == 'T' || text[begin] == 't') && (text[begin + 1] == 'I' || text[begin + 1] == 'i') &&
            (text[begin + 2] == 'L' || text[begin + 2] == 'l') && (text[begin + 3] == 'T' || text[begin + 3] == 't') &&
            text[begin + 4] == '=') {
            tilt = begin;
            break;
        }
        at = eol + 1;
    }
    if (tilt == std::string::npos)
        return none("no TILT line: not an IES (LM-63) file");
    const size_t eol = std::min(text.find('\n', tilt), text.size());
    std::string mode = text.substr(tilt + 5, eol - tilt - 5);
    while (!mode.empty() && (mode.back() == '\r' || mode.back() == ' ' || mode.back() == '\t'))
        mode.pop_back();
    while (!mode.empty() && (mode.front() == ' ' || mode.front() == '\t'))
        mode.erase(mode.begin());
    for (char &c : mode)
        c = char(std::toupper(static_cast<unsigned char>(c)));

    std::vector<double> n;
    if (!readNumbers(text, eol, n))
        return none("a number of the file is not finite");
    size_t k = 0;
    if (mode == "INCLUDE") { // the lamp's tilt: its geometry, the count of angles, the angles and their factors
        if (n.size() < 2 || !(n[1] >= 0.0) || n[1] > 1e6)
            return none("the TILT data is incomplete");
        k = 2 + 2 * size_t(n[1]);
    }
    // Lamps, lumens, multiplier, vertical and horizontal angle counts, photometric type, units, width, length,
    // height; ballast factor, ballast-lamp factor, watts.
    if (n.size() < k + 13)
        return none("the file ends before its angles");
    const double multiplier = n[k + 2];
    const double nvd = n[k + 3], nhd = n[k + 4];
    if (!(nvd >= 1.0) || !(nhd >= 1.0) || nvd > 100000.0 || nhd > 100000.0 || nvd != std::floor(nvd) || nhd != std::floor(nhd))
        return none("the counts of angles are not counts");
    const size_t nv = size_t(nvd), nh = size_t(nhd);
    const int type = int(n[k + 5]);
    k += 13;
    if (n.size() < k + nv + nh + nv * nh)
        return none("the file ends before its last candela value");
    Table table;
    table.vertical.assign(n.begin() + std::ptrdiff_t(k), n.begin() + std::ptrdiff_t(k + nv));
    k += nv;
    table.horizontal.assign(n.begin() + std::ptrdiff_t(k), n.begin() + std::ptrdiff_t(k + nh));
    k += nh;
    table.candela.assign(n.begin() + std::ptrdiff_t(k), n.begin() + std::ptrdiff_t(k + nv * nh));
    for (double &c : table.candela)
        c = std::max(c * multiplier, 0.0);
    for (size_t i = 1; i < nv; ++i)
        if (!(table.vertical[i] > table.vertical[i - 1]))
            return none("the vertical angles do not rise");
    for (size_t i = 1; i < nh; ++i)
        if (!(table.horizontal[i] > table.horizontal[i - 1]))
            return none("the horizontal angles do not rise");
    if (table.vertical.front() < 0.0 || table.vertical.back() > 180.0)
        return none("a vertical angle is outside 0 to 180 degrees");
    table.classify();

    auto profile = std::make_shared<IesProfile>();
    profile->photometricType = type;
    profile->height = kRows;
    profile->width = nh > 1 ? kColumns : 1;
    profile->candela.resize(size_t(profile->width) * profile->height);
    // The means: each row's share of the sphere is the band half a step around it.
    const double step = 180.0 / double(kRows - 1), lo = table.vertical.front(), hi = table.vertical.back();
    double sum = 0.0, weight = 0.0, sphereSum = 0.0, sphereWeight = 0.0;
    float largest = 0.0f;
    for (uint32_t r = 0; r < kRows; ++r) {
        const double theta = double(r) * step;
        double rowSum = 0.0;
        for (uint32_t c = 0; c < profile->width; ++c) {
            const double phi = profile->width > 1 ? double(c) * 360.0 / double(profile->width - 1) : 0.0;
            const double v = table.lookup(theta, phi);
            profile->candela[size_t(r) * profile->width + c] = float(v);
            largest = std::max(largest, float(v));
            if (c + 1 < profile->width || profile->width == 1) // the last column is the first again
                rowSum += v;
        }
        const double rowMean = rowSum / double(profile->width > 1 ? profile->width - 1 : 1);
        const auto band = [&](double a, double b) { // the solid angle between two vertical angles, over 2 pi
            return a < b ? std::cos(a * kPiD / 180.0) - std::cos(b * kPiD / 180.0) : 0.0;
        };
        const double a = std::max(theta - 0.5 * step, 0.0), b = std::min(theta + 0.5 * step, 180.0);
        sphereSum += rowMean * band(a, b);
        sphereWeight += band(a, b);
        const double w = band(std::max(a, lo), std::min(b, hi));
        sum += rowMean * w;
        weight += w;
    }
    profile->maxCandela = largest;
    profile->sphereMean = float(sphereWeight > 0.0 ? sphereSum / sphereWeight : 0.0);
    // A file of one vertical angle covers no solid angle: its value.
    profile->mean = float(weight > 0.0 ? sum / weight : table.lookup(lo, 0.0));
    return profile;
}

float iesCandela(const IesProfile &profile, float vertical, float horizontal) {
    if (profile.height == 0 || profile.width == 0)
        return 0.0f;
    const float row = std::clamp(vertical, 0.0f, 180.0f) * float(profile.height - 1) * (1.0f / 180.0f);
    const uint32_t r0 = std::min(uint32_t(row), profile.height - 1), r1 = std::min(r0 + 1, profile.height - 1);
    const float tr = row - float(r0);
    const float *t = profile.candela.data();
    if (profile.width == 1)
        return t[r0] * (1.0f - tr) + t[r1] * tr;
    float phi = std::fmod(horizontal, 360.0f);
    if (phi < 0.0f)
        phi += 360.0f;
    const float column = phi * float(profile.width - 1) * (1.0f / 360.0f);
    const uint32_t c0 = std::min(uint32_t(column), profile.width - 2), c1 = c0 + 1;
    const float tc = column - float(c0);
    const float a = t[size_t(r0) * profile.width + c0] * (1.0f - tc) + t[size_t(r0) * profile.width + c1] * tc;
    const float b = t[size_t(r1) * profile.width + c0] * (1.0f - tc) + t[size_t(r1) * profile.width + c1] * tc;
    return a * (1.0f - tr) + b * tr;
}

} // namespace nova
