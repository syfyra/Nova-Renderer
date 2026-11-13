#include "velocitySceneIndex.h"

#include "diagnostics.h"

#include <pxr/base/gf/quatf.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneGlobalsSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    ((novaVelocityBlur, "nova:object:velocityBlur"))
    ((novaInstanceVelocityBlur, "nova:object:instanceVelocityBlur"))
    ((novaAccelerationSamples, "nova:object:accelerationSamples"))
    ((karmaVelocityBlur, "karma:object:vblur"))
    ((karmaInstanceVelocityBlur, "karma:object:instance_vblur"))
    ((karmaGeoSamples, "karma:object:geosamples"))
    ((karmaInstanceSamples, "karma:object:instance_samples"))
    (angularVelocities)
);

int HdNovaVelocityBlurMode(VtValue const &value) {
    if (value.IsHolding<TfToken>() || value.IsHolding<std::string>()) {
        // A number as text, or Karma's menu labels as its render settings
        // author them.
        const std::string text = TfStringToLower(TfStringTrim(
            value.IsHolding<TfToken>() ? value.UncheckedGet<TfToken>().GetString() : value.UncheckedGet<std::string>()));
        if (text.find("accel") != std::string::npos)
            return 2;
        if (text.rfind("no", 0) == 0 || text == "off")
            return 0;
        if (text.find("veloc") != std::string::npos)
            return 1;
        char *end = nullptr;
        const long n = std::strtol(text.c_str(), &end, 10);
        return !text.empty() && end != text.c_str() ? int(std::clamp(n, 0l, 2l)) : -1;
    }
    const VtValue d = VtValue::Cast<double>(value);
    if (!d.IsHolding<double>() || !std::isfinite(d.UncheckedGet<double>()))
        return -1;
    return int(std::clamp(std::lround(d.UncheckedGet<double>()), 0l, 2l));
}

// Shared by the index and the data sources it hands out (which Hydra may
// keep beyond a GetPrim call): the render settings' defaults of the render
// index, and whether anything has resolved with them since they were set
// (if not, a change needs no dirtying: nothing was computed from them).
struct HdNovaVelocityModeSceneIndex::State {
    mutable std::mutex mutex;
    HdNovaVelocityDefaults defaults;
    mutable std::atomic<bool> used{false};

    HdNovaVelocityDefaults Get() const {
        used.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mutex);
        return defaults;
    }
};

namespace {

using StatePtr = std::shared_ptr<const HdNovaVelocityModeSceneIndex::State>;

// The value of a primvar (empty if absent).
VtValue primvarValue(HdPrimvarsSchema const &primvars, TfToken const &name, HdSampledDataSource::Time t = 0.0f) {
    HdPrimvarSchema pv = primvars.GetPrimvar(name);
    if (!pv)
        return VtValue();
    HdSampledDataSourceHandle v = pv.GetPrimvarValue();
    return v ? v->GetValue(t) : VtValue();
}

int sampleCount(VtValue const &value) {
    const VtValue d = VtValue::Cast<double>(value);
    if (!d.IsHolding<double>() || !std::isfinite(d.UncheckedGet<double>()))
        return -1;
    return int(std::clamp(std::lround(d.UncheckedGet<double>()), 1l, 64l));
}

// A prim's own velocity blur mode (0 none, 1 velocity, 2 acceleration) and
// acceleration samples from its primvars (with those inherited from
// ancestors); -1 where it sets nothing.
int ownMode(HdPrimvarsSchema const &primvars, bool instancer) {
    const int mode = HdNovaVelocityBlurMode(
        primvarValue(primvars, instancer ? _tokens->novaInstanceVelocityBlur : _tokens->novaVelocityBlur));
    return mode >= 0 ? mode
                     : HdNovaVelocityBlurMode(primvarValue(
                           primvars, instancer ? _tokens->karmaInstanceVelocityBlur : _tokens->karmaVelocityBlur));
}

int ownSamples(HdPrimvarsSchema const &primvars, bool instancer) {
    const int samples = sampleCount(primvarValue(primvars, _tokens->novaAccelerationSamples));
    return samples >= 0 ? samples
                        : sampleCount(primvarValue(
                              primvars, instancer ? _tokens->karmaInstanceSamples : _tokens->karmaGeoSamples));
}

int modeWith(int own, bool instancer, HdNovaVelocityDefaults const &d) {
    return own >= 0 ? own : instancer ? d.instanceVelocityBlur : d.velocityBlur;
}

int samplesWith(int own, bool instancer, HdNovaVelocityDefaults const &d) {
    return own >= 0 ? own : instancer ? d.instanceSamples : d.geoSamples;
}

int resolveMode(HdPrimvarsSchema const &primvars, bool instancer, HdNovaVelocityModeSceneIndex::State const &state) {
    const int own = ownMode(primvars, instancer);
    return own >= 0 ? own : modeWith(own, instancer, state.Get());
}

int resolveSamples(HdPrimvarsSchema const &primvars, bool instancer, HdNovaVelocityModeSceneIndex::State const &state) {
    const int own = ownSamples(primvars, instancer);
    return own >= 0 ? own : samplesWith(own, instancer, state.Get());
}

// Whether a prim carries motion that the velocity indices resolve.
bool hasVelocityData(HdPrimvarsSchema const &primvars, bool instancer) {
    return primvars.GetPrimvar(HdTokens->velocities) || primvars.GetPrimvar(HdTokens->accelerations) ||
           (instancer && primvars.GetPrimvar(_tokens->angularVelocities));
}

// __velocityMotionMode, evaluated when read: settings may change while
// Hydra holds the prim's data sources.
class ModeDataSource final : public HdTypedSampledDataSource<TfToken> {
public:
    HD_DECLARE_DATASOURCE(ModeDataSource);

    VtValue GetValue(Time t) override { return VtValue(GetTypedValue(t)); }
    TfToken GetTypedValue(Time) override {
        switch (resolveMode(HdPrimvarsSchema::GetFromParent(_prim), _instancer, *_state)) {
        case 1:
            return HdsiVelocityMotionResolvingSceneIndexTokens->noAcceleration;
        case 2:
            return HdsiVelocityMotionResolvingSceneIndexTokens->enable;
        default: // time samples only, as if there were no velocities
            return HdsiVelocityMotionResolvingSceneIndexTokens->ignore;
        }
    }
    bool GetContributingSampleTimesForInterval(Time, Time, std::vector<Time> *) override { return false; }

private:
    ModeDataSource(HdContainerDataSourceHandle prim, bool instancer, StatePtr state)
        : _prim(std::move(prim)), _instancer(instancer), _state(std::move(state)) {}
    HdContainerDataSourceHandle _prim;
    bool _instancer;
    StatePtr _state;
};

// The shutter samples of acceleration blur (the resolving index's
// nonlinearSampleCount).
class SamplesDataSource final : public HdTypedSampledDataSource<int> {
public:
    HD_DECLARE_DATASOURCE(SamplesDataSource);

    VtValue GetValue(Time t) override { return VtValue(GetTypedValue(t)); }
    int GetTypedValue(Time) override {
        return resolveSamples(HdPrimvarsSchema::GetFromParent(_prim), _instancer, *_state);
    }
    bool GetContributingSampleTimesForInterval(Time, Time, std::vector<Time> *) override { return false; }

private:
    SamplesDataSource(HdContainerDataSourceHandle prim, bool instancer, StatePtr state)
        : _prim(std::move(prim)), _instancer(instancer), _state(std::move(state)) {}
    HdContainerDataSourceHandle _prim;
    bool _instancer;
    StatePtr _state;
};

// Identity orientations, one per instance position: the resolving index only
// turns orientations that exist, Karma turns instances without them as well.
// Single precision (pxr keeps the type; half precision would quantize the
// turned orientations to about 3e-4 radians).
class IdentityRotationsDataSource final : public HdTypedSampledDataSource<VtQuatfArray> {
public:
    HD_DECLARE_DATASOURCE(IdentityRotationsDataSource);

    VtValue GetValue(Time t) override { return VtValue(GetTypedValue(t)); }
    VtQuatfArray GetTypedValue(Time t) override {
        const size_t n = _positions ? _positions->GetValue(t).GetArraySize() : 0;
        return VtQuatfArray(n, GfQuatf::GetIdentity());
    }
    bool GetContributingSampleTimesForInterval(Time start, Time end, std::vector<Time> *times) override {
        // The count follows the positions.
        return _positions && _positions->GetContributingSampleTimesForInterval(start, end, times);
    }

private:
    explicit IdentityRotationsDataSource(HdSampledDataSourceHandle positions) : _positions(std::move(positions)) {}
    HdSampledDataSourceHandle _positions;
};

// What a prim's primvars gain: nonlinearSampleCount while it blurs by
// acceleration (a user's UsdGeomMotionAPI count is replaced: Karma ignores
// it), and identity orientations for a point instancer that blurs by
// angular velocities without orientations. Overlaid on the input's.
class PrimvarsDataSource final : public HdContainerDataSource {
public:
    HD_DECLARE_DATASOURCE(PrimvarsDataSource);

    TfTokenVector GetNames() override {
        TfTokenVector names;
        const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(_prim);
        const int mode = resolveMode(primvars, _instancer, *_state);
        if (mode == 0 || !hasVelocityData(primvars, _instancer))
            return names; // nothing to resolve: the input's primvars as they are
        if (mode == 2)
            names.push_back(HdTokens->nonlinearSampleCount);
        if (_NeedsRotations(primvars))
            names.push_back(HdInstancerTokens->instanceRotations);
        return names;
    }
    HdDataSourceBaseHandle Get(TfToken const &name) override {
        if (name == HdTokens->nonlinearSampleCount) {
            const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(_prim);
            if (resolveMode(primvars, _instancer, *_state) != 2 || !hasVelocityData(primvars, _instancer))
                return nullptr;
            return HdPrimvarSchema::Builder()
                .SetPrimvarValue(SamplesDataSource::New(_prim, _instancer, _state))
                .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant))
                .Build();
        }
        if (name == HdInstancerTokens->instanceRotations && _instancer) {
            const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(_prim);
            if (resolveMode(primvars, _instancer, *_state) < 1 || !_NeedsRotations(primvars))
                return nullptr;
            return HdPrimvarSchema::Builder()
                .SetPrimvarValue(IdentityRotationsDataSource::New(
                    primvars.GetPrimvar(HdInstancerTokens->instanceTranslations).GetPrimvarValue()))
                .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->instance))
                .Build();
        }
        return nullptr;
    }

private:
    PrimvarsDataSource(HdContainerDataSourceHandle prim, bool instancer, StatePtr state)
        : _prim(std::move(prim)), _instancer(instancer), _state(std::move(state)) {}
    bool _NeedsRotations(HdPrimvarsSchema const &primvars) const {
        return _instancer && primvars.GetPrimvar(_tokens->angularVelocities) &&
               !primvars.GetPrimvar(HdInstancerTokens->instanceRotations);
    }
    HdContainerDataSourceHandle _prim;
    bool _instancer;
    StatePtr _state;
};

// Downstream of the resolving index: where Karma samples velocity motion
// twice (velocity blur, and acceleration blur at Karma's default of 2), a
// resolved primvar keeps only shutter open and close. pxr samples rotations
// (angular velocities) and accelerations at least three times.
//
// Only motion pxr made from velocity data is trimmed, per primvar: points
// and instance positions need velocities, orientations angular velocities,
// each with one value per element (pxr's validity rule), and pxr's times
// must differ from the input's.
//
// Where velocity blur is on and that data is not usable (no velocities, an
// empty array, another count than the elements), pxr passes USD's time
// samples through. Karma does not blur by them (measured, audit M-1 of
// 0.2.0): velocity blur on means the time samples of points, instance
// positions and orientations are not motion, and the value at the frame is
// held over the shutter. So here: the subframe samples of Houdini's Motion
// Blur LOP under a Velocity Blur setting render sharp, as in Karma, and so
// does deforming geometry without v under a global Velocity Blur.
bool nearTime(float a, float b, float length) { return std::fabs(a - b) <= 1e-4f * std::max(1.0f, length); }

// Whether the prim blurs by velocities (mode 1 or 2).
bool velocityBlurOn(HdContainerDataSourceHandle const &prim) {
    auto modeDs = HdTypedSampledDataSource<TfToken>::Cast(
        prim->Get(HdsiVelocityMotionResolvingSceneIndexTokens->velocityMotionMode));
    if (!modeDs)
        return false;
    const TfToken mode = modeDs->GetTypedValue(0.0f);
    return mode == HdsiVelocityMotionResolvingSceneIndexTokens->noAcceleration ||
           mode == HdsiVelocityMotionResolvingSceneIndexTokens->enable;
}

bool sampledTwice(HdContainerDataSourceHandle const &prim) {
    auto modeDs = HdTypedSampledDataSource<TfToken>::Cast(
        prim->Get(HdsiVelocityMotionResolvingSceneIndexTokens->velocityMotionMode));
    if (!modeDs)
        return false;
    const TfToken mode = modeDs->GetTypedValue(0.0f);
    return mode == HdsiVelocityMotionResolvingSceneIndexTokens->noAcceleration ||
           (mode == HdsiVelocityMotionResolvingSceneIndexTokens->enable &&
            sampleCount(primvarValue(HdPrimvarsSchema::GetFromParent(prim), HdTokens->nonlinearSampleCount)) <= 2);
}

class EndpointsDataSource final : public HdSampledDataSource {
public:
    HD_DECLARE_DATASOURCE(EndpointsDataSource);

    VtValue GetValue(Time t) override { return _input->GetValue(_Held() ? 0.0f : t); }
    bool GetContributingSampleTimesForInterval(Time start, Time end, std::vector<Time> *times) override {
        if (_Held()) // the frame's value over the whole shutter
            return false;
        if (!_input->GetContributingSampleTimesForInterval(start, end, times))
            return false;
        const float length = end - start;
        if (times->size() > 2 && nearTime(times->front(), start, length) && nearTime(times->back(), end, length) &&
            sampledTwice(_prim) && _Resolved(start, end, *times))
            *times = {start, end};
        return true;
    }

private:
    EndpointsDataSource(HdSampledDataSourceHandle input, HdContainerDataSourceHandle prim, TfToken name,
                        HdSceneIndexBaseRefPtr upstream, SdfPath path)
        : _input(std::move(input)), _prim(std::move(prim)), _name(std::move(name)), _upstream(std::move(upstream)),
          _path(std::move(path)) {}
    // The primvar's velocity data is usable: one velocity (angular velocity
    // for orientations) per element.
    bool _Valid() const {
        const TfToken &source =
            _name == HdInstancerTokens->instanceRotations ? _tokens->angularVelocities : HdTokens->velocities;
        const size_t n = primvarValue(HdPrimvarsSchema::GetFromParent(_prim), source).GetArraySize();
        return n != 0 && n == _input->GetValue(0.0f).GetArraySize();
    }
    // Karma's rule: velocity blur on and no usable velocity data, so the
    // primvar's time samples are not motion. Decided once per data source
    // (a change of the prim's data makes new ones).
    bool _Held() const {
        int held = _held.load(std::memory_order_relaxed);
        if (held < 0) {
            held = velocityBlurOn(_prim) && !_Valid() ? 1 : 0;
            _held.store(held, std::memory_order_relaxed);
        }
        return held == 1;
    }
    // pxr resolved this primvar from valid velocity data: one velocity per
    // element, and times other than those of the resolving index's input.
    bool _Resolved(Time start, Time end, std::vector<Time> const &times) const {
        if (!_Valid())
            return false;
        HdSampledDataSourceHandle before =
            HdPrimvarsSchema::GetFromParent(_upstream->GetPrim(_path).dataSource).GetPrimvar(_name).GetPrimvarValue();
        std::vector<Time> inputTimes;
        if (!before || !before->GetContributingSampleTimesForInterval(start, end, &inputTimes))
            return true;
        return inputTimes != times;
    }
    HdSampledDataSourceHandle _input;
    HdContainerDataSourceHandle _prim;
    TfToken _name;
    HdSceneIndexBaseRefPtr _upstream; // the resolving index's input
    SdfPath _path;
    mutable std::atomic<int> _held{-1}; // -1: not decided yet
};

class EndpointPrimvarsDataSource final : public HdContainerDataSource {
public:
    HD_DECLARE_DATASOURCE(EndpointPrimvarsDataSource);

    // Overlaid on the input's primvars, which name everything.
    TfTokenVector GetNames() override { return {}; }
    HdDataSourceBaseHandle Get(TfToken const &name) override {
        if (name != HdTokens->points && name != HdInstancerTokens->instanceTranslations &&
            name != HdInstancerTokens->instanceRotations)
            return nullptr;
        HdPrimvarSchema pv = HdPrimvarsSchema::GetFromParent(_prim).GetPrimvar(name);
        HdSampledDataSourceHandle value = pv.GetPrimvarValue();
        if (!value)
            return nullptr;
        return HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(HdPrimvarSchemaTokens->primvarValue,
                                               EndpointsDataSource::New(value, _prim, name, _upstream, _path)),
            pv.GetContainer());
    }

private:
    EndpointPrimvarsDataSource(HdContainerDataSourceHandle prim, HdSceneIndexBaseRefPtr upstream, SdfPath path)
        : _prim(std::move(prim)), _upstream(std::move(upstream)), _path(std::move(path)) {}
    HdContainerDataSourceHandle _prim;
    HdSceneIndexBaseRefPtr _upstream;
    SdfPath _path;
};

TF_DECLARE_REF_PTRS(EndpointsSceneIndex);

// `input` is pxr's resolving index, `upstream` its input.
class EndpointsSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static EndpointsSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input, HdSceneIndexBaseRefPtr const &upstream) {
        return TfCreateRefPtr(new EndpointsSceneIndex(input, upstream));
    }
    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
        if (prim.dataSource && HdsiVelocityMotionResolvingSceneIndex::PrimTypeSupportsVelocityMotion(prim.primType))
            prim.dataSource = HdOverlayContainerDataSource::New(
                HdRetainedContainerDataSource::New(
                    HdPrimvarsSchema::GetSchemaToken(),
                    EndpointPrimvarsDataSource::New(prim.dataSource, _upstream, primPath)),
                prim.dataSource);
        return prim;
    }
    SdfPathVector GetChildPrimPaths(SdfPath const &primPath) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
    }

protected:
    EndpointsSceneIndex(HdSceneIndexBaseRefPtr const &input, HdSceneIndexBaseRefPtr const &upstream)
        : HdSingleInputFilteringSceneIndexBase(input), _upstream(upstream) {}
    HdSceneIndexBaseRefPtr _upstream;
    void _PrimsAdded(HdSceneIndexBase const &, HdSceneIndexObserver::AddedPrimEntries const &entries) override {
        _SendPrimsAdded(entries);
    }
    void _PrimsRemoved(HdSceneIndexBase const &, HdSceneIndexObserver::RemovedPrimEntries const &entries) override {
        _SendPrimsRemoved(entries);
    }
    void _PrimsDirtied(HdSceneIndexBase const &, HdSceneIndexObserver::DirtiedPrimEntries const &entries) override {
        _SendPrimsDirtied(entries);
    }
};

// The scene globals' timeCodesPerSecond from the render settings.
class TimeCodesDataSource final : public HdTypedSampledDataSource<double> {
public:
    HD_DECLARE_DATASOURCE(TimeCodesDataSource);

    VtValue GetValue(Time t) override { return VtValue(GetTypedValue(t)); }
    double GetTypedValue(Time) override { return _state->Get().timeCodesPerSecond; }
    bool GetContributingSampleTimesForInterval(Time, Time, std::vector<Time> *) override { return false; }

private:
    explicit TimeCodesDataSource(StatePtr state) : _state(std::move(state)) {}
    StatePtr _state;
};

// The setting primvars: an edit of one re-resolves the prim's motion.
HdDataSourceLocatorSet const &settingLocators() {
    static const HdDataSourceLocatorSet locators = [] {
        HdDataSourceLocatorSet s;
        for (TfToken const &name : {_tokens->novaVelocityBlur, _tokens->novaInstanceVelocityBlur,
                                    _tokens->novaAccelerationSamples, _tokens->karmaVelocityBlur,
                                    _tokens->karmaInstanceVelocityBlur, _tokens->karmaGeoSamples,
                                    _tokens->karmaInstanceSamples})
            s.insert(HdPrimvarsSchema::GetDefaultLocator().Append(name));
        return s;
    }();
    return locators;
}

// What a new resolution changes: the parameter, the sample count and the
// resolved primvars.
HdDataSourceLocatorSet const &resolvedLocators() {
    static const HdDataSourceLocatorSet locators = [] {
        HdDataSourceLocatorSet s;
        s.insert(HdDataSourceLocator(HdsiVelocityMotionResolvingSceneIndexTokens->velocityMotionMode));
        for (TfToken const &name : {HdTokens->nonlinearSampleCount, HdTokens->points,
                                    HdInstancerTokens->instanceTranslations, HdInstancerTokens->instanceRotations,
                                    HdInstancerTokens->instanceScales})
            s.insert(HdPrimvarsSchema::GetDefaultLocator().Append(name));
        return s;
    }();
    return locators;
}

} // namespace

HdNovaVelocityModeSceneIndexRefPtr HdNovaVelocityModeSceneIndex::New(HdSceneIndexBaseRefPtr const &input) {
    return TfCreateRefPtr(new HdNovaVelocityModeSceneIndex(input));
}

HdNovaVelocityModeSceneIndex::HdNovaVelocityModeSceneIndex(HdSceneIndexBaseRefPtr const &input)
    : HdSingleInputFilteringSceneIndexBase(input), _state(std::make_shared<State>()) {}

HdNovaVelocityModeSceneIndex::~HdNovaVelocityModeSceneIndex() = default;

HdNovaVelocityModeSceneIndexRefPtr HdNovaVelocityModeSceneIndex::Find(HdRenderIndex *index) {
    if (!index)
        return {};
    // The render index's chain: its terminal scene index and, through the
    // filtering indices (the merging index included), everything above.
    std::vector<HdSceneIndexBaseRefPtr> todo{index->GetTerminalSceneIndex()};
    std::unordered_set<const HdSceneIndexBase *> seen;
    while (!todo.empty()) {
        HdSceneIndexBaseRefPtr scene = todo.back();
        todo.pop_back();
        if (!scene || !seen.insert(get_pointer(scene)).second)
            continue;
        if (HdNovaVelocityModeSceneIndexRefPtr found = TfDynamic_cast<HdNovaVelocityModeSceneIndexRefPtr>(scene))
            return found;
        if (HdFilteringSceneIndexBaseRefPtr filtering = TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>(scene))
            for (HdSceneIndexBaseRefPtr const &input : filtering->GetInputScenes())
                todo.push_back(input);
    }
    return {};
}

HdNovaVelocityDefaults HdNovaVelocityModeSceneIndex::GetDefaults() const { return _state->Get(); }

HdSceneIndexPrim HdNovaVelocityModeSceneIndex::GetPrim(SdfPath const &primPath) const {
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    if (primPath == HdSceneGlobalsSchema::GetDefaultPrimPath()) {
        // The frame rate the velocities convert with. Houdini's scene
        // globals say 24 whatever the stage's timeCodesPerSecond (husk,
        // measured); houdini:fps follows the stage.
        prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(
            HdRetainedContainerDataSource::New(
                HdSceneGlobalsSchema::GetSchemaToken(),
                HdRetainedContainerDataSource::New(HdSceneGlobalsSchemaTokens->timeCodesPerSecond,
                                                   TimeCodesDataSource::New(_state))),
            prim.dataSource);
        return prim;
    }
    if (!prim.dataSource || !HdsiVelocityMotionResolvingSceneIndex::PrimTypeSupportsVelocityMotion(prim.primType))
        return prim;
    const bool instancer = prim.primType == HdPrimTypeTokens->instancer;
    HdContainerDataSourceHandle input = prim.dataSource;
    prim.dataSource = HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(HdsiVelocityMotionResolvingSceneIndexTokens->velocityMotionMode,
                                           ModeDataSource::New(input, instancer, _state),
                                           HdPrimvarsSchema::GetSchemaToken(),
                                           PrimvarsDataSource::New(input, instancer, _state)),
        input);
    return prim;
}

SdfPathVector HdNovaVelocityModeSceneIndex::GetChildPrimPaths(SdfPath const &primPath) const {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdNovaVelocityModeSceneIndex::_PrimsAdded(HdSceneIndexBase const &,
                                               HdSceneIndexObserver::AddedPrimEntries const &entries) {
    _SendPrimsAdded(entries);
}

void HdNovaVelocityModeSceneIndex::_PrimsRemoved(HdSceneIndexBase const &,
                                                 HdSceneIndexObserver::RemovedPrimEntries const &entries) {
    _SendPrimsRemoved(entries);
}

void HdNovaVelocityModeSceneIndex::_PrimsDirtied(HdSceneIndexBase const &,
                                                 HdSceneIndexObserver::DirtiedPrimEntries const &entries) {
    // A settings edit re-resolves the prim: the resolving index downstream
    // then delivers new points (or instance positions and orientations).
    std::vector<size_t> touched;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].dirtyLocators.Intersects(settingLocators()))
            touched.push_back(i);
    if (touched.empty()) {
        _SendPrimsDirtied(entries);
        return;
    }
    HdSceneIndexObserver::DirtiedPrimEntries out(entries);
    for (size_t i : touched)
        out[i].dirtyLocators.insert(resolvedLocators());
    _SendPrimsDirtied(out);
}

bool HdNovaVelocityModeSceneIndex::SetDefaults(HdNovaVelocityDefaults const &defaults) {
    HdNovaVelocityDefaults old;
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        if (_state->defaults == defaults)
            return false;
        old = _state->defaults;
        _state->defaults = defaults;
    }
    TF_DEBUG(HDNOVA_MOTION)
        .Msg("Nova: velocity blur defaults vblur %d instance_vblur %d geosamples %d instance_samples %d, "
             "%g time codes per second\n",
             defaults.velocityBlur, defaults.instanceVelocityBlur, defaults.geoSamples, defaults.instanceSamples,
             defaults.timeCodesPerSecond);
    // Nothing resolved with the old defaults yet (before the first sync):
    // nothing to redo.
    if (!_state->used.load() || !_IsObserved())
        return false;

    // The prims whose motion the change affects: meshes, curves, points and
    // instancers with velocity data whose mode, samples (in acceleration
    // mode) or frame rate (when they blur) change; volumes whose velocity
    // blur switches or whose blur depends on the frame rate (D17).
    const bool timeCodes = old.timeCodesPerSecond != defaults.timeCodesPerSecond;
    HdDataSourceLocatorSet volumeLocators;
    volumeLocators.insert(HdPrimvarsSchema::GetDefaultLocator());
    HdSceneIndexObserver::DirtiedPrimEntries entries;
    SdfPathVector stack{SdfPath::AbsoluteRootPath()};
    while (!stack.empty()) {
        const SdfPath path = stack.back();
        stack.pop_back();
        const HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
        const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
        if (prim.primType == HdPrimTypeTokens->volume) {
            const int own = ownMode(primvars, false);
            const bool before = modeWith(own, false, old) >= 1, after = modeWith(own, false, defaults) >= 1;
            if (before != after || (after && timeCodes))
                entries.emplace_back(path, volumeLocators);
        } else if (HdsiVelocityMotionResolvingSceneIndex::PrimTypeSupportsVelocityMotion(prim.primType)) {
            const bool instancer = prim.primType == HdPrimTypeTokens->instancer;
            if (hasVelocityData(primvars, instancer)) {
                const int own = ownMode(primvars, instancer);
                const int before = modeWith(own, instancer, old), after = modeWith(own, instancer, defaults);
                bool dirty = before != after || (after >= 1 && timeCodes);
                if (!dirty && after == 2) {
                    const int samples = ownSamples(primvars, instancer);
                    dirty = samplesWith(samples, instancer, old) != samplesWith(samples, instancer, defaults);
                }
                if (dirty)
                    entries.emplace_back(path, resolvedLocators());
            }
        }
        for (SdfPath const &child : _GetInputSceneIndex()->GetChildPrimPaths(path))
            stack.push_back(child);
    }
    if (entries.empty())
        return false;
    _SendPrimsDirtied(entries);
    return true;
}

HdSceneIndexBaseRefPtr HdNovaVelocityModeSceneIndex::Append(HdSceneIndexBaseRefPtr const &input) {
    HdNovaVelocityModeSceneIndexRefPtr modes = New(input);
    return EndpointsSceneIndex::New(HdsiVelocityMotionResolvingSceneIndex::New(modes), modes);
}

PXR_NAMESPACE_CLOSE_SCOPE
