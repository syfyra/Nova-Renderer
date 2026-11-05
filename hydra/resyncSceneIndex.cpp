#include "resyncSceneIndex.h"

#include <pxr/base/tf/weakPtr.h>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// Live resync scene indices by render instance (weak: the render index owns
// them through its scene index chain).
std::mutex &registryMutex() {
    static std::mutex m;
    return m;
}

std::vector<std::pair<std::string, HdNovaResyncSceneIndexPtr>> &registry() {
    static std::vector<std::pair<std::string, HdNovaResyncSceneIndexPtr>> r;
    return r;
}

} // namespace

HdNovaResyncSceneIndexRefPtr HdNovaResyncSceneIndex::New(HdSceneIndexBaseRefPtr const &input,
                                                         std::string const &renderInstanceId) {
    HdNovaResyncSceneIndexRefPtr result = TfCreateRefPtr(new HdNovaResyncSceneIndex(input, renderInstanceId));
    std::lock_guard<std::mutex> lock(registryMutex());
    registry().emplace_back(renderInstanceId, HdNovaResyncSceneIndexPtr(result));
    return result;
}

HdNovaResyncSceneIndex::HdNovaResyncSceneIndex(HdSceneIndexBaseRefPtr const &input,
                                               std::string const &renderInstanceId)
    : HdSingleInputFilteringSceneIndexBase(input), _renderInstanceId(renderInstanceId) {}

HdNovaResyncSceneIndex::~HdNovaResyncSceneIndex() {
    std::lock_guard<std::mutex> lock(registryMutex());
    auto &r = registry();
    r.erase(std::remove_if(r.begin(), r.end(),
                           [this](auto const &entry) { return !entry.second || get_pointer(entry.second) == this; }),
            r.end());
}

HdSceneIndexPrim HdNovaResyncSceneIndex::GetPrim(SdfPath const &primPath) const {
    return _GetInputSceneIndex()->GetPrim(primPath);
}

SdfPathVector HdNovaResyncSceneIndex::GetChildPrimPaths(SdfPath const &primPath) const {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdNovaResyncSceneIndex::_PrimsAdded(HdSceneIndexBase const &,
                                         HdSceneIndexObserver::AddedPrimEntries const &entries) {
    _SendPrimsAdded(entries);
}

void HdNovaResyncSceneIndex::_PrimsRemoved(HdSceneIndexBase const &,
                                           HdSceneIndexObserver::RemovedPrimEntries const &entries) {
    _SendPrimsRemoved(entries);
}

void HdNovaResyncSceneIndex::_PrimsDirtied(HdSceneIndexBase const &,
                                           HdSceneIndexObserver::DirtiedPrimEntries const &entries) {
    _SendPrimsDirtied(entries);
}

namespace {

// Strong references (used outside the lock) to the live indices of a render
// instance. A render index created without an instance name hands "" to the
// plugins but reports its address from GetInstanceName(): the unnamed
// instances then (extra syncs elsewhere are harmless). An index whose last
// reference is gone waits in its destructor for the lock: it is not revived.
std::vector<HdNovaResyncSceneIndexRefPtr> liveIndices(std::string const &renderInstanceId) {
    std::vector<HdNovaResyncSceneIndexRefPtr> targets;
    std::lock_guard<std::mutex> lock(registryMutex());
    for (bool unnamed : {false, true}) {
        for (auto const &[id, index] : registry())
            if (id == (unnamed ? std::string() : renderInstanceId))
                if (HdNovaResyncSceneIndexRefPtr strong = TfCreateRefPtrFromProtectedWeakPtr(index))
                    targets.push_back(strong);
        if (!targets.empty())
            break;
    }
    return targets;
}

} // namespace

bool HdNovaResyncSceneIndex::DirtyPrims(std::string const &renderInstanceId, TfTokenVector const &primTypes,
                                        HdDataSourceLocatorSet const &locators) {
    const std::vector<HdNovaResyncSceneIndexRefPtr> targets = liveIndices(renderInstanceId);
    for (HdNovaResyncSceneIndexRefPtr const &index : targets)
        index->_DirtyAll(primTypes, locators);
    return !targets.empty();
}

bool HdNovaResyncSceneIndex::DirtyPaths(std::string const &renderInstanceId, SdfPathVector const &paths,
                                        HdDataSourceLocatorSet const &locators) {
    const std::vector<HdNovaResyncSceneIndexRefPtr> targets = liveIndices(renderInstanceId);
    for (HdNovaResyncSceneIndexRefPtr const &index : targets) {
        if (!index->_IsObserved())
            continue;
        HdSceneIndexObserver::DirtiedPrimEntries entries;
        for (SdfPath const &path : paths)
            entries.emplace_back(path, locators);
        if (!entries.empty())
            index->_SendPrimsDirtied(entries);
    }
    return !targets.empty();
}

void HdNovaResyncSceneIndex::_DirtyAll(TfTokenVector const &primTypes, HdDataSourceLocatorSet const &locators) {
    if (!_IsObserved())
        return;
    HdSceneIndexObserver::DirtiedPrimEntries entries;
    SdfPathVector stack{SdfPath::AbsoluteRootPath()};
    while (!stack.empty()) {
        const SdfPath path = stack.back();
        stack.pop_back();
        const TfToken type = _GetInputSceneIndex()->GetPrim(path).primType;
        if (std::find(primTypes.begin(), primTypes.end(), type) != primTypes.end())
            entries.emplace_back(path, locators);
        for (SdfPath const &child : _GetInputSceneIndex()->GetChildPrimPaths(path))
            stack.push_back(child);
    }
    if (!entries.empty())
        _SendPrimsDirtied(entries);
}

PXR_NAMESPACE_CLOSE_SCOPE
