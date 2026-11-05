#pragma once

// Volume fields (docs/design/M8.md D1, D3): the bprims openvdbAsset and
// houdiniFieldAsset say where a field comes from (file or in-memory "op:"
// path, field name, index). The grids load through the volume bridge
// (nova_volume.dll) when a volume first needs them, and stay until the field
// changes (a SOP recook gives a new "&t=" path: DirtyParams).

#include "core/volume/bridge.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/imaging/hd/field.h>
#include <pxr/pxr.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaField final : public HdField {
public:
    HdNovaField(SdfPath const &id, TfToken const &primType);

    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;

    struct Grids {
        std::vector<nova::VolumeGridPtr> grids; // one, or three (x, y, z) for a vector field; empty on failure
        std::string fieldName;                 // the field's own name
        GfMatrix4d transform{1.0};             // the field prim's world transform
        uint64_t version = 0;
    };
    // The field's grids, loaded on the first call after a change (volumes
    // sync in parallel: thread-safe). A field that cannot be read warns once
    // per change and gives no grids.
    Grids Load();
    // Bumped whenever the field's source or transform changes.
    uint64_t GetVersion() const;
    // The field's own name (fieldName), without loading it.
    std::string GetFieldName() const;

    // The houdiniFieldAsset prim type (Houdini's HoudiniFieldAsset schema).
    static TfToken const &HoudiniFieldAssetToken();
    static TfToken const &OpenVdbAssetToken();

private:
    TfToken _primType;
    mutable std::mutex _mutex;
    nova::VolumeFieldRequest _request;
    GfMatrix4d _transform{1.0};
    uint64_t _version = 1;
    bool _loaded = false;
    std::vector<nova::VolumeGridPtr> _grids;
};

// The volume bridge, loaded on first use from next to the module (or
// NOVA_VOLUME_BRIDGE); null with `error` if it cannot be loaded.
nova::VolumeBridge *HdNovaVolumeBridge(std::string *error = nullptr);

PXR_NAMESPACE_CLOSE_SCOPE
