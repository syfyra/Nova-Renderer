#pragma once

// Cryptomatte names of Hydra prims (docs/design/M6.md D10; Karma's names,
// measured in docs/research/m6/cryptomatte.md 5):
//   objects   the gprim's USD path (HdPrimOriginSchema); native instances:
//             the instancer's first instance location plus the prototype-
//             relative path; point-instancer prototypes: the prototype path;
//   materials the bound material's path, "defaultshader" when unbound;
//   primvars  string values, ints as decimal strings (constant or per face).

#include "core/film/crypto.h"

#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// The names behind the ids, per cryptomatte source ("__name",
// "__materialname" or a primvar name), for the manifests. Thread-safe.
class HdNovaCryptoNames {
public:
    uint32_t Note(std::string const &source, std::string const &name);
    std::string Find(std::string const &source, uint32_t id) const;

private:
    mutable std::mutex _mutex;
    std::unordered_map<std::string, std::unordered_map<uint32_t, std::string>> _names;
};

std::string HdNovaCryptoObjectName(HdSceneDelegate *sd, SdfPath const &id, SdfPath const &instancerId);

// The constant and uniform string/int primvars of a gprim as cryptomatte
// attributes (their names noted); with an instancer also the instancers'
// instance-rate ones the gprim does not define itself, one value per
// instance (Karma: the gprim's own primvar wins, then the innermost
// instancer's; docs/design/M11-P6.md 7).
std::vector<nova::CryptoAttribute> HdNovaCryptoAttributes(HdSceneDelegate *sd, SdfPath const &id,
                                                          HdNovaCryptoNames &names,
                                                          SdfPath const &instancerId = SdfPath());

PXR_NAMESPACE_CLOSE_SCOPE
