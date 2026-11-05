#pragma once

// The volume bridge (docs/design/M8.md D1, houdini/volume_bridge): a
// separate, HDK-linked DLL that hdNova loads at run time. It resolves a USD
// volume field the way Karma does (in-memory "op:" paths, .vdb and
// .bgeo.sc files) and returns its grids as NanoVDB float grids. No Houdini
// types appear here, so hdNova and core stay HDK-free.

#include "core/volume/grid.h"

#include <string>
#include <vector>

namespace nova {

struct VolumeFieldRequest {
    std::string filePath;     // the resolved file path, or the "op:" path as authored
    std::string fieldName;    // the grid or volume name
    int fieldIndex = 0;       // the index among same-named VDB grids, or the primitive number of a native volume
    bool houdiniField = false; // a HoudiniFieldAsset (native volumes and .bgeo); else an OpenVDBAsset
};

// A scalar field has one grid; a vector field three (x, y, z), each with the
// field's index-to-object map in its NanoVDB map.
struct VolumeFieldGrids {
    std::vector<VolumeGridPtr> grids;
};

class VolumeBridge {
public:
    virtual ~VolumeBridge() = default;
    // Loads a field. Thread-safe (calls into Houdini are serialized).
    // False with `error` if it cannot be read.
    virtual bool load(const VolumeFieldRequest &request, VolumeFieldGrids &out, std::string *error) = 0;
};

// The bridge DLL's entry point (extern "C", resolved by name).
using VolumeBridgeFactory = VolumeBridge *(*)();
constexpr const char *kVolumeBridgeEntry = "novaVolumeBridge";

} // namespace nova
