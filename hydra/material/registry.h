#pragma once

// Material bookkeeping shared by the material and mesh prims (which sync in
// parallel): material path -> scene MaterialId, program sharing by structure
// hash, and warnings once per material and message.

#include "core/geometry/displace.h"
#include "core/scene/scene.h"
#include "core/shading/program.h"

#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// A material's displacement as meshes run it (M4.6), immutable once set.
struct HdNovaDisplacement {
    nova::DisplacementShader shader;
    uint64_t hash = 0; // program structure, parameter values and textures
};
using HdNovaDisplacementPtr = std::shared_ptr<const HdNovaDisplacement>;

// What a material's network renders as (docs/design/M11-P5.md): it has a
// surface terminal, a volume shader, and Karma's karma:volumeuniform on that
// shader. Meshes and spheres with a volume shader become media bounded by
// their surface (hydra/medium.h).
struct HdNovaMaterialKind {
    bool surface = false;
    bool volume = false;
    bool volumeUniform = false;
    bool operator==(const HdNovaMaterialKind &) const = default;
};

class HdNovaMaterialRegistry {
public:
    // The scene material of `path` (an empty path is the default material).
    // Created on first use, rendering the default material until the
    // material prim syncs. `scene` must be acquired for editing.
    nova::MaterialId Acquire(SdfPath const &path, nova::Scene &scene);

    // The program with the same structure, if one exists (shared), else
    // `program` itself.
    nova::shading::ProgramPtr Intern(nova::shading::ProgramPtr program);

    // The material of geometry without one, and of materials that are not
    // translated (yet) or gone: Karma's plastic, or with nova:defaultMaterial
    // "lambert" a matte surface of the display colour. The render pass gives
    // the scene's default material the same.
    void SetPlasticDefault(bool plastic) { _plasticDefault.store(plastic, std::memory_order_relaxed); }
    bool PlasticDefault() const { return _plasticDefault.load(std::memory_order_relaxed); }
    nova::MaterialDesc DefaultMaterial() const;

    // Logs `message` once per material.
    void Warn(SdfPath const &material, std::string const &message);
    // Whether a warning containing `needle` was logged for `material` (tests).
    bool Warned(SdfPath const &material, std::string const &needle) const;

    // Displacement (M4.6). The material prim sets it (null: none); a change
    // queues the meshes bound to the material for a re-sync, which the
    // render pass sends (Hydra never re-syncs meshes on material edits).
    void SetDisplacement(SdfPath const &material, HdNovaDisplacementPtr displacement);
    HdNovaDisplacementPtr GetDisplacement(SdfPath const &material) const;
    // The materials a mesh (or volume) uses (empty: unbind).
    void BindMesh(SdfPath const &mesh, std::vector<SdfPath> const &materials);
    // Meshes queued for a re-sync, cleared.
    std::vector<SdfPath> TakePendingMeshes();

    // Volume shaders (docs/design/M8.md D5): the names the material's volume
    // shader reads (empty: it has none). The material prim sets them; a
    // change queues the prims bound to the material for a re-sync, so
    // volumes load the fields it reads.
    void SetVolumeFields(SdfPath const &material, std::vector<std::string> names);
    std::vector<std::string> GetVolumeFields(SdfPath const &material) const;

    // The material's kind (M11-P5); a change queues the prims bound to it
    // for a re-sync. Unknown materials: no surface, no volume.
    void SetMaterialKind(SdfPath const &material, HdNovaMaterialKind kind);
    HdNovaMaterialKind GetMaterialKind(SdfPath const &material) const;

private:
    mutable std::mutex _displacementMutex;
    std::unordered_map<SdfPath, HdNovaDisplacementPtr, SdfPath::Hash> _displacements;
    std::unordered_map<SdfPath, std::vector<std::string>, SdfPath::Hash> _volumeFields;
    std::unordered_map<SdfPath, HdNovaMaterialKind, SdfPath::Hash> _kinds;
    std::unordered_map<SdfPath, std::set<SdfPath>, SdfPath::Hash> _meshesOfMaterial;
    std::unordered_map<SdfPath, std::vector<SdfPath>, SdfPath::Hash> _materialsOfMesh;
    std::set<SdfPath> _pendingMeshes;

    std::atomic<bool> _plasticDefault{true};

    mutable std::mutex _mutex;
    std::unordered_map<SdfPath, nova::MaterialId, SdfPath::Hash> _ids;
    std::unordered_map<uint64_t, std::weak_ptr<const nova::shading::Program>> _programs;
    std::set<std::string> _warned;
};

PXR_NAMESPACE_CLOSE_SCOPE
