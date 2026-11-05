#pragma once

// Motion sampling for Hydra prims: everything that moves is resampled to
// uniform steps over the camera shutter, the layout the core and Embree use.
//
// Under Hydra 2 emulation the Sample* overloads without an interval return
// no motion, so the (startTime, endTime) overloads are always used. Their
// samples are frame-relative and not uniform: transforms bring the interval
// ends plus inner samples, points may bring bracketing samples outside the
// shutter.

#include "core/math/mat.h"
#include "core/scene/motion.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/timeSampleArray.h>
#include <pxr/pxr.h>

#include <algorithm>
#include <cmath>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// Shutter interval in frames relative to the current time.
struct HdNovaShutter {
    float open = 0.0f, close = 0.0f;
    bool IsBlurred() const { return close - open > 1e-6f; }
    float TimeAt(float u) const { return open + u * (close - open); }
    // Normalized shutter position of the frame itself (for the AOV pass).
    float FrameTime() const { return IsBlurred() ? std::clamp(-open / (close - open), 0.0f, 1.0f) : 0.0f; }
    // The frame itself, clamped into the shutter (frames relative to now).
    float FrameOffset() const { return std::clamp(0.0f, open, close); }
    bool operator==(const HdNovaShutter &) const = default;
};

// UsdRender semantics: disableMotionBlur renders the current time only;
// instantaneousShutter collapses the shutter to its open time.
inline HdNovaShutter HdNovaResolveShutter(double open, double close, bool disableMotionBlur, bool instantaneousShutter) {
    HdNovaShutter s;
    if (disableMotionBlur)
        return s;
    s.open = float(open);
    s.close = instantaneousShutter ? float(open) : float(close);
    if (!(s.close >= s.open)) // "no exposure" in USD; render instantaneously instead
        s.close = s.open;
    return s;
}

inline nova::Mat4 HdNovaToMat4(GfMatrix4d const &m) {
    nova::Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = m[i][j];
    return r;
}

// Uniform step count that resolves the finest authored spacing inside the
// shutter (Karma's "motion samples from stage"); samples outside only serve
// interpolation.
inline unsigned HdNovaMotionStepCount(const float *times, size_t count, const HdNovaShutter &sh, unsigned maxSteps) {
    if (!sh.IsBlurred() || count < 2)
        return 1;
    const float length = sh.close - sh.open;
    float minGap = length, prev = sh.open;
    for (size_t i = 0; i < count; ++i) {
        if (times[i] <= sh.open || times[i] >= sh.close)
            continue;
        minGap = std::min(minGap, times[i] - prev);
        prev = times[i];
    }
    minGap = std::min(minGap, sh.close - prev);
    const float segments = std::ceil(length / std::max(minGap, length * 1e-3f) - 1e-3f);
    return std::clamp(unsigned(segments) + 1u, 2u, std::max(maxSteps, 2u));
}

// A vec3f primvar (points, normals) at uniform shutter steps. One step if
// nothing moves or the array size changes over time (topology change).
inline bool HdNovaSampleVec3fArray(HdSceneDelegate *sd, SdfPath const &id, TfToken const &key,
                                   const HdNovaShutter &sh, unsigned maxSteps, std::vector<VtVec3fArray> *steps) {
    steps->clear();
    // The value at the frame, consistent with the topology at the frame.
    const VtValue frameValue = sd->Get(id, key);
    if (!frameValue.IsHolding<VtVec3fArray>())
        return false;
    const VtVec3fArray &frame = frameValue.UncheckedGet<VtVec3fArray>();
    const auto atFrame = [&] {
        steps->assign(1, frame);
        return true;
    };
    if (!sh.IsBlurred())
        return atFrame();
    HdTimeSampleArray<VtValue, 8> boxed;
    sd->SamplePrimvar(id, key, sh.open, sh.close, &boxed); // the interval overload
    HdTimeSampleArray<VtVec3fArray, 8> samples;
    if (boxed.count == 0 || !samples.UnboxFrom(boxed))
        return atFrame();
    // A size that differs from the frame's (fluids, fracture), also when the
    // shutter excludes the frame and only the samples inside it agree: the
    // frame's points without motion, as USD holds them and Karma renders
    // them (the topology is the frame's). Never Resample() across such
    // samples: HdResampleNeighbors reads both arrays up to the first one's size.
    for (size_t i = 0; i < samples.count; ++i)
        if (samples.values[i].size() != frame.size())
            return atFrame();
    const unsigned n = HdNovaMotionStepCount(samples.times.data(), samples.count, sh, maxSteps);
    steps->resize(n);
    for (unsigned k = 0; k < n; ++k)
        (*steps)[k] = samples.Resample(n == 1 ? sh.FrameOffset() : sh.TimeAt(float(k) / float(n - 1)));
    if (n > 1 && std::all_of(steps->begin() + 1, steps->end(), [&](const VtVec3fArray &a) { return a == (*steps)[0]; }))
        steps->resize(1); // no actual motion
    return true;
}

// Interpolates sampled matrices at time t: decomposed, rotation slerped
// (Karma's default rotation blur), translation lerped in double. Equal
// neighbours are returned exactly, so static samples stay bit-identical.
inline GfMatrix4d HdNovaInterpolateTransform(const float *times, const GfMatrix4d *values,
                                             const nova::SrtTransform *keys, size_t count, float t) {
    size_t i = 0;
    while (i + 1 < count && times[i + 1] <= t)
        ++i;
    if (i + 1 >= count || t <= times[i] || values[i] == values[i + 1])
        return values[std::min(i, count - 1)];
    const float f = (t - times[i]) / (times[i + 1] - times[i]);
    const nova::Affine3f a = nova::interpolateSrt(keys[i], keys[i + 1], f);
    GfMatrix4d m(1.0);
    const nova::Vec3f rows[3] = {a.x, a.y, a.z};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r][c] = rows[r][c];
    for (int c = 0; c < 3; ++c)
        m[3][c] = (1.0 - f) * values[i][3][c] + f * values[i + 1][3][c];
    return m;
}

// The authored samples of a transform over the shutter (frame-relative times,
// ascending, possibly bracketing it) with their decompositions; one sample at
// the frame without motion.
struct HdNovaTransformSamples {
    std::vector<float> times;
    std::vector<GfMatrix4d> values;
    std::vector<nova::SrtTransform> keys;

    void Assign(const float *t, const GfMatrix4d *v, size_t count) {
        times.assign(t, t + count);
        values.assign(v, v + count);
        keys.resize(count);
        for (size_t i = 0; i < count; ++i)
            keys[i] = nova::decomposeSrt(HdNovaToMat4(values[i]));
        nova::alignQuaternions(keys.data(), uint32_t(count));
    }
    unsigned StepCount(const HdNovaShutter &sh, unsigned maxSteps) const {
        return HdNovaMotionStepCount(times.data(), times.size(), sh, maxSteps);
    }
    GfMatrix4d At(float t) const {
        if (values.size() < 2)
            return values.empty() ? GfMatrix4d(1.0) : values[0];
        return HdNovaInterpolateTransform(times.data(), values.data(), keys.data(), values.size(), t);
    }
};

// A prim's transform samples (`instancer`: of an instancer).
inline void HdNovaSampleTransformSamples(HdSceneDelegate *sd, SdfPath const &id, const HdNovaShutter &sh,
                                         HdNovaTransformSamples *out, bool instancer = false) {
    if (sh.IsBlurred()) {
        HdTimeSampleArray<GfMatrix4d, 4> s;
        if (instancer)
            sd->SampleInstancerTransform(id, sh.open, sh.close, &s);
        else
            sd->SampleTransform(id, sh.open, sh.close, &s);
        if (s.count > 0) {
            out->Assign(s.times.data(), s.values.data(), s.count);
            return;
        }
    }
    const float t = 0.0f;
    const GfMatrix4d m = instancer ? sd->GetInstancerTransform(id) : sd->GetTransform(id);
    out->Assign(&t, &m, 1);
}

// A prim's transform at uniform shutter steps (one step if static).
inline void HdNovaSampleTransform(HdSceneDelegate *sd, SdfPath const &id, const HdNovaShutter &sh, unsigned maxSteps,
                                  std::vector<GfMatrix4d> *steps) {
    HdNovaTransformSamples s;
    HdNovaSampleTransformSamples(sd, id, sh, &s);
    const unsigned n = s.StepCount(sh, maxSteps);
    steps->resize(n);
    for (unsigned k = 0; k < n; ++k)
        (*steps)[k] = s.At(n == 1 ? sh.FrameOffset() : sh.TimeAt(float(k) / float(n - 1)));
    if (n > 1 && std::all_of(steps->begin() + 1, steps->end(), [&](const GfMatrix4d &m) { return m == (*steps)[0]; }))
        steps->resize(1);
}

// The render camera path arrives as std::string (husk's initial settings),
// SdfPath (Solaris) or token.
inline SdfPath HdNovaPathFromValue(VtValue const &v) {
    if (v.IsHolding<SdfPath>())
        return v.UncheckedGet<SdfPath>();
    if (v.IsHolding<std::string>() && !v.UncheckedGet<std::string>().empty())
        return SdfPath(v.UncheckedGet<std::string>());
    if (v.IsHolding<TfToken>() && !v.UncheckedGet<TfToken>().IsEmpty())
        return SdfPath(v.UncheckedGet<TfToken>().GetString());
    return SdfPath();
}

PXR_NAMESPACE_CLOSE_SCOPE
