#include "instancer.h"
#include "primvarUtil.h"

#include "renderParam.h"

#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/work/loops.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/timeSampleArray.h>
#include <pxr/imaging/hd/tokens.h>

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

nova::Xform34 toXform34(GfMatrix4d const &m) {
    nova::Xform34 x;
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 3; ++k)
            x.c[r][k] = float(m[r][k]);
    return x;
}

template <class T>
T const *typedData(VtValue const &v, size_t &size) {
    if (!v.IsHolding<VtArray<T>>())
        return nullptr;
    VtArray<T> const &a = v.UncheckedGet<VtArray<T>>();
    size = a.size();
    return a.cdata();
}

// Typed views, resolved once per compute call instead of per instance.
struct Vec3View {
    GfVec3f const *f = nullptr;
    GfVec3d const *d = nullptr;
    GfVec3h const *h = nullptr;
    size_t n = 0;
    explicit Vec3View(VtValue const &v) {
        if (!(f = typedData<GfVec3f>(v, n)) && !(d = typedData<GfVec3d>(v, n)))
            h = typedData<GfVec3h>(v, n);
    }
    bool get(int i, GfVec3d &out) const {
        if (i < 0 || size_t(i) >= n)
            return false; // missing entry: identity, like Storm and hdEmbree
        out = f ? GfVec3d(f[i]) : d ? d[i] : GfVec3d(h[i]);
        return true;
    }
};

// GfQuath/GfQuatf/GfQuatd keep the imaginary part FIRST in memory
// (GfVec3x _imaginary; T _real;), so they are read through GetReal() and
// GetImaginary() and never reinterpreted as a vec4 (hdEmbree's bug).
struct QuatView {
    GfQuath const *h = nullptr;
    GfQuatf const *f = nullptr;
    GfQuatd const *d = nullptr;
    GfVec4f const *v4 = nullptr; // legacy "rotate" primvar: <real, i, j, k>
    size_t n = 0;
    explicit QuatView(VtValue const &v) {
        if (!(h = typedData<GfQuath>(v, n)) && !(f = typedData<GfQuatf>(v, n)) && !(d = typedData<GfQuatd>(v, n)))
            v4 = typedData<GfVec4f>(v, n);
    }
    bool get(int i, GfQuatd &out) const {
        if (i < 0 || size_t(i) >= n)
            return false;
        if (h)
            out = GfQuatd(h[i].GetReal(), GfVec3d(h[i].GetImaginary()));
        else if (f)
            out = GfQuatd(f[i].GetReal(), GfVec3d(f[i].GetImaginary()));
        else if (d)
            out = d[i];
        else
            out = GfQuatd(v4[i][0], v4[i][1], v4[i][2], v4[i][3]);
        return true;
    }
};

struct MatrixView {
    GfMatrix4d const *d = nullptr;
    GfMatrix4f const *f = nullptr;
    size_t n = 0;
    explicit MatrixView(VtValue const &v) {
        if (!(d = typedData<GfMatrix4d>(v, n)))
            f = typedData<GfMatrix4f>(v, n);
    }
    bool get(int i, GfMatrix4d &out) const {
        if (i < 0 || size_t(i) >= n)
            return false;
        out = d ? d[i] : GfMatrix4d(f[i]);
        return true;
    }
};

// The samples bracketing time t: index i and fraction f between samples i
// and i + 1 (clamped at the ends). False for fewer than two samples.
bool bracket(std::vector<float> const &times, float t, size_t &i, float &f) {
    i = 0;
    f = 0.0f;
    if (times.size() < 2)
        return false;
    while (i + 2 < times.size() && times[i + 1] <= t)
        ++i;
    const float t0 = times[i], t1 = times[i + 1];
    f = t1 > t0 ? std::clamp((t - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
    return true;
}

GfMatrix4d interpolateMatrix(GfMatrix4d const &a, GfMatrix4d const &b, float f) {
    const float times[2] = {0.0f, 1.0f};
    const GfMatrix4d values[2] = {a, b};
    nova::SrtTransform keys[2] = {nova::decomposeSrt(HdNovaToMat4(a)), nova::decomposeSrt(HdNovaToMat4(b))};
    nova::alignQuaternions(keys, 2);
    return HdNovaInterpolateTransform(times, values, keys, 2, f);
}

// Element interpolation like UsdGeomPointInstancer: positions and scales
// linearly, orientations along the shortest arc.
struct MixLinear {
    static GfVec3d Mix(GfVec3d const &a, GfVec3d const &b, float f) { return a * (1.0 - f) + b * f; }
};
struct MixSlerp {
    static GfQuatd Mix(GfQuatd const &a, GfQuatd const &b, float f) { return GfSlerp(double(f), a, b); }
};
struct MixMatrix {
    static GfMatrix4d Mix(GfMatrix4d const &a, GfMatrix4d const &b, float f) { return interpolateMatrix(a, b, f); }
};

// An instance primvar at one time: views of the samples bracketing it and
// the weight of the later one. Elements are mixed on access, so a prototype
// touches only its own instances instead of interpolating whole arrays.
// Equal neighbours stay bit-identical (static instances keep equal keys).
template <class View, class T, class M>
struct Track {
    View a{VtValue()}, b{VtValue()};
    float f = 0.0f; // weight of b; 0 = a alone

    // `values` must outlive the track. All samples have the same length
    // (HdNovaInstancer::_SamplePrimvars keeps only the frame otherwise).
    Track(std::vector<float> const &times, std::vector<VtValue> const &values, float t) {
        size_t i;
        float w;
        if (!bracket(times, t, i, w)) {
            if (!values.empty())
                a = View(values[0]);
            return;
        }
        a = View(values[w >= 1.0f ? i + 1 : i]);
        if (w > 0.0f && w < 1.0f) {
            b = View(values[i + 1]);
            f = w;
        }
    }
    bool get(int i, T &out) const {
        if (!a.get(i, out))
            return false; // missing entry: identity
        T later;
        if (f > 0.0f && b.get(i, later) && !(later == out))
            out = M::Mix(out, later, f);
        return true;
    }
};

struct Level {
    Track<Vec3View, GfVec3d, MixLinear> translations, scales;
    Track<QuatView, GfQuatd, MixSlerp> rotations;
    Track<MatrixView, GfMatrix4d, MixMatrix> transforms;
    GfMatrix4d instancerXf;

    // instanceTransforms[i] * S(i) * R(i) * T(i) * instancerTransform, row
    // vectors. Matches UsdGeomPointInstancer::ComputeInstanceTransformsAtTime:
    // the quaternion goes through GfMatrix4d::SetRotate(GfQuatd), which does
    // NOT normalize (USD relies on unit quaternions, see the USD commit
    // "Don't normalize point instancer quaternions", 2021).
    GfMatrix4d transform(int i) const {
        GfMatrix4d srt(1.0);
        GfQuatd q;
        if (rotations.get(i, q))
            srt.SetRotate(q);
        GfVec3d v;
        if (scales.get(i, v)) // S * R: row k of R scaled by s[k]
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    srt[r][c] *= v[r];
        if (translations.get(i, v))
            srt.SetTranslateOnly(v);
        GfMatrix4d m;
        return transforms.get(i, m) ? m * srt * instancerXf : srt * instancerXf;
    }
};

// The instancer level at `time` from its primvar samples (nested type
// HdNovaInstancer::Samples, deduced).
template <class S>
Level levelAt(S const &translations, S const &scales, S const &rotations, S const &transforms,
              GfMatrix4d const &instancerXf, float time) {
    return Level{{translations.times, translations.values, time},
                 {scales.times, scales.values, time},
                 {rotations.times, rotations.values, time},
                 {transforms.times, transforms.values, time},
                 instancerXf};
}

} // namespace

HdNovaInstancer::HdNovaInstancer(HdSceneDelegate *delegate, SdfPath const &id) : HdInstancer(delegate, id) {}

HdNovaInstancer *HdNovaInstancer::_Parent() const {
    if (GetParentId().IsEmpty())
        return nullptr;
    return static_cast<HdNovaInstancer *>(GetDelegate()->GetRenderIndex().GetInstancer(GetParentId()));
}

void HdNovaInstancer::Sync(HdSceneDelegate *delegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) {
    // Parent instancer id and the instancer -> instancer dependency, so that
    // edits of a parent reach this instancer's rprims as DirtyInstancer.
    _UpdateInstancer(delegate, dirtyBits);

    std::lock_guard<std::mutex> lock(_mutex);
    const HdNovaShutter shutter =
        renderParam ? static_cast<HdNovaRenderParam *>(renderParam)->GetShutter() : HdNovaShutter{};
    const bool resample = !(shutter == _shutter) || _xf.values.empty();
    _shutter = shutter;

    if (*dirtyBits & HdChangeTracker::DirtyVisibility)
        _visible = delegate->GetVisible(GetId());
    if ((*dirtyBits & HdChangeTracker::DirtyTransform) || resample)
        _SampleTransform(delegate);
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId()) || resample)
        _SamplePrimvars(delegate);
    // DirtyInstanceIndex needs no cache: indices are fetched when computing.
    // _SyncInstancerAndParents() marks the instancer clean afterwards.
}

void HdNovaInstancer::SyncShutter(HdNovaShutter const &shutter) {
    for (HdNovaInstancer *inst = this; inst; inst = inst->_Parent()) {
        std::lock_guard<std::mutex> lock(inst->_mutex);
        if (!(inst->_shutter == shutter) || inst->_xf.values.empty()) {
            inst->_shutter = shutter;
            inst->_SampleTransform(inst->GetDelegate());
            inst->_SamplePrimvars(inst->GetDelegate());
        }
    }
}

void HdNovaInstancer::_SampleTransform(HdSceneDelegate *delegate) {
    HdNovaSampleTransformSamples(delegate, GetId(), _shutter, &_xf, /*instancer=*/true);
}

void HdNovaInstancer::_SamplePrimvars(HdSceneDelegate *delegate) {
    SdfPath const &id = GetId();
    // Rebuilt from the current descriptors, so a removed primvar (e.g. a
    // deleted "scales" attribute) does not linger (hdEmbree keeps it).
    _translations = _rotations = _scales = _transforms = Samples();
    _shadingPrimvars.clear();
    for (HdPrimvarDescriptor const &pv : delegate->GetPrimvarDescriptors(id, HdInterpolationInstance)) {
        if (!HdNovaIsGeometryPrimvar(pv.name)) {
            _shadingPrimvars.emplace_back(pv.name, delegate->Get(id, pv.name)); // shading only: no motion samples
            continue;
        }
        Samples *target = pv.name == HdInstancerTokens->instanceTranslations ? &_translations
                          : pv.name == HdInstancerTokens->instanceRotations  ? &_rotations
                          : pv.name == HdInstancerTokens->instanceScales     ? &_scales
                          : pv.name == HdInstancerTokens->instanceTransforms ? &_transforms
                                                                             : nullptr;
        if (!target)
            continue;
        if (_shutter.IsBlurred()) {
            HdTimeSampleArray<VtValue, 4> s;
            delegate->SamplePrimvar(id, pv.name, _shutter.open, _shutter.close, &s);
            target->times.assign(s.times.begin(), s.times.begin() + s.count);
            target->values.assign(s.values.begin(), s.values.begin() + s.count);
            // Particles born or dying within the shutter change the array
            // length and shift the indices, so samples cannot be matched per
            // instance: the primvar holds its value at the frame, which fits
            // the frame's instance indices (USD holds it too; Karma as well
            // when the frame lies inside the shutter, otherwise it pairs the
            // shutter samples by index).
            if (target->values.size() > 1) {
                const VtValue frame = delegate->Get(id, pv.name);
                const size_t n = frame.GetArraySize();
                if (std::any_of(target->values.begin(), target->values.end(),
                                [n](VtValue const &v) { return v.GetArraySize() != n; })) {
                    target->times.assign(1, 0.0f);
                    target->values.assign(1, frame);
                }
            }
        }
        if (target->empty()) {
            target->times.assign(1, 0.0f);
            target->values.assign(1, delegate->Get(id, pv.name));
        }
    }
}

unsigned HdNovaInstancer::GetMotionStepCount(unsigned maxSteps) const {
    if (!_shutter.IsBlurred())
        return 1;
    unsigned steps = _xf.StepCount(_shutter, maxSteps);
    for (Samples const *s : {&_translations, &_rotations, &_scales, &_transforms})
        steps = std::max(steps, HdNovaMotionStepCount(s->times.data(), s->times.size(), _shutter, maxSteps));
    if (HdNovaInstancer const *parent = _Parent())
        steps = std::max(steps, parent->GetMotionStepCount(maxSteps));
    return steps;
}

void HdNovaInstancer::ComputeInstanceTransformsAt(SdfPath const &prototypeId, GfMatrix4d const &prototypeXf,
                                                  float time, std::vector<GfMatrix4d> &out) const {
    _ComputeAt(prototypeId, time, out);
    WorkParallelForN(out.size(), [&](size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k)
            out[k] = prototypeXf * out[k];
    });
}

void HdNovaInstancer::_ComputeAt(SdfPath const &prototypeId, float time, std::vector<GfMatrix4d> &out) const {
    out.clear();
    if (!_visible)
        return;
    VtIntArray const indices = GetDelegate()->GetInstanceIndices(GetId(), prototypeId);
    if (indices.empty())
        return;

    std::vector<GfMatrix4d> parent; // flattened outer levels
    if (!GetParentId().IsEmpty()) {
        HdNovaInstancer const *p = _Parent();
        if (!TF_VERIFY(p))
            return;
        p->_ComputeAt(GetId(), time, parent);
        if (parent.empty())
            return; // the parent level has no (visible) instances
    }

    // Interpolated per instance (inside the parallel loop), only for this
    // prototype's instances.
    Level const level = levelAt(_translations, _scales, _rotations, _transforms, _xf.At(time), time);

    size_t const n = indices.size();
    size_t const np = parent.empty() ? 1 : parent.size();
    out.resize(n * np);
    WorkParallelForN(n, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) {
            GfMatrix4d const local = level.transform(indices[c]);
            if (parent.empty())
                out[c] = local;
            else
                for (size_t p = 0; p < np; ++p)
                    out[p * n + c] = local * parent[p]; // inner level first, then the outer one
        }
    });
}

size_t HdNovaInstancer::_FlattenedCount(SdfPath const &prototypeId) const {
    if (!_visible)
        return 0;
    const size_t n = GetDelegate()->GetInstanceIndices(GetId(), prototypeId).size();
    if (GetParentId().IsEmpty())
        return n;
    HdNovaInstancer const *p = _Parent();
    return p ? n * p->_FlattenedCount(GetId()) : 0;
}

void HdNovaInstancer::ComputeInstanceAttributes(SdfPath const &prototypeId,
                                                std::vector<nova::InstanceAttribute> &out) const {
    out.clear();
    if (!_visible)
        return;
    VtIntArray const indices = GetDelegate()->GetInstanceIndices(GetId(), prototypeId);
    if (indices.empty())
        return;

    // The outer levels: their attributes (one value per parent instance).
    std::vector<nova::InstanceAttribute> parent;
    size_t np = 1;
    if (!GetParentId().IsEmpty()) {
        HdNovaInstancer const *p = _Parent();
        if (!TF_VERIFY(p))
            return;
        p->ComputeInstanceAttributes(GetId(), parent);
        np = p->_FlattenedCount(GetId());
        if (np == 0)
            return;
    }
    size_t const n = indices.size();
    auto findParent = [&](std::string const &name) -> nova::InstanceAttribute const * {
        for (nova::InstanceAttribute const &a : parent)
            if (a.name == name && a.data.size() == np * a.width)
                return &a;
        return nullptr;
    };

    for (auto const &[name, value] : _shadingPrimvars) {
        int width = 0;
        std::vector<float> data;
        if (!HdNovaPrimvarToFloats(value, &width, &data) || data.empty())
            continue;
        size_t const w = size_t(width), count = data.size() / w;
        nova::InstanceAttribute a;
        a.name = name.GetString();
        a.width = uint8_t(width);
        a.data.assign(n * np * w, 0.0f);
        nova::InstanceAttribute const *outer = findParent(a.name);
        if (outer && outer->width != a.width)
            outer = nullptr;
        for (size_t p = 0; p < np; ++p)
            for (size_t c = 0; c < n; ++c) {
                float const *src = indices[c] >= 0 && size_t(indices[c]) < count ? &data[size_t(indices[c]) * w]
                                   : outer                                     ? &outer->data[p * w]
                                                                               : nullptr;
                if (src)
                    std::copy_n(src, w, &a.data[(p * n + c) * w]);
            }
        out.push_back(std::move(a));
    }
    // Names only outer levels define: their value for every inner instance.
    for (nova::InstanceAttribute const &pa : parent) {
        if (pa.data.size() != np * pa.width ||
            std::any_of(out.begin(), out.end(), [&](nova::InstanceAttribute const &a) { return a.name == pa.name; }))
            continue;
        nova::InstanceAttribute a;
        a.name = pa.name;
        a.width = pa.width;
        a.data.resize(n * np * pa.width);
        for (size_t p = 0; p < np; ++p)
            for (size_t c = 0; c < n; ++c)
                std::copy_n(&pa.data[p * pa.width], pa.width, &a.data[(p * n + c) * pa.width]);
        out.push_back(std::move(a));
    }
}

void HdNovaInstancer::ComputeInstanceTransforms(SdfPath const &prototypeId, GfMatrix4d const &prototypeXf,
                                                std::vector<nova::Xform34> &out) const {
    out.clear();
    if (!_visible)
        return;
    const float time = _shutter.FrameOffset();
    const bool moving = _xf.values.size() > 1 || _translations.values.size() > 1 || _rotations.values.size() > 1 ||
                        _scales.values.size() > 1 || _transforms.values.size() > 1;
    if (moving) { // only if the motion steps were unusable: exact, in double precision
        std::vector<GfMatrix4d> exact;
        ComputeInstanceTransformsAt(prototypeId, prototypeXf, time, exact);
        out.resize(exact.size());
        for (size_t k = 0; k < exact.size(); ++k)
            out[k] = toXform34(exact[k]);
        return;
    }

    VtIntArray const indices = GetDelegate()->GetInstanceIndices(GetId(), prototypeId);
    if (indices.empty())
        return;
    std::vector<GfMatrix4d> parent; // flattened outer levels, usually small
    if (!GetParentId().IsEmpty()) {
        HdNovaInstancer const *p = _Parent();
        if (!TF_VERIFY(p))
            return;
        p->_ComputeAt(GetId(), time, parent);
        if (parent.empty())
            return;
    }

    // Single samples: the common static case with millions of instances, so
    // no double-precision temporaries.
    Level const level = levelAt(_translations, _scales, _rotations, _transforms, _xf.At(time), time);
    size_t const n = indices.size();
    size_t const np = parent.empty() ? 1 : parent.size();
    out.resize(n * np);
    // Parallel over the innermost level. Each child transform is computed
    // once and written directly as floats.
    WorkParallelForN(n, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) {
            GfMatrix4d const local = prototypeXf * level.transform(indices[c]);
            if (parent.empty())
                out[c] = toXform34(local);
            else
                for (size_t p = 0; p < np; ++p)
                    out[p * n + c] = toXform34(local * parent[p]);
        }
    });
}

PXR_NAMESPACE_CLOSE_SCOPE
