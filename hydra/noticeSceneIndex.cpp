#include "noticeSceneIndex.h"

#include <pxr/imaging/hd/categoriesSchema.h>
#include <pxr/imaging/hd/collectionsSchema.h>
#include <pxr/imaging/hd/instanceCategoriesSchema.h>
#include <pxr/imaging/hd/instanceSchema.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/tokens.h>

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

HdNovaRelinkSceneIndexRefPtr HdNovaRelinkSceneIndex::New(HdSceneIndexBaseRefPtr const &input) {
    return TfCreateRefPtr(new HdNovaRelinkSceneIndex(input));
}

HdNovaRelinkSceneIndex::HdNovaRelinkSceneIndex(HdSceneIndexBaseRefPtr const &input)
    : HdSingleInputFilteringSceneIndexBase(input) {}

HdSceneIndexPrim HdNovaRelinkSceneIndex::GetPrim(SdfPath const &primPath) const {
    return _GetInputSceneIndex()->GetPrim(primPath);
}

SdfPathVector HdNovaRelinkSceneIndex::GetChildPrimPaths(SdfPath const &primPath) const {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdNovaRelinkSceneIndex::_PrimsAdded(HdSceneIndexBase const &,
                                         HdSceneIndexObserver::AddedPrimEntries const &entries) {
    _SendPrimsAdded(entries);
    HdSceneIndexObserver::DirtiedPrimEntries again;
    for (HdSceneIndexObserver::AddedPrimEntry const &entry : entries) {
        if (!HdPrimTypeIsLight(entry.primType) && entry.primType != HdPrimTypeTokens->lightFilter)
            continue;
        if (!_lights.insert(entry.primPath).second)
            again.emplace_back(entry.primPath, HdCollectionsSchema::GetDefaultLocator());
    }
    if (!again.empty())
        _SendPrimsDirtied(again);
}

void HdNovaRelinkSceneIndex::_PrimsRemoved(HdSceneIndexBase const &,
                                           HdSceneIndexObserver::RemovedPrimEntries const &entries) {
    _SendPrimsRemoved(entries);
}

void HdNovaRelinkSceneIndex::_PrimsDirtied(HdSceneIndexBase const &,
                                           HdSceneIndexObserver::DirtiedPrimEntries const &entries) {
    _SendPrimsDirtied(entries);
}

HdNovaNoticeSceneIndexRefPtr HdNovaNoticeSceneIndex::New(HdSceneIndexBaseRefPtr const &input) {
    return TfCreateRefPtr(new HdNovaNoticeSceneIndex(input));
}

HdNovaNoticeSceneIndex::HdNovaNoticeSceneIndex(HdSceneIndexBaseRefPtr const &input)
    : HdSingleInputFilteringSceneIndexBase(input) {}

HdSceneIndexPrim HdNovaNoticeSceneIndex::GetPrim(SdfPath const &primPath) const {
    return _GetInputSceneIndex()->GetPrim(primPath);
}

SdfPathVector HdNovaNoticeSceneIndex::GetChildPrimPaths(SdfPath const &primPath) const {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdNovaNoticeSceneIndex::_PrimsAdded(HdSceneIndexBase const &,
                                         HdSceneIndexObserver::AddedPrimEntries const &entries) {
    _SendPrimsAdded(entries);
}

void HdNovaNoticeSceneIndex::_PrimsRemoved(HdSceneIndexBase const &,
                                           HdSceneIndexObserver::RemovedPrimEntries const &entries) {
    _SendPrimsRemoved(entries);
}

void HdNovaNoticeSceneIndex::_PrimsDirtied(HdSceneIndexBase const &,
                                           HdSceneIndexObserver::DirtiedPrimEntries const &entries) {
    static const HdDataSourceLocator categories = HdCategoriesSchema::GetDefaultLocator();
    static const HdDataSourceLocator light = HdLightSchema::GetDefaultLocator();
    static const HdDataSourceLocator material = HdMaterialSchema::GetDefaultLocator();
    SdfPathVector instancers, meshLights;
    for (HdSceneIndexObserver::DirtiedPrimEntry const &entry : entries) {
        const bool linked = entry.dirtyLocators.Intersects(categories);
        const bool lit = entry.dirtyLocators.Intersects(light) || entry.dirtyLocators.Intersects(material);
        if (!linked && !lit)
            continue;
        const HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(entry.primPath);
        if (!prim.dataSource)
            continue;
        if (linked) {
            // A native instance names the instancer that holds it.
            const HdInstanceSchema instance = HdInstanceSchema::GetFromParent(prim.dataSource);
            const HdPathDataSourceHandle path = instance.IsDefined() ? instance.GetInstancer() : HdPathDataSourceHandle();
            const SdfPath instancer = path ? path->GetTypedValue(0.0f) : SdfPath();
            if (!instancer.IsEmpty() && std::find(instancers.begin(), instancers.end(), instancer) == instancers.end())
                instancers.push_back(instancer);
        }
        // A mesh that is a light (MeshLightAPI) reads its light with its primvars.
        if (lit && prim.primType == HdPrimTypeTokens->mesh && HdLightSchema::GetFromParent(prim.dataSource).IsDefined())
            meshLights.push_back(entry.primPath);
    }
    if (instancers.empty() && meshLights.empty()) {
        _SendPrimsDirtied(entries);
        return;
    }
    HdSceneIndexObserver::DirtiedPrimEntries all(entries.begin(), entries.end());
    for (SdfPath const &instancer : instancers)
        all.emplace_back(instancer, HdInstanceCategoriesSchema::GetDefaultLocator());
    for (SdfPath const &mesh : meshLights)
        all.emplace_back(mesh, HdPrimvarsSchema::GetDefaultLocator());
    _SendPrimsDirtied(all);
}

PXR_NAMESPACE_CLOSE_SCOPE
