#include "stablePath.h"

#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneIndex.h>

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

const char kPrototype[] = "__Prototype_";
const char kNiInstancer[] = "UsdNiInstancer";
const char kNiPrototype[] = "UsdNiPrototype";
const char kForInstancer[] = "ForInstancer";

bool startsWith(std::string const &text, const char *prefix) { return text.rfind(prefix, 0) == 0; }

std::string stable(HdSceneIndexBaseRefPtr const &si, SdfPath const &id, int depth) {
    const std::string &raw = id.GetString();
    if (!si || depth > 16 || raw.find(kPrototype) == std::string::npos)
        return raw;
    const SdfPathVector prefixes = id.GetPrefixes(); // /a, /a/b, ...
    std::string out;
    for (size_t i = 0; i < prefixes.size(); ++i) {
        const std::string &name = prefixes[i].GetName();
        // .../__Prototype_N/UsdNiInstancer[/UsdNiPrototype/...]: the first instance's place.
        if (startsWith(name, kPrototype) && i + 1 < prefixes.size() && prefixes[i + 1].GetName() == kNiInstancer) {
            const HdInstancerTopologySchema topology =
                HdInstancerTopologySchema::GetFromParent(si->GetPrim(prefixes[i + 1]).dataSource);
            const HdPathArrayDataSourceHandle locations =
                topology.IsDefined() ? topology.GetInstanceLocations() : HdPathArrayDataSourceHandle();
            const VtArray<SdfPath> paths = locations ? locations->GetTypedValue(0.0f) : VtArray<SdfPath>();
            if (!paths.empty()) {
                out = stable(si, *std::min_element(paths.begin(), paths.end()), depth + 1);
                ++i; // the instancer
                if (i + 1 < prefixes.size() && prefixes[i + 1].GetName() == kNiPrototype)
                    ++i;
                else if (i + 1 == prefixes.size())
                    out += "/" + std::string(kNiInstancer); // the instancer itself
                continue;
            }
        }
        // .../ForInstancer<hash of the instancer's path>: the instancer's stable path.
        if (startsWith(name, kForInstancer)) {
            const HdInstancedBySchema by = HdInstancedBySchema::GetFromParent(si->GetPrim(prefixes[i]).dataSource);
            const HdPathArrayDataSourceHandle instancers = by.IsDefined() ? by.GetPaths() : HdPathArrayDataSourceHandle();
            const VtArray<SdfPath> paths = instancers ? instancers->GetTypedValue(0.0f) : VtArray<SdfPath>();
            if (!paths.empty()) {
                out += "/" + std::string(kForInstancer) + "{" + stable(si, paths[0], depth + 1) + "}";
                continue;
            }
        }
        out += "/" + name;
    }
    return out;
}

} // namespace

std::string HdNovaStablePath(HdSceneIndexBaseRefPtr const &sceneIndex, SdfPath const &id) {
    return stable(sceneIndex, id, 0);
}

std::string HdNovaStablePath(HdSceneDelegate *sd, SdfPath const &id) {
    if (id.GetString().find(kPrototype) == std::string::npos)
        return id.GetString();
    return stable(sd->GetRenderIndex().GetTerminalSceneIndex(), id, 0);
}

PXR_NAMESPACE_CLOSE_SCOPE
