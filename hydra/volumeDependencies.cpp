#include "volumeDependencies.h"

#include <pxr/imaging/hd/dependenciesSchema.h>
#include <pxr/imaging/hd/dependencySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/volumeFieldBindingSchema.h>
#include <pxr/imaging/hd/volumeFieldSchema.h>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(_tokens, ((bindingsDependency, "nova_volumeFieldBinding")));

using LocatorDs = HdRetainedTypedSampledDataSource<HdDataSourceLocator>;

// One dependency per bound field, and one that recomputes them when the
// bindings change.
HdContainerDataSourceHandle volumeDependencies(SdfPath const &self, HdContainerDataSourceHandle const &prim) {
    static const HdLocatorDataSourceHandle field = LocatorDs::New(HdVolumeFieldSchema::GetDefaultLocator());
    static const HdLocatorDataSourceHandle binding = LocatorDs::New(HdVolumeFieldBindingSchema::GetDefaultLocator());
    static const HdLocatorDataSourceHandle dependencies = LocatorDs::New(HdDependenciesSchema::GetDefaultLocator());
    TfTokenVector names{_tokens->bindingsDependency};
    std::vector<HdDataSourceBaseHandle> values{HdDependencySchema::Builder()
                                                   .SetDependedOnPrimPath(HdRetainedTypedSampledDataSource<SdfPath>::New(self))
                                                   .SetDependedOnDataSourceLocator(binding)
                                                   .SetAffectedDataSourceLocator(dependencies)
                                                   .Build()};
    const HdVolumeFieldBindingSchema bindings = HdVolumeFieldBindingSchema::GetFromParent(prim);
    for (TfToken const &name : bindings.GetVolumeFieldBindingNames()) {
        HdPathDataSourceHandle const path = bindings.GetVolumeFieldBinding(name);
        if (!path)
            continue;
        names.push_back(TfToken("nova_field_" + name.GetString()));
        values.push_back(HdDependencySchema::Builder()
                             .SetDependedOnPrimPath(path)
                             .SetDependedOnDataSourceLocator(field)
                             .SetAffectedDataSourceLocator(binding)
                             .Build());
    }
    return HdRetainedContainerDataSource::New(names.size(), names.data(), values.data());
}

} // namespace

HdNovaVolumeDependencySceneIndexRefPtr HdNovaVolumeDependencySceneIndex::New(HdSceneIndexBaseRefPtr const &input) {
    return TfCreateRefPtr(new HdNovaVolumeDependencySceneIndex(input));
}

HdNovaVolumeDependencySceneIndex::HdNovaVolumeDependencySceneIndex(HdSceneIndexBaseRefPtr const &input)
    : HdSingleInputFilteringSceneIndexBase(input) {}

HdSceneIndexPrim HdNovaVolumeDependencySceneIndex::GetPrim(SdfPath const &primPath) const {
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    if (prim.primType == HdPrimTypeTokens->volume && prim.dataSource)
        prim.dataSource = HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(HdDependenciesSchema::GetSchemaToken(),
                                               volumeDependencies(primPath, prim.dataSource)),
            prim.dataSource);
    return prim;
}

SdfPathVector HdNovaVolumeDependencySceneIndex::GetChildPrimPaths(SdfPath const &primPath) const {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdNovaVolumeDependencySceneIndex::_PrimsAdded(HdSceneIndexBase const &,
                                                   HdSceneIndexObserver::AddedPrimEntries const &entries) {
    _SendPrimsAdded(entries);
}

void HdNovaVolumeDependencySceneIndex::_PrimsRemoved(HdSceneIndexBase const &,
                                                     HdSceneIndexObserver::RemovedPrimEntries const &entries) {
    _SendPrimsRemoved(entries);
}

void HdNovaVolumeDependencySceneIndex::_PrimsDirtied(HdSceneIndexBase const &,
                                                     HdSceneIndexObserver::DirtiedPrimEntries const &entries) {
    _SendPrimsDirtied(entries);
}

PXR_NAMESPACE_CLOSE_SCOPE
