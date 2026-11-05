#include "velocitySceneIndex.h"

#include "diagnostics.h"

#include <pxr/base/gf/quath.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneGlobalsSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
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
// keep beyond a GetPrim call): the render settings' defaults.
struct HdNovaVelocityModeSceneIndex::State {
    mutable std::mutex mutex;
    HdNovaVelocityDefaults defaults;

    HdNovaVelocityDefaults Get() const {
        std::lock_guard<std::mutex> lock(mutex);
        return defaults;
    }
};

namespace {

using StatePtr = std::shared_ptr<const HdNovaVelocityModeSceneIndex::State>;

// The value of a primvar (empty if absent).
VtValue primvarValue(HdPrimvarsSchema const &primvars, TfToken const &name) {
    HdPrimvarSchema pv = primvars.GetPrimvar(name);
    if (!pv)
        return VtValue();
    HdSampledDataSourceHandle v = pv.GetPrimvarValue();
    return v ? v->GetValue(0.0f) : VtValue();
}

VtValue primvarValue(HdContainerDataSourceHandle const &prim, TfToken const &name) {
    return primvarValue(HdPrimvarsSchema::GetFromParent(prim), name);
}

int sampleCount(VtValue const &value) {
    const VtValue d = VtValue::Cast<double>(value);
    if (!d.IsHolding<double>() || !std::isfinite(d.UncheckedGet<double>()))
        return -1;
    return int(std::clamp(std::lround(d.UncheckedGet<double>()), 1l, 64l));
}

// A prim's velocity blur mode (0 none, 1 velocity, 2 acceleration) and the
// shutter samples of acceleration blur, from its primvars (with those
// inherited from ancestors) or the render settings' defaults.
int resolveMode(HdPrimvarsSchema const &primvars, bool instancer, HdNovaVelocityModeSceneIndex::State const &state) {
    int mode = HdNovaVelocityBlurMode(
        primvarValue(primvars, instancer ? _tokens->novaInstanceVelocityBlur : _tokens->novaVelocityBlur));
    if (mode < 0)
        mode = HdNovaVelocityBlurMode(
            primvarValue(primvars, instancer ? _tokens->karmaInstanceVelocityBlur : _tokens->karmaVelocityBlur));
    if (mode >= 0)
        return mode;
    const HdNovaVelocityDefaults defaults = state.Get();
    return instancer ? defaults.instanceVelocityBlur : defaults.velocityBlur;
}

int resolveSamples(HdPrimvarsSchema const &primvars, bool instancer, HdNovaVelocityModeSceneIndex::State const &state) {
    int samples = sampleCount(primvarValue(primvars, _tokens->novaAccelerationSamples));
    if (samples < 0)
        samples = sampleCount(primvarValue(primvars, instancer ? _tokens->karmaInstanceSamples : _tokens->karmaGeoSamples));
    if (samples >= 0)
        return samples;
    const HdNovaVelocityDefaults defaults = state.Get();
    return instancer ? defaults.instanceSamples : defaults.geoSamples;
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

TF_DEFINE_PRIVATE_TOKENS(_pxrTokens, (angularVelocities));

// Identity orientations, one per angular velocity: the resolving index only
// turns orientations that exist, Karma turns instances without them as well.
class IdentityRotationsDataSource final : public HdTypedSampledDataSource<VtQuathArray> {
public:
    HD_DECLARE_DATASOURCE(IdentityRotationsDataSource);

    VtValue GetValue(Time t) override { return VtValue(GetTypedValue(t)); }
    VtQuathArray GetTypedValue(Time t) override {
        const size_t n = _angular ? _angular->GetValue(t).GetArraySize() : 0;
        return VtQuathArray(n, GfQuath::GetIdentity());
    }
    bool GetContributingSampleTimesForInterval(Time, Time, std::vector<Time> *) override { return false; }

private:
    explicit IdentityRotationsDataSource(HdSampledDataSourceHandle angular) : _angular(std::move(angular)) {}
    HdSampledDataSourceHandle _angular;
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
        if (mode == 2)
            names.push_back(HdTokens->nonlinearSampleCount);
        if (mode >= 1 && _NeedsRotations(primvars))
            names.push_back(HdInstancerTokens->instanceRotations);
        return names;
    }
    HdDataSourceBaseHandle Get(TfToken const &name) override {
        if (name == HdTokens->nonlinearSampleCount) {
            if (resolveMode(HdPrimvarsSchema::GetFromParent(_prim), _instancer, *_state) != 2)
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
                    primvars.GetPrimvar(_pxrTokens->angularVelocities).GetPrimvarValue()))
                .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->instance))
                .Build();
        }
        return nullptr;
    }

private:
    PrimvarsDataSource(HdContainerDataSourceHandle prim, bool instancer, StatePtr state)
        : _prim(std::move(prim)), _instancer(instancer), _state(std::move(state)) {}
    bool _NeedsRotations(HdPrimvarsSchema const &primvars) const {
        return _instancer && primvars.GetPrimvar(_pxrTokens->angularVelocities) &&
               !primvars.GetPrimvar(HdInstancerTokens->instanceRotations);
    }
    HdContainerDataSourceHandle _prim;
    bool _instancer;
    StatePtr _state;
};

// Downstream of the resolving index: a prim whose velocity motion has two
// samples (velocity blur, and acceleration blur at Karma's default of 2)
// samples its resolved primvars at shutter open and close only. The
// resolving index samples rotations (angular velocities) and accelerations
// at least three times; Karma uses the two ends.
bool resolvedTwice(HdContainerDataSourceHandle const &prim) {
    auto modeDs = HdTypedSampledDataSource<TfToken>::Cast(
        prim->Get(HdsiVelocityMotionResolvingSceneIndexTokens->velocityMotionMode));
    if (!modeDs)
        return false;
    const TfToken mode = modeDs->GetTypedValue(0.0f);
    const bool twice = mode == HdsiVelocityMotionResolvingSceneIndexTokens->noAcceleration ||
                       (mode == HdsiVelocityMotionResolvingSceneIndexTokens->enable &&
                        sampleCount(primvarValue(prim, HdTokens->nonlinearSampleCount)) <= 2);
    // Only motion from velocities (a prim without them keeps its time samples).
    HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim);
    return twice && (primvars.GetPrimvar(HdTokens->velocities) || primvars.GetPrimvar(_pxrTokens->angularVelocities));
}

class EndpointsDataSource final : public HdSampledDataSource {
public:
    HD_DECLARE_DATASOURCE(EndpointsDataSource);

    VtValue GetValue(Time t) override { return _input->GetValue(t); }
    bool GetContributingSampleTimesForInterval(Time start, Time end, std::vector<Time> *times) override {
        if (!_input->GetContributingSampleTimesForInterval(start, end, times))
            return false;
        // Only the resolving index's own samples (within the interval, from
        // end to end); USD's samples (invalid velocities) stay as they are.
        if (times->size() > 2 && times->front() == start && times->back() == end && resolvedTwice(_prim))
            *times = {start, end};
        return true;
    }

private:
    EndpointsDataSource(HdSampledDataSourceHandle input, HdContainerDataSourceHandle prim)
        : _input(std::move(input)), _prim(std::move(prim)) {}
    HdSampledDataSourceHandle _input;
    HdContainerDataSourceHandle _prim;
};

class EndpointPrimvarsDataSource final : public HdContainerDataSource {
public:
    HD_DECLARE_DATASOURCE(EndpointPrimvarsDataSource);

    // Overlaid on the input's primvars, which name everything.
    TfTokenVector GetNames() override { return {}; }
    HdDataSourceBaseHandle Get(TfToken const &name) override {
        if (name != HdTokens->points && name != HdInstancerTokens->instanceTranslations &&
            name != HdInstancerTokens->instanceRotations && name != HdInstancerTokens->instanceScales)
            return nullptr;
        HdPrimvarSchema pv = HdPrimvarsSchema::GetFromParent(_prim).GetPrimvar(name);
        HdSampledDataSourceHandle value = pv.GetPrimvarValue();
        if (!value)
            return nullptr;
        return HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(HdPrimvarSchemaTokens->primvarValue,
                                               EndpointsDataSource::New(value, _prim)),
            pv.GetContainer());
    }

private:
    explicit EndpointPrimvarsDataSource(HdContainerDataSourceHandle prim) : _prim(std::move(prim)) {}
    HdContainerDataSourceHandle _prim;
};

TF_DECLARE_REF_PTRS(EndpointsSceneIndex);

class EndpointsSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static EndpointsSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input) {
        return TfCreateRefPtr(new EndpointsSceneIndex(input));
    }
    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
        if (prim.dataSource && HdsiVelocityMotionResolvingSceneIndex::PrimTypeSupportsVelocityMotion(prim.primType))
            prim.dataSource = HdOverlayContainerDataSource::New(
                HdRetainedContainerDataSource::New(HdPrimvarsSchema::GetSchemaToken(),
                                                   EndpointPrimvarsDataSource::New(prim.dataSource)),
                prim.dataSource);
        return prim;
    }
    SdfPathVector GetChildPrimPaths(SdfPath const &primPath) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
    }

protected:
    explicit EndpointsSceneIndex(HdSceneIndexBaseRefPtr const &input) : HdSingleInputFilteringSceneIndexBase(input) {}
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

// Live indices by render instance (weak: the render index owns them).
std::mutex &registryMutex() {
    static std::mutex m;
    return m;
}

std::vector<std::pair<std::string, HdNovaVelocityModeSceneIndexPtr>> &registry() {
    static std::vector<std::pair<std::string, HdNovaVelocityModeSceneIndexPtr>> r;
    return r;
}

} // namespace

HdNovaVelocityModeSceneIndexRefPtr HdNovaVelocityModeSceneIndex::New(HdSceneIndexBaseRefPtr const &input,
                                                                     std::string const &renderInstanceId) {
    HdNovaVelocityModeSceneIndexRefPtr result =
        TfCreateRefPtr(new HdNovaVelocityModeSceneIndex(input, renderInstanceId));
    std::lock_guard<std::mutex> lock(registryMutex());
    registry().emplace_back(renderInstanceId, HdNovaVelocityModeSceneIndexPtr(result));
    return result;
}

HdNovaVelocityModeSceneIndex::HdNovaVelocityModeSceneIndex(HdSceneIndexBaseRefPtr const &input,
                                                           std::string const &renderInstanceId)
    : HdSingleInputFilteringSceneIndexBase(input), _renderInstanceId(renderInstanceId),
      _state(std::make_shared<State>()) {}

HdNovaVelocityModeSceneIndex::~HdNovaVelocityModeSceneIndex() {
    std::lock_guard<std::mutex> lock(registryMutex());
    auto &r = registry();
    r.erase(std::remove_if(r.begin(), r.end(),
                           [this](auto const &entry) { return !entry.second || get_pointer(entry.second) == this; }),
            r.end());
}

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

void HdNovaVelocityModeSceneIndex::_DirtyAll(bool timeCodes) {
    if (!_IsObserved())
        return;
    HdSceneIndexObserver::DirtiedPrimEntries entries;
    if (timeCodes) {
        HdDataSourceLocatorSet root;
        root.insert(HdSceneGlobalsSchema::GetTimeCodesPerSecondLocator());
        entries.emplace_back(HdSceneGlobalsSchema::GetDefaultPrimPath(), root);
    }
    SdfPathVector stack{SdfPath::AbsoluteRootPath()};
    while (!stack.empty()) {
        const SdfPath path = stack.back();
        stack.pop_back();
        if (HdsiVelocityMotionResolvingSceneIndex::PrimTypeSupportsVelocityMotion(
                _GetInputSceneIndex()->GetPrim(path).primType))
            entries.emplace_back(path, resolvedLocators());
        for (SdfPath const &child : _GetInputSceneIndex()->GetChildPrimPaths(path))
            stack.push_back(child);
    }
    if (!entries.empty())
        _SendPrimsDirtied(entries);
}

bool HdNovaVelocityModeSceneIndex::SetDefaults(std::string const &renderInstanceId,
                                               HdNovaVelocityDefaults const &defaults) {
    std::vector<HdNovaVelocityModeSceneIndexRefPtr> targets;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        for (auto const &[id, index] : registry())
            if (id == renderInstanceId && index)
                targets.emplace_back(index);
        // As HdNovaResyncSceneIndex: a render index created without an
        // instance name hands "" to the plugins but reports its address.
        if (targets.empty())
            for (auto const &[id, index] : registry())
                if (id.empty() && index)
                    targets.emplace_back(index);
    }
    bool changed = false;
    for (HdNovaVelocityModeSceneIndexRefPtr const &index : targets) {
        bool timeCodes = false;
        {
            std::lock_guard<std::mutex> lock(index->_state->mutex);
            if (index->_state->defaults == defaults)
                continue;
            timeCodes = index->_state->defaults.timeCodesPerSecond != defaults.timeCodesPerSecond;
            index->_state->defaults = defaults;
        }
        TF_DEBUG(HDNOVA_MOTION)
            .Msg("Nova: velocity blur defaults vblur %d instance_vblur %d geosamples %d instance_samples %d, "
                 "%g time codes per second\n",
                 defaults.velocityBlur, defaults.instanceVelocityBlur, defaults.geoSamples, defaults.instanceSamples,
                 defaults.timeCodesPerSecond);
        index->_DirtyAll(timeCodes);
        changed = true;
    }
    return changed;
}

HdSceneIndexBaseRefPtr HdNovaVelocityModeSceneIndex::Append(HdSceneIndexBaseRefPtr const &input,
                                                           std::string const &renderInstanceId) {
    return EndpointsSceneIndex::New(HdsiVelocityMotionResolvingSceneIndex::New(New(input, renderInstanceId)));
}

PXR_NAMESPACE_CLOSE_SCOPE
