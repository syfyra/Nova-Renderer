#include "renderPassSceneIndex.h"

#include <pxr/base/tf/staticTokens.h>
#include <pxr/imaging/hd/collectionSchema.h>
#include <pxr/imaging/hd/collectionsSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneGlobalsSchema.h>
#include <pxr/imaging/hd/sceneIndexPrimView.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/visibilitySchema.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (prune)
    (renderVisibility)
    (cameraVisibility)
    (matte)
    ((karmaRenderVisibility, "karma:object:rendervisibility"))
    ((karmaHoldoutMode, "karma:object:holdoutmode"))
);

namespace {

// What Nova renders as geometry. This index is the first of Nova's chain:
// the implicit prims are not meshes yet.
bool isGeometry(TfToken const &type) {
    return type == HdPrimTypeTokens->mesh || type == HdPrimTypeTokens->basisCurves ||
           type == HdPrimTypeTokens->points || type == HdPrimTypeTokens->volume || type == HdPrimTypeTokens->sphere ||
           type == HdPrimTypeTokens->cube || type == HdPrimTypeTokens->cone || type == HdPrimTypeTokens->cylinder ||
           type == HdPrimTypeTokens->capsule || type == HdPrimTypeTokens->plane;
}

HdDataSourceBaseHandle constantText(std::string const &text) {
    return HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<std::string>::New(text))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant))
        .Build();
}

// The prim's own rendervisibility expression ("" if it has none).
std::string ownRenderVisibility(HdContainerDataSourceHandle const &prim) {
    HdPrimvarSchema pv = HdPrimvarsSchema::GetFromParent(prim).GetPrimvar(_tokens->karmaRenderVisibility);
    HdSampledDataSourceHandle ds = pv ? pv.GetPrimvarValue() : nullptr;
    const VtValue v = ds ? ds->GetValue(0.0f) : VtValue();
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    if (v.IsHolding<VtArray<std::string>>() && v.UncheckedGet<VtArray<std::string>>().size() == 1)
        return v.UncheckedGet<VtArray<std::string>>()[0];
    return {};
}

} // namespace

HdNovaRenderPassSceneIndex::Effect HdNovaRenderPassSceneIndex::Pass::effectOn(SdfPath const &primPath,
                                                                              TfToken const &primType) const {
    Effect e;
    const bool geometry = isGeometry(primType);
    if (!geometry && !HdPrimTypeIsLight(primType))
        return e;
    e.hidden = visible && !visible->Match(primPath);
    e.noCamera = geometry && camera && !camera->Match(primPath);
    e.matte = geometry && mattes && mattes->Match(primPath);
    return e;
}

HdNovaRenderPassSceneIndexRefPtr HdNovaRenderPassSceneIndex::New(HdSceneIndexBaseRefPtr const &input) {
    return TfCreateRefPtr(new HdNovaRenderPassSceneIndex(input));
}

HdNovaRenderPassSceneIndex::HdNovaRenderPassSceneIndex(HdSceneIndexBaseRefPtr const &input)
    : HdSingleInputFilteringSceneIndexBase(input) {
    _Update(nullptr); // an input that is populated already
}

HdNovaRenderPassSceneIndex::~HdNovaRenderPassSceneIndex() = default;

HdSceneIndexPrim HdNovaRenderPassSceneIndex::GetPrim(SdfPath const &primPath) const {
    if (_pass.prunes(primPath))
        return HdSceneIndexPrim();
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    if (!prim.dataSource || !_pass.any())
        return prim;
    const Effect e = _pass.effectOn(primPath, prim.primType);
    if (!e.any())
        return prim;

    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    if (e.hidden) {
        names.push_back(HdVisibilitySchema::GetSchemaToken());
        values.push_back(
            HdVisibilitySchema::Builder().SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(false)).Build());
    }
    if (e.noCamera || e.matte) {
        std::vector<TfToken> primvarNames;
        std::vector<HdDataSourceBaseHandle> primvars;
        if (e.noCamera) {
            // A later negated term takes its ray types out of what stands before it (core/scene/visibility.h).
            const std::string own = ownRenderVisibility(prim.dataSource);
            primvarNames.push_back(_tokens->karmaRenderVisibility);
            primvars.push_back(constantText(own.empty() ? "-primary" : "(" + own + ") -primary"));
        }
        if (e.matte) {
            primvarNames.push_back(_tokens->karmaHoldoutMode);
            primvars.push_back(constantText("matte"));
        }
        names.push_back(HdPrimvarsSchema::GetSchemaToken());
        values.push_back(HdRetainedContainerDataSource::New(primvarNames.size(), primvarNames.data(), primvars.data()));
    }
    prim.dataSource = HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(names.size(), names.data(), values.data()), prim.dataSource);
    return prim;
}

SdfPathVector HdNovaRenderPassSceneIndex::GetChildPrimPaths(SdfPath const &primPath) const {
    SdfPathVector children = _GetInputSceneIndex()->GetChildPrimPaths(primPath);
    if (_pass.pruned)
        children.erase(std::remove_if(children.begin(), children.end(),
                                      [this](SdfPath const &child) { return _pass.prunes(child); }),
                       children.end());
    return children;
}

void HdNovaRenderPassSceneIndex::_Send(Changes const &changes) {
    if (!changes.removed.empty())
        _SendPrimsRemoved(changes.removed);
    if (!changes.added.empty())
        _SendPrimsAdded(changes.added);
    if (!changes.dirtied.empty())
        _SendPrimsDirtied(changes.dirtied);
}

void HdNovaRenderPassSceneIndex::_Update(Changes *changes) {
    const HdSceneIndexBaseRefPtr input = _GetInputSceneIndex();
    Pass next;
    if (HdPathDataSourceHandle path = HdSceneGlobalsSchema::GetFromSceneIndex(input).GetActiveRenderPassPrim())
        next.path = path->GetTypedValue(0.0f);
    if (!next.path.IsEmpty()) {
        const HdSceneIndexPrim pass = input->GetPrim(next.path);
        const HdCollectionsSchema collections = HdCollectionsSchema::GetFromParent(pass.dataSource);
        const auto compile = [&](TfToken const &name, SdfPathExpression &expression,
                                 std::optional<HdCollectionExpressionEvaluator> &evaluator) {
            if (!collections)
                return;
            HdCollectionSchema collection = collections.GetCollection(name);
            HdPathExpressionDataSourceHandle membership = collection ? collection.GetMembershipExpression() : nullptr;
            if (!membership)
                return;
            expression = membership->GetTypedValue(0.0f);
            if (!expression.IsEmpty())
                evaluator.emplace(input, expression);
        };
        compile(_tokens->prune, next.prune, next.pruned);
        compile(_tokens->renderVisibility, next.renderVisibility, next.visible);
        compile(_tokens->cameraVisibility, next.cameraVisibility, next.camera);
        compile(_tokens->matte, next.matte, next.mattes);
    }
    if (next.path == _pass.path && next.prune == _pass.prune && next.renderVisibility == _pass.renderVisibility &&
        next.cameraVisibility == _pass.cameraVisibility && next.matte == _pass.matte)
        return;
    const Pass old = std::exchange(_pass, std::move(next));
    if (!changes || (!old.any() && !_pass.any()))
        return;
    for (SdfPath const &path : HdSceneIndexPrimView(input)) {
        const bool was = old.prunes(path), is = _pass.prunes(path);
        if (was && is)
            continue;
        if (is) { // gone with everything below it: its topmost pruned prim says so
            if (!_pass.prunes(path.GetParentPath()))
                changes->removed.emplace_back(path);
            continue;
        }
        const TfToken type = input->GetPrim(path).primType;
        if (was) {
            changes->added.emplace_back(path, type);
            continue;
        }
        const Effect before = old.effectOn(path, type), after = _pass.effectOn(path, type);
        if (before == after)
            continue;
        HdDataSourceLocatorSet locators;
        if (before.hidden != after.hidden)
            locators.insert(HdVisibilitySchema::GetDefaultLocator());
        if (before.noCamera != after.noCamera || before.matte != after.matte)
            locators.insert(HdPrimvarsSchema::GetDefaultLocator());
        changes->dirtied.emplace_back(path, locators);
    }
}

void HdNovaRenderPassSceneIndex::_PrimsAdded(HdSceneIndexBase const &,
                                             HdSceneIndexObserver::AddedPrimEntries const &entries) {
    // The first prims: nothing downstream has been told anything yet. Later
    // the pass prim itself may arrive after the globals named it.
    Changes changes;
    bool update = !_populated;
    for (HdSceneIndexObserver::AddedPrimEntry const &e : entries)
        update = update || (!_pass.path.IsEmpty() && e.primPath == _pass.path);
    if (update)
        _Update(_populated ? &changes : nullptr);
    _populated = true;
    if (!_pass.pruned) {
        _SendPrimsAdded(entries);
    } else {
        HdSceneIndexObserver::AddedPrimEntries kept;
        for (HdSceneIndexObserver::AddedPrimEntry const &e : entries)
            if (!_pass.prunes(e.primPath))
                kept.push_back(e);
        if (!kept.empty())
            _SendPrimsAdded(kept);
    }
    _Send(changes);
}

void HdNovaRenderPassSceneIndex::_PrimsRemoved(HdSceneIndexBase const &,
                                               HdSceneIndexObserver::RemovedPrimEntries const &entries) {
    Changes changes;
    bool update = false;
    for (HdSceneIndexObserver::RemovedPrimEntry const &e : entries)
        update = update || (!_pass.path.IsEmpty() && _pass.path.HasPrefix(e.primPath));
    _SendPrimsRemoved(entries);
    if (update)
        _Update(&changes);
    _Send(changes);
}

void HdNovaRenderPassSceneIndex::_PrimsDirtied(HdSceneIndexBase const &,
                                               HdSceneIndexObserver::DirtiedPrimEntries const &entries) {
    // Another pass in the scene globals, or the pass's collections edited.
    bool update = false;
    for (HdSceneIndexObserver::DirtiedPrimEntry const &e : entries) {
        if (e.primPath == HdSceneGlobalsSchema::GetDefaultPrimPath())
            update = update || e.dirtyLocators.Intersects(HdSceneGlobalsSchema::GetActiveRenderPassPrimLocator());
        else if (!_pass.path.IsEmpty() && e.primPath == _pass.path)
            update = update || e.dirtyLocators.Intersects(HdCollectionsSchema::GetDefaultLocator());
    }
    if (!_pass.pruned) {
        _SendPrimsDirtied(entries);
    } else {
        HdSceneIndexObserver::DirtiedPrimEntries kept;
        for (HdSceneIndexObserver::DirtiedPrimEntry const &e : entries)
            if (!_pass.prunes(e.primPath))
                kept.push_back(e);
        if (!kept.empty())
            _SendPrimsDirtied(kept);
    }
    if (!update)
        return;
    Changes changes;
    _Update(&changes);
    _Send(changes);
}

PXR_NAMESPACE_CLOSE_SCOPE
