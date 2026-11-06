#pragma once

#include "core/accel/accel.h"
#include "core/film/crypto.h"
#include "core/lights/light_sampler.h"
#include "core/lights/light_tree.h"
#include "core/lights/lights.h"
#include "core/scene/light.h"
#include "core/scene/hit.h"
#include "core/scene/material.h"
#include "core/scene/mesh.h"
#include "core/scene/ray_limits.h"
#include "core/scene/ray.h"
#include "core/volume/majorant.h"
#include "core/volume/volume.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace nova {

struct MeshRecord {
    MeshDesc desc;
    // Object-to-world transform of every instance; the acceleration structure
    // shares this buffer. A non-instanced mesh has one entry, desc.transform.
    // Empty: nothing is rendered.
    std::vector<Xform34> instances;
    // Moving instances instead: motionSteps uniform keys per instance over the
    // shutter, step-major (motionKeys[s * count + i]); also shared.
    std::vector<SrtTransform> motionKeys;
    uint32_t motionSteps = 1; // > 1: the instances move, motionKeys apply
    // Instance-rate attributes (instancer primvars), one value per instance
    // in the order of the instances; the mesh's own attributes win.
    std::vector<InstanceAttribute> instanceAttributes;
    // Materials: slot per triangle (empty: every triangle uses slot 0), the
    // material of each slot (empty: the default material).
    std::vector<MaterialId> materialSlots;
    bool filtered = false; // a bound material has opacity: hits go through the opacity filter
    RayLimitOverrides rayLimits; // per-object ray limits (core/scene/ray_limits.h)
    std::string lpeTag;          // karma:object:lpetag (empty: the material's tag)
    uint32_t cryptoObject = 0;   // the cryptomatte id of the object's name (0: none)
    std::vector<CryptoAttribute> cryptoAttributes; // string/int primvars for cryptomattes
    bool fakeCaustics = false;   // effective (commit()): shadow rays cross it with its transmittance
    ObjectShading shading;       // effective (commit()): fake-caustic style, internal reflection, ...
    std::vector<uint16_t> triangleSlots;
    // Where each material's primvar reads come from on this mesh (commit()).
    struct MaterialBinding {
        MaterialId material;
        std::vector<AttributeRef> refs; // one per Program::primvars entry
    };
    std::vector<MaterialBinding> bindings;
    bool instanced = false;   // the instances come from setMeshInstances*()
    bool alive = false;
    // Changes with every updateMesh(), unique in the process: devices that copy
    // the geometry (Nova XPU) keep their copy while it stays (0: none yet).
    uint64_t geometryVersion = 0;
    // Light linking (docs/design/M9.md D15): the object's categories and,
    // instanced, each instance's own (added to the object's); commit()
    // turns them into a receiver set and a shadow mask per instance.
    std::vector<std::string> categories;
    std::vector<std::vector<std::string>> instanceCategories;
    std::vector<uint32_t> receiverSets; // per instance (empty: set 0, the unlinked lights only)
    std::vector<uint64_t> shadowMasks; // per instance: the shadow-link categories it is in
    // Emissive triangles as lights (M9 D14), from commit(): per triangle its
    // emitter number (kInvalidId: it does not emit; empty: none does), and
    // instance i's emitters in Scene::lights() from emitterBase + i *
    // emitterStride.
    MeshLightSource lightSource;
    std::vector<uint32_t> emitterOf;
    uint32_t emitterBase = 0, emitterStride = 0;

    uint32_t instanceCount() const {
        return motionSteps > 1 ? uint32_t(motionKeys.size() / motionSteps) : uint32_t(instances.size());
    }
};
static_assert(std::is_nothrow_move_constructible_v<MeshRecord>,
              "growing m_meshes must move records: the accel holds pointers into MeshRecord::instances");

// A volume prim and what commit() derives from it (docs/design/M8.md D7).
struct VolumeRecord {
    VolumeDesc desc;
    // Object-to-world: one transform, or motionSteps uniform keys over the
    // shutter (as meshes).
    Affine3f objectToWorld;
    std::vector<SrtTransform> motionKeys;
    uint32_t motionSteps = 1;
    // Built at commit(): the reference field (density, else the first field
    // the material reads) whose index space the majorant cells divide, its
    // index-to-object map, the cells with the extinction bound of each, and
    // the world bounds over the shutter (lower > upper: nothing to render).
    int32_t reference = -1;
    Affine3f indexToObject;
    MajorantLayout layout;
    std::vector<float> majorant;
    // The extinction bound alone, where emitting cells raised `majorant` to
    // their collision floor (empty otherwise): for rays that collect none
    // of this volume's emission.
    std::vector<float> extinctionMajorant;
    Vec3f lower{1.0f, 1.0f, 1.0f}, upper{-1.0f, -1.0f, -1.0f};
    // The volume shader (D4): the material's Program::volume, or none (the
    // default volume, D6). Per field the map from the volume's object space
    // into its index space; per primvar read of the program
    // (Program::primvarBindings) the field of each component (-1: the read's
    // fallback).
    MaterialId material = kDefaultMaterialId;
    const shading::Program *program = nullptr;
    std::vector<Affine3f> fieldObjectToIndex;
    std::vector<std::array<int32_t, 4>> bindings;
    // Cells whose extinction has no interval bound (D7): a sampled estimate,
    // which the tracker tolerates (D8); the host warns.
    uint32_t estimatedCells = 0;
    // The shader reads Time or Frame: the majorants hold for the frame they
    // were built at (Scene::setShadeFrame) only.
    bool frameDependent = false;
    // The coefficients depend on the ray level (the shader reads it, or
    // Karma's modifiers are on): continuations then gather lights through a
    // transmittance of their own (wavefront.cpp traceMedia).
    bool levelDependent = false;
    // A light source (VolumeDesc::light with emission): its cells by their
    // emission bounds times their world volume, the sum's power (4 pi
    // times), one cell's world volume, and its index in Scene::lights().
    Distribution1D lightCells;
    float lightPower = 0.0f;
    float cellVolume = 0.0f;
    uint32_t lightIndex = kInvalidId;
    // Velocity blur (VolumeDesc::velocity): the fields of the components (-1:
    // none; all three or no blur) and the largest lookup offset, in object
    // units, by which the majorant cells are widened.
    std::array<int32_t, 3> velocity{-1, -1, -1};
    float blurReach = 0.0f;
    bool alive = false;

    bool empty() const { return !(lower.x <= upper.x) || layout.empty(); }
};

// Scene container shared by the host (Hydra) and the renderer.
// Edits may come from several threads at once (Hydra syncs prims in parallel)
// but never while the renderer is running; commit() then pushes them into the
// acceleration structure and the kernel-side light list.
class Scene;

// The opacity of a material at a hit, in [0, 1] (the per-hit opacity program
// with the shade stage's inputs; core/integrator/material_eval.cpp).
// `globals`: the render's frame and fps (null: frame 1 at 24 fps).
float evaluateOpacity(const Scene &scene, MaterialId material, const SurfaceHit &hit, Vec3f dir, float time,
                      uint8_t rayType = 0, uint32_t depth = 0, const ShadeGlobals *globals = nullptr);

// The emission of a material at `count` hits seen from -dirs[i]
// (core/integrator/wavefront.cpp): emissive triangles' power estimates
// (M9 D14).
void evaluateEmission(const Scene &scene, MaterialId material, const SurfaceHit *hits, const Vec3f *dirs, int count,
                      Spectrum *out);

class Scene {
public:
    // `threadLimit` caps the acceleration-structure build threads (0 = all).
    explicit Scene(int threadLimit = 0);
    ~Scene();
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;

    MeshId createMesh();
    // Geometry; an instanced mesh keeps its instances.
    void updateMesh(MeshId id, MeshDesc desc);
    // Single (non-instanced) transform; also leaves instanced mode.
    void updateMeshTransform(MeshId id, const Mat4 &objectToWorld);
    // Single moving transform: uniform steps over the shutter (one step = static).
    void updateMeshMotion(MeshId id, const std::vector<Mat4> &steps);
    // Instanced mesh: one object-to-world transform per instance (the mesh's
    // own transform already applied). The list index is the hit's instance
    // index and the instanceId AOV. An empty list renders nothing.
    void setMeshInstances(MeshId id, std::vector<Xform34> instances);
    // Moving instances: `steps` uniform keys per instance, step-major.
    void setMeshInstanceMotion(MeshId id, std::vector<SrtTransform> keys, uint32_t steps);
    // Instance-rate attributes (empty: none), for names the mesh lacks.
    void setMeshInstanceAttributes(MeshId id, std::vector<InstanceAttribute> attributes);
    void setMeshVisible(MeshId id, bool visible);
    void setMeshUserId(MeshId id, int32_t userId);
    void removeMesh(MeshId id);

    // Materials. Id 0 is the default material and always exists. Editing a
    // material never touches geometry; meshes keep referring to removed
    // materials' ids, which render with the default material.
    MaterialId createMaterial();
    void updateMaterial(MaterialId id, const MaterialDesc &desc);
    void removeMaterial(MaterialId id);
    // The material of every slot of a mesh, and the slot of every triangle
    // (empty: all slot 0; e.g. from GeomSubsets). No acceleration rebuild.
    void setMeshMaterials(MeshId id, std::vector<MaterialId> slots, std::vector<uint16_t> triangleSlots = {});
    // Per-object ray limits (overrides of RenderSettings::limits). No
    // acceleration rebuild.
    void setMeshRayLimits(MeshId id, const RayLimitOverrides &limits);
    // The object's tag in light path expressions (empty: its materials'
    // tags). No acceleration rebuild.
    void setMeshLpeTag(MeshId id, const std::string &tag);
    // The object's cryptomatte id and its string/int primvars' ids. No
    // acceleration rebuild.
    void setMeshCrypto(MeshId id, uint32_t object, std::vector<CryptoAttribute> attributes);
    // Light-linking categories of the object and of each of its instances
    // (docs/design/M9.md D15). No acceleration rebuild.
    void setMeshCategories(MeshId id, std::vector<std::string> categories,
                           std::vector<std::vector<std::string>> instanceCategories = {});
    // Emissive meshes as lights (M9 D14). No acceleration rebuild.
    void setMeshLightSource(MeshId id, const MeshLightSource &settings);
    // The shading-side object properties of objects without their own
    // (fake caustics and their style, internal reflection, the roughness
    // clamp of true caustics; the limits stay with RenderSettings::limits).
    // Applied at commit(); setDefaultFakeCaustics sets the switch alone.
    void setObjectDefaults(const RayLimits &defaults);
    void setDefaultFakeCaustics(bool on);

    LightId createLight();
    void updateLight(LightId id, const LightDesc &desc);
    void removeLight(LightId id);

    // Volumes (docs/design/M8.md). The majorants are built at commit().
    VolumeId createVolume();
    void updateVolume(VolumeId id, VolumeDesc desc);
    // Object-to-world at uniform steps over the shutter (one step = static).
    void updateVolumeMotion(VolumeId id, const std::vector<Mat4> &steps);
    void setVolumeVisible(VolumeId id, bool visible);
    void removeVolume(VolumeId id);
    // The frame being rendered and the frames per second (RenderSettings'):
    // volume shaders that read Time or Frame bound their extinction at it, so
    // a change rebuilds their majorants at the next commit().
    void setShadeFrame(float frame, float fps);
    float shadeFrame() const { return m_shadeFrame; }
    float shadeFps() const { return m_shadeFps; }

    // Applies pending edits. Returns true if anything changed.
    bool commit();

    // Bumped on every edit; the host uses it to restart progressive rendering.
    uint64_t version() const { return m_version.load(); }
    // Number of updateMesh() calls (geometry rebuilds), for IPR tests.
    uint64_t meshUpdateCount() const { return m_meshUpdates.load(); }

    // Interactive sessions refit deformed meshes (docs/design/M7.md D7).
    void setInteractive(bool interactive) { m_accel->setRefit(interactive); }

    // --- Render-time access (valid after commit()) -------------------------
    const IAccel &accel() const { return *m_accel; }
    // Some mesh has opacity (rays then carry filter seeds).
    bool anyFiltered() const { return m_anyFiltered; }
    bool anyFakeCaustics() const { return m_anyFakeCaustics; }

    // The host's texture sampler (not owned; null: texture lookups return
    // their fallback).
    void setTextureSampler(const ITextureSampler *sampler) { m_textureSampler = sampler; }
    const ITextureSampler *textureSampler() const { return m_textureSampler; }
    const MeshRecord &meshRecord(MeshId id) const { return m_meshes[id]; }
    // Ids are below meshCount(); removed ids hold dead records.
    uint32_t meshCount() const { return uint32_t(m_meshes.size()); }
    const std::vector<LightData> &lights() const { return m_lightData; }
    // The lights before the emissive triangles (M9 D14), which come last:
    // what loops over every light need (the triangles are geometry, found by
    // rays and the light tree).
    uint32_t analyticLightCount() const { return m_emitterBegin; }
    // The LPE tag of lights()[i] (an emissive triangle's: its object's or
    // material's).
    const std::string &lightLpeTag(uint32_t i) const;
    // The light of an emissive triangle of a treated mesh (M9 D14), or
    // kInvalidId.
    uint32_t emitterLight(MeshId mesh, uint32_t instance, uint32_t prim) const {
        const MeshRecord &rec = m_meshes[mesh];
        if (prim >= rec.emitterOf.size() || rec.emitterOf[prim] == kInvalidId)
            return kInvalidId;
        return rec.emitterBase + instance * rec.emitterStride + rec.emitterOf[prim];
    }
    bool anyEmitters() const { return m_emitterBegin < m_lightData.size(); }
    // Equiangular sampling in volumes (M9 D17): the lights that opted in,
    // chosen by power (table slot -> light), and each light's probability
    // of being chosen (indexed like lights(), below analyticLightCount()).
    struct EquiangularLights {
        const uint32_t *lights = nullptr;
        LightSamplerView table;
        const float *pmf = nullptr;
    };
    EquiangularLights equiangularLights() const {
        return {m_eqLights.data(), m_eqSampler.view(), m_eqPmf.data()};
    }
    // Power-proportional light selection over lights(); pmf is indexed alike.
    LightSamplerView lightSampler() const { return m_lightSampler.view(); }
    LightTreeView lightTree() const { return m_lightTree.view(); }
    // Light linking (M9 D15): per light its link and per receiver set its
    // categories (null: no linking), an object instance's receiver set, and
    // whether it blocks a shadow-linked light's shadow rays.
    LightLinks lightLinks() const {
        return m_linking ? LightLinks{m_lightLinkMask.data(), m_receiverCategories.data()} : LightLinks{};
    }
    uint32_t receiverSetCount() const { return uint32_t(m_receiverCategories.size()); }
    bool shadowLinking() const { return m_shadowLinking; }
    uint32_t receiverSet(MeshId mesh, uint32_t instance) const {
        const std::vector<uint32_t> &r = m_meshes[mesh].receiverSets;
        return r.empty() ? 0u : r[instance < r.size() ? instance : 0];
    }
    // Shadow-linked dome lights (M9 D15): their indices in lights(). Their
    // BSDF samples are shadow rays of their own (path.h).
    const std::vector<uint32_t> &shadowLinkedDomes() const { return m_shadowDomes; }
    bool castsShadow(MeshId mesh, uint32_t instance, uint16_t category) const {
        if (category >= 64)
            return true;
        const std::vector<uint64_t> &m = m_meshes[mesh].shadowMasks;
        return !m.empty() && ((m[instance < m.size() ? instance : 0] >> category) & 1u) != 0;
    }
    // Radius of the bounding sphere of the visible geometry (1 if empty).
    float sceneRadius() const { return m_sceneRadius; }
    // Float error of hit distances at the scene's largest coordinate
    // (kIntersectionUlps float ulps of it; 0 if empty). Shadow rays stop at
    // least this far short of lights, so a light lying on a large plane is not
    // shadowed by it (shadowRayLength(), lightSearchLimit()).
    float intersectionTolerance() const { return m_intersectionTolerance; }

    // The material a triangle renders with (the default material if none
    // or removed).
    MaterialId materialIdOf(MeshId mesh, uint32_t prim) const {
        const MeshRecord &rec = m_meshes[mesh];
        if (rec.materialSlots.empty())
            return kDefaultMaterialId;
        const uint32_t slot = prim < rec.triangleSlots.size() ? rec.triangleSlots[prim] : 0u;
        const MaterialId id = slot < rec.materialSlots.size() ? rec.materialSlots[slot] : kDefaultMaterialId;
        return id < m_materials.size() && m_materials[id] ? id : kDefaultMaterialId;
    }
    const MaterialRecord &material(MaterialId id) const { return *m_materials[id]; }
    bool hasMaterial(MaterialId id) const { return id < m_materials.size() && m_materials[id].has_value(); }
    const MaterialRecord &materialOf(MeshId mesh, uint32_t prim) const { return material(materialIdOf(mesh, prim)); }
    // The sources of a material's primvars on a mesh (null if it reads none).
    const AttributeRef *attributeRefs(MeshId mesh, MaterialId material) const {
        for (const MeshRecord::MaterialBinding &b : m_meshes[mesh].bindings)
            if (b.material == material)
                return b.refs.data();
        return nullptr;
    }
    // Ids are below materialCount(); removed ids hold no material.
    uint32_t materialCount() const { return uint32_t(m_materials.size()); }
    // Where a primvar of a mesh comes from (None: absent; "a|b" the first
    // present), for the primvar AOVs. Valid until the next commit().
    AttributeRef attributeRef(MeshId mesh, const std::string &names) const;

    // Surface data of a hit; `time` (normalized shutter) must be the ray's
    // time so moving geometry is rebuilt where Embree intersected it. Hits on
    // analytic spheres need the ray (origin, direction); triangles ignore it.
    SurfaceHit resolveHit(MeshId mesh, uint32_t prim, Vec2f uv, float t, uint32_t instance = 0, float time = 0.0f,
                          Vec3f org = {}, Vec3f dir = {}) const;

    // The object-to-world transform of an instance at a ray time.
    Affine3f objectToWorld(MeshId mesh, uint32_t instance, float time) const;

    // Ids are below volumeCount(); removed ids hold dead records.
    uint32_t volumeCount() const { return uint32_t(m_volumes.size()); }
    const VolumeRecord &volumeRecord(VolumeId id) const { return m_volumes[id]; }
    // The visible volumes with something to render, in id order.
    const std::vector<VolumeId> &activeVolumes() const { return m_activeVolumes; }
    // Whether an active volume depends on the ray level (VolumeRecord).
    bool anyLevelDependentVolume() const { return m_levelDependentVolumes; }
    // A volume's object-to-world transform at a ray time.
    Affine3f volumeToWorld(VolumeId id, float time) const;

    // Mean curvature at a hit relative to its Ng (ray cones; 0 for flat
    // shading): from the corner normals, 1/radius on spheres. Computed on
    // demand, it is only needed after specular and glossy scattering.
    float curvature(const SurfaceHit &hit, float time) const;

    // Single-ray queries for tests and tools; the integrator uses batches.
    bool intersect(const Ray &ray, SurfaceHit &hit, float time = 0.0f) const;
    bool occluded(const Ray &ray, float time = 0.0f) const;
    // The closest hit of a world ray (unit direction) on one instance of one
    // mesh alone (subsurface walks): its distance, primitive and Embree
    // barycentrics; false on a miss. Hits closer than tMin are skipped.
    bool intersectLocal(MeshId mesh, uint32_t instance, float time, Vec3f org, Vec3f dir, float tMax, float &t,
                        uint32_t &prim, Vec2f &uv, float tMin = 0.0f) const;

private:
    std::mutex m_mutex;
    std::unique_ptr<IAccel> m_accel;
    const ITextureSampler *m_textureSampler = nullptr;

    std::vector<MeshRecord> m_meshes; // indexed by MeshId
    std::vector<MeshId> m_freeMeshIds;
    std::unordered_set<MeshId> m_dirtyGeometry;
    std::unordered_set<MeshId> m_dirtyInstances;
    std::unordered_set<MeshId> m_dirtyVisibility;
    std::unordered_set<MeshId> m_removed;

    // Indexed by MaterialId; ids are never reused, so a mesh still pointing
    // at a removed material renders the default, not a newer material.
    std::vector<std::optional<MaterialRecord>> m_materials;
    bool m_bindingsDirty = true;

    // Cut-out and stochastic opacity in the accelerator (K10): meshes with a
    // material that has opacity are filtered; a hit counts with the
    // probability of its opacity, decided by a hash of the ray's filter seed
    // and the surface (mesh, prim, instance), so repeated calls agree.
    struct OpacityFilter final : IHitFilter {
        const Scene *scene = nullptr;
        bool accept(const HitCandidate &hit, const RayBatch &rays) const override;
    };
    OpacityFilter m_opacityFilter;
    bool m_anyFiltered = false;
    bool m_anyFakeCaustics = false;
    RayLimits m_objectDefaults; // the shading-side fields (setObjectDefaults)
    void updateFilterFlags(bool &geometryChanged);
    void rebuildBindings();

    std::vector<VolumeRecord> m_volumes; // indexed by VolumeId
    std::vector<VolumeId> m_freeVolumeIds;
    std::unordered_set<VolumeId> m_dirtyVolumes;
    bool m_volumesDirty = false; // added, removed, moved or hidden: the active list and bounds change
    std::vector<VolumeId> m_activeVolumes;
    bool m_levelDependentVolumes = false;
    float m_shadeFrame = 1.0f, m_shadeFps = 24.0f; // setShadeFrame
    void buildVolume(VolumeRecord &rec) const;
    void markVolumesOf(MaterialId material);

    std::vector<std::optional<LightDesc>> m_lights; // indexed by LightId
    std::vector<LightId> m_freeLightIds;
    bool m_lightsDirty = false;
    std::vector<LightData> m_lightData;   // the host's lights, then the volume lights
    std::vector<std::string> m_lightTags; // parallel to m_lightData
    std::vector<std::pair<std::string, std::string>> m_lightLinks; // lightLink, shadowLink; parallel to m_lightData
    std::vector<uint64_t> m_lightLinkMask;      // per light: its link (LightLinks::light)
    std::vector<uint64_t> m_receiverCategories; // per receiver set: its link categories (set 0: none)
    std::vector<uint32_t> m_shadowDomes;        // shadow-linked dome lights
    bool m_linking = false, m_shadowLinking = false, m_linksDirty = false;
    void computeLinks();
    size_t m_hostLightCount = 0;
    // Emissive triangles (M9 D14): lights from m_emitterBegin on; rebuilt
    // when a treated mesh, its materials or the lights before them change.
    uint32_t m_emitterBegin = 0;
    bool m_emittersDirty = false;
    void buildEmitters();
    std::vector<uint32_t> m_eqLights; // equiangular sampling (M9 D17)
    std::vector<float> m_eqPmf;
    LightSampler m_eqSampler;
    LightSampler m_lightSampler;
    LightTree m_lightTree;
    float m_sceneRadius = 1.0f;
    float m_intersectionTolerance = 0.0f;

    // Data derived from light textures, kept while a light uses the texture,
    // so that editing a light does not rescan its image: the sampling
    // distribution of a dome texture and the mean luminance of a rect
    // texture. Holding the image keeps its address from being reused.
    struct TextureData {
        std::shared_ptr<const Image> image;
        std::unique_ptr<Distribution2D> distribution; // domes, built on first use
        float meanLuminance = -1.0f;                  // rects, computed on first use
    };
    std::vector<TextureData> m_textures;

    std::atomic<uint64_t> m_version{0};
    std::atomic<uint64_t> m_meshUpdates{0};
};

} // namespace nova
