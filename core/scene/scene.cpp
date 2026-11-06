#include "core/scene/scene.h"

#include "core/geometry/curves.h"
#include "core/math/frame.h"

#include "core/texture/ray_cone.h"
#include "core/volume/build.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nova {

Scene::Scene(int threadLimit) : m_accel(createEmbreeAccel(threadLimit)) {
    m_materials.emplace_back(makeMaterialRecord(defaultMaterial()));
    m_opacityFilter.scene = this;
    m_accel->setHitFilter(&m_opacityFilter);
}

namespace {

// A light's bounds for the light tree (docs/design/M9.md D11): the box of
// its shape, its power, the cone of its emission normals (w, theta_o) and
// the spread of its emission around them (theta_e). Spheres, cylinders,
// points and volumes emit everywhere; rects and disks along their normal
// with a cosine falloff; shaping cones narrow both.
LightBounds treeBounds(const LightData &l, float power, Vec3f volumeLo, Vec3f volumeHi) {
    LightBounds b;
    b.phi = power;
    b.w = {0.0f, 0.0f, 1.0f};
    b.cosO = -1.0f;
    b.cosE = 0.0f;
    const auto extent = [](Vec3f a, Vec3f c) { // half-extent of a disc/ellipse spanned by a and c
        return Vec3f{std::sqrt(a.x * a.x + c.x * c.x), std::sqrt(a.y * a.y + c.y * c.y), std::sqrt(a.z * a.z + c.z * c.z)};
    };
    switch (l.type) {
    case LightType::Sphere: {
        const Vec3f r{l.radius, l.radius, l.radius};
        b.lo = l.position - r;
        b.hi = l.position + r;
        break;
    }
    case LightType::Rect: {
        const Vec3f e = absolute(l.axisU) + absolute(l.axisV);
        b.lo = l.position - e;
        b.hi = l.position + e;
        b.w = l.normal;
        b.cosO = 1.0f;
        b.twoSided = l.twoSided;
        break;
    }
    case LightType::Disk: {
        const Vec3f e = extent(l.axisU, l.axisV);
        b.lo = l.position - e;
        b.hi = l.position + e;
        b.w = l.normal;
        b.cosO = 1.0f;
        b.twoSided = l.twoSided;
        break;
    }
    case LightType::Cylinder: {
        const Vec3f e = absolute(l.axisU) + extent(l.axisV, l.axisW);
        b.lo = l.position - e;
        b.hi = l.position + e;
        break;
    }
    case LightType::Volume:
        b.lo = volumeLo;
        b.hi = volumeHi;
        break;
    case LightType::Triangle: { // two-sided, cosine falloff about the face normal (M9 D14)
        const Vec3f a = l.position, c = l.position + l.axisU, d = l.position + l.axisV;
        b.lo = {std::min({a.x, c.x, d.x}), std::min({a.y, c.y, d.y}), std::min({a.z, c.z, d.z})};
        b.hi = {std::max({a.x, c.x, d.x}), std::max({a.y, c.y, d.y}), std::max({a.z, c.z, d.z})};
        b.w = l.normal;
        b.cosO = 1.0f;
        b.twoSided = true;
        return b;
    }
    default:
        b.phi = 0.0f; // not a bounded light
        return b;
    }
    b.crossable = isAreaLight(l.type) && !l.delta; // rays collect them by crossing (D13)
    if (l.shaped && l.cosConeCut > -1.0f) {
        if (l.type == LightType::Rect || l.type == LightType::Disk) {
            b.cosE = nova::fmax(l.cosConeCut, 0.0f); // emission within the cone of the normal
        } else if (l.cosConeCut >= 0.0f) {
            // Points and spheres: full emission within the cone's start,
            // falling off to its cut. Cones wider than 90 degrees keep the
            // unshaped bounds: lightImportance() bounds the emission spread
            // by a positive cosine (and a planar node by its plane), so it
            // would drop what such a cone emits behind the light.
            const float start = l.cosConeStart > l.cosConeCut ? l.cosConeStart : l.cosConeCut;
            b.w = l.axis;
            b.cosO = start;
            b.cosE = std::cos(nova::fmax(std::acos(nova::fmin(nova::fmax(l.cosConeCut, -1.0f), 1.0f)) -
                                             std::acos(nova::fmin(start, 1.0f)),
                                         1e-3f));
        }
    }
    return b;
}

uint32_t hashMix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

} // namespace

bool Scene::OpacityFilter::accept(const HitCandidate &c, const RayBatch &rays) const {
    const MeshRecord &mesh = scene->meshRecord(c.mesh);
    const uint8_t raw = rays.rayType ? rays.rayType[c.ray] : 0;
    // Ray visibility (core/scene/visibility.h); occlusion queries are shadow rays.
    if (!visibleTo(mesh.visibility, c.occlusion ? uint8_t(RayClass::Shadow) : uint8_t(raw & kRayClassBits)))
        return false;
    // Shadow rays from background-holdout points (docs/design/M11-P2.md):
    // other background holdouts do not shadow them; the bgoccludeshadow
    // probe sees nothing but background holdouts.
    if (raw & kRayProbeBackground) {
        if (mesh.holdout != HoldoutMode::Background)
            return false;
    } else if ((raw & kRayFromBackground) && mesh.holdout == HoldoutMode::Background) {
        return false;
    }
    // Fake caustics: shadow rays ignore the object here and multiply its
    // transmittance afterwards (docs/design/M4.md 3.3); ambient occlusion
    // has no such phase, the object blocks it by its opacity.
    if (c.occlusion && rays.skipFakeCaustics && mesh.fakeCaustics)
        return false;
    // Shadow linking (M9 D15): only the objects in the light's shadow set block it.
    if (c.occlusion && rays.shadowCategory && rays.shadowCategory[c.ray] != 0xFFFFu &&
        !scene->castsShadow(c.mesh, c.instance, rays.shadowCategory[c.ray]))
        return false;
    const MaterialId id = scene->materialIdOf(c.mesh, c.prim);
    const MaterialRecord &rec = scene->material(id);
    if (!rec.hasOpacity)
        return true;
    float o = rec.uniformOpacity;
    if (!rec.opacityUniforms.empty() || rec.externalOpacity) {
        const float time = rays.time ? rays.time[c.ray] : 0.0f;
        const SurfaceHit hit =
            scene->resolveHit(c.mesh, c.prim, c.uv, c.t, c.instance, time, rays.org[c.ray], rays.dir[c.ray]);
        const uint8_t type = c.occlusion ? uint8_t(RayClass::Shadow) : uint8_t(raw & kRayClassBits);
        o = evaluateOpacity(*scene, id, hit, rays.dir[c.ray], time, type, rays.depth, rays.globals);
    }
    if (o >= 1.0f)
        return true;
    if (!(o > 0.0f))
        return false;
    // One uniform number per (ray, surface): the ray's seed decides, the
    // surface ids decorrelate the surfaces along the ray.
    const float seed = rays.filterSeed ? rays.filterSeed[c.ray] : 0.5f;
    uint32_t seedBits;
    std::memcpy(&seedBits, &seed, sizeof(seedBits));
    const uint32_t h = hashMix(seedBits ^ hashMix(c.mesh * 0x9E3779B1u + hashMix(c.prim ^ hashMix(c.instance + 0x632BE5ABu))));
    return float(h >> 8) * (1.0f / 16777216.0f) < o;
}

void Scene::updateFilterFlags(bool &geometryChanged) {
    m_anyFiltered = false;
    m_anyFakeCaustics = false;
    // Ray visibility and holdouts (core/scene/visibility.h): the effective
    // settings; a partial mask or a background holdout (which shadow rays
    // from background points cross) goes through the filter, and with
    // bgoccludeshadow every object does (the probe of background occluders
    // must see only them, docs/design/M11-P2.md).
    m_anyHoldouts = false;
    m_shadowVisibilityDiffers = false;
    bool anyBackground = false;
    for (MeshRecord &rec : m_meshes) {
        if (!rec.alive)
            continue;
        const ObjectVisibility &own = rec.visibilitySettings;
        const int mask = own.mask >= 0 ? own.mask : m_defaultVisibility.mask;
        const int holdout = own.holdout >= 0 ? own.holdout : m_defaultVisibility.holdout;
        rec.visibility = mask >= 0 ? uint8_t(mask & kVisibleAll) : kVisibleAll;
        rec.holdout = holdout > 0 && holdout <= int(HoldoutMode::Background) ? HoldoutMode(holdout) : HoldoutMode::None;
        m_anyHoldouts |= rec.holdout != HoldoutMode::None;
        anyBackground |= rec.holdout == HoldoutMode::Background;
        // Visible to the shadow rays but not to some scattered ray, or the
        // other way round (light collection, Scene::shadowVisibilityDiffers).
        const bool shadow = visibleTo(rec.visibility, uint8_t(RayClass::Shadow));
        for (RayClass c : {RayClass::Diffuse, RayClass::Reflect, RayClass::Refract, RayClass::Sss, RayClass::Volume})
            m_shadowVisibilityDiffers |= visibleTo(rec.visibility, uint8_t(c)) != shadow;
    }
    m_anyBackground = anyBackground;
    const bool filterAll = anyBackground && m_bgOccludeShadow;
    for (MeshId id = 0; id < m_meshes.size(); ++id) {
        MeshRecord &rec = m_meshes[id];
        if (!rec.alive)
            continue;
        bool want = rec.visibility != kVisibleAll || rec.holdout == HoldoutMode::Background || filterAll;
        if (rec.materialSlots.empty()) {
            want |= material(kDefaultMaterialId).hasOpacity;
        } else {
            for (MaterialId m : rec.materialSlots)
                want |= material(m).hasOpacity;
        }
        rec.fakeCaustics = rec.rayLimits.fakeCaustics >= 0 ? rec.rayLimits.fakeCaustics != 0 : m_defaultFakeCaustics;
        want |= rec.fakeCaustics || m_shadowLinking;
        m_anyFakeCaustics |= rec.fakeCaustics;
        if (want != rec.filtered) {
            rec.filtered = want;
            m_accel->setMeshFiltered(id, want);
            geometryChanged = true;
        }
        m_anyFiltered |= want;
    }
}
Scene::~Scene() = default;

MeshId Scene::createMesh() {
    std::lock_guard lock(m_mutex);
    MeshId id;
    if (!m_freeMeshIds.empty()) {
        id = m_freeMeshIds.back();
        m_freeMeshIds.pop_back();
    } else {
        id = MeshId(m_meshes.size());
        m_meshes.emplace_back();
    }
    m_meshes[id] = MeshRecord{};
    m_meshes[id].alive = true;
    return id;
}

void Scene::updateMesh(MeshId id, MeshDesc desc) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive) return;
    MeshRecord &rec = m_meshes[id];
    rec.desc = std::move(desc);
    if (!rec.instanced && rec.motionSteps <= 1) // instances and motion survive geometry edits
        rec.instances.assign(1, toXform34(rec.desc.transform));
    m_dirtyGeometry.insert(id);
    m_bindingsDirty = true;
    static std::atomic<uint64_t> geometryVersions{0};
    rec.geometryVersion = ++geometryVersions;
    ++m_meshUpdates;
    ++m_version;
}

void Scene::updateMeshTransform(MeshId id, const Mat4 &objectToWorld) {
    updateMeshMotion(id, {objectToWorld});
}

void Scene::updateMeshMotion(MeshId id, const std::vector<Mat4> &steps) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive || steps.empty()) return;
    MeshRecord &rec = m_meshes[id];
    rec.desc.transform = steps[steps.size() / 2];
    rec.instanced = false;
    if (!rec.instanceAttributes.empty()) {
        rec.instanceAttributes.clear();
        m_bindingsDirty = true;
    }
    rec.motionKeys.clear();
    rec.motionSteps = 1;
    rec.instances.assign(1, toXform34(steps.front()));
    if (steps.size() > 1) {
        rec.motionSteps = uint32_t(std::min<size_t>(steps.size(), kMaxMotionSteps));
        rec.motionKeys.resize(rec.motionSteps);
        for (uint32_t s = 0; s < rec.motionSteps; ++s)
            rec.motionKeys[s] = decomposeSrt(steps[s]);
        alignQuaternions(rec.motionKeys.data(), rec.motionSteps);
    }
    m_dirtyInstances.insert(id);
    ++m_version;
}

void Scene::setMeshInstances(MeshId id, std::vector<Xform34> instances) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive) return;
    MeshRecord &rec = m_meshes[id];
    rec.instanced = true;
    rec.motionKeys.clear();
    rec.motionSteps = 1;
    rec.instances = std::move(instances); // the old buffer is unused until commit() rebinds
    m_dirtyInstances.insert(id);
    ++m_version;
}

void Scene::setMeshInstanceMotion(MeshId id, std::vector<SrtTransform> keys, uint32_t steps) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive) return;
    MeshRecord &rec = m_meshes[id];
    rec.instanced = true;
    rec.instances.clear();
    rec.motionSteps = steps > 1 && keys.size() % steps == 0 ? steps : 1;
    if (rec.motionSteps > 1) {
        rec.motionKeys = std::move(keys);
        const uint32_t count = uint32_t(rec.motionKeys.size() / rec.motionSteps);
        for (uint32_t i = 0; i < count; ++i)
            alignQuaternions(rec.motionKeys.data() + i, rec.motionSteps, count);
    } else {
        rec.motionKeys.clear(); // malformed: render nothing rather than garbage
    }
    m_dirtyInstances.insert(id);
    ++m_version;
}

void Scene::setMeshInstanceAttributes(MeshId id, std::vector<InstanceAttribute> attributes) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive) return;
    if (attributes.empty() && m_meshes[id].instanceAttributes.empty()) return;
    m_meshes[id].instanceAttributes = std::move(attributes); // shading data only: no acceleration-structure change
    m_bindingsDirty = true;
    ++m_version;
}

void Scene::setMeshVisible(MeshId id, bool visible) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive || m_meshes[id].desc.visible == visible) return;
    m_meshes[id].desc.visible = visible;
    m_dirtyVisibility.insert(id);
    ++m_version;
}

void Scene::setMeshUserId(MeshId id, int32_t userId) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive || m_meshes[id].desc.userId == userId) return;
    m_meshes[id].desc.userId = userId; // no acceleration-structure change
    ++m_version;
}

void Scene::removeMesh(MeshId id) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive) return;
    m_meshes[id] = MeshRecord{}; // releases the geometry, alive = false
    m_dirtyGeometry.erase(id);
    m_dirtyInstances.erase(id);
    m_dirtyVisibility.erase(id);
    m_removed.insert(id); // the id is recycled once the accel has dropped it
    ++m_version;
}

MaterialId Scene::createMaterial() {
    std::lock_guard lock(m_mutex);
    m_materials.emplace_back(makeMaterialRecord(MaterialDesc{}));
    return MaterialId(m_materials.size() - 1);
}

void Scene::updateMaterial(MaterialId id, const MaterialDesc &desc) {
    MaterialRecord record = makeMaterialRecord(desc); // the prologue runs outside the lock
    std::lock_guard lock(m_mutex);
    if (id >= m_materials.size())
        return;
    const bool sameProgram = m_materials[id] && m_materials[id]->desc.program == desc.program;
    m_materials[id] = std::move(record);
    m_bindingsDirty |= !sameProgram; // a value edit keeps the bindings
    markVolumesOf(id); // their majorants depend on the values too
    m_emittersDirty = true; // and emissive triangles their emission (M9 D14)
    ++m_version;
}

void Scene::removeMaterial(MaterialId id) {
    std::lock_guard lock(m_mutex);
    if (id == kDefaultMaterialId || id >= m_materials.size() || !m_materials[id])
        return;
    m_materials[id].reset();
    m_bindingsDirty = true;
    markVolumesOf(id);
    m_emittersDirty = true;
    ++m_version;
}

void Scene::markVolumesOf(MaterialId id) {
    for (VolumeId v = 0; v < m_volumes.size(); ++v)
        if (m_volumes[v].alive && m_volumes[v].desc.material == id) {
            m_dirtyVolumes.insert(v);
            m_volumesDirty = true;
        }
}

void Scene::setMeshMaterials(MeshId id, std::vector<MaterialId> slots, std::vector<uint16_t> triangleSlots) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive)
        return;
    m_meshes[id].materialSlots = std::move(slots);
    m_meshes[id].triangleSlots = std::move(triangleSlots);
    m_bindingsDirty = true;
    ++m_version;
}

void Scene::setMeshRayLimits(MeshId id, const RayLimitOverrides &limits) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive || m_meshes[id].rayLimits == limits)
        return;
    m_meshes[id].rayLimits = limits;
    ++m_version;
}

void Scene::setMeshCrypto(MeshId id, uint32_t object, std::vector<CryptoAttribute> attributes) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive)
        return;
    MeshRecord &rec = m_meshes[id];
    rec.cryptoObject = object;
    rec.cryptoAttributes = std::move(attributes);
    ++m_version;
}

void Scene::setMeshLpeTag(MeshId id, const std::string &tag) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive || m_meshes[id].lpeTag == tag)
        return;
    m_meshes[id].lpeTag = tag;
    ++m_version;
}

void Scene::setDefaultFakeCaustics(bool on) {
    std::lock_guard lock(m_mutex);
    if (m_defaultFakeCaustics == on)
        return;
    m_defaultFakeCaustics = on;
    ++m_version;
}

void Scene::setMeshVisibility(MeshId id, const ObjectVisibility &visibility) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive || m_meshes[id].visibilitySettings == visibility)
        return;
    m_meshes[id].visibilitySettings = visibility;
    ++m_version;
}

void Scene::setDefaultVisibility(const ObjectVisibility &visibility) {
    std::lock_guard lock(m_mutex);
    if (m_defaultVisibility == visibility)
        return;
    m_defaultVisibility = visibility;
    ++m_version;
}

void Scene::setBackgroundOccludesShadows(bool on) {
    std::lock_guard lock(m_mutex);
    if (m_bgOccludeShadow == on)
        return;
    m_bgOccludeShadow = on;
    ++m_version;
}

namespace {

// The source of one primvar name on a mesh (None: absent).
AttributeRef bindName(const MeshRecord &rec, const std::string &name) {
    AttributeRef ref;
    if (const MeshAttribute *a = rec.desc.findAttribute(name)) {
        ref.data = a->data.data();
        ref.width = a->width;
        switch (a->rate) {
        case AttributeRate::Constant:
            ref.source = AttributeSource::Constant;
            break;
        case AttributeRate::Uniform:
            ref.source = AttributeSource::Uniform;
            break;
        case AttributeRate::Vertex:
            ref.source = AttributeSource::Vertex;
            break;
        default:
            ref.source = AttributeSource::Corner;
            break;
        }
        // Too little data for the mesh: as if absent. Curves read per-segment
        // data only (prepareCurves()).
        const size_t corners = rec.desc.isCurves() ? 3 * rec.desc.primitiveCount() : rec.desc.indices.size();
        const size_t need = a->rate == AttributeRate::Constant  ? 1
                            : a->rate == AttributeRate::Uniform ? rec.desc.primitiveCount()
                            : a->rate == AttributeRate::Vertex  ? rec.desc.pointCount()
                                                                : corners;
        if (a->width < 1 || a->width > 4 || a->data.size() < need * a->width || a->rate == AttributeRate::Varying ||
            (rec.desc.isSphere() && a->rate != AttributeRate::Constant && a->rate != AttributeRate::Uniform) ||
            (rec.desc.isCurves() && a->rate == AttributeRate::Vertex) ||
            (rec.desc.isPoints() && a->rate != AttributeRate::Constant && a->rate != AttributeRate::Uniform))
            ref = AttributeRef{};
    }
    if (ref.source == AttributeSource::None && rec.desc.isSphere() && name == "st") {
        ref.source = AttributeSource::Parametric; // the sphere's own parameterization
        ref.width = 2;
    }
    if (ref.source == AttributeSource::None) {
        for (const InstanceAttribute &a : rec.instanceAttributes)
            if (a.name == name && a.width >= 1 && a.width <= 4 &&
                a.data.size() >= size_t(rec.instanceCount()) * a.width) {
                ref.data = a.data.data();
                ref.width = a.width;
                ref.source = AttributeSource::Instance;
                break;
            }
    }
    return ref;
}

// "a|b": the first name the mesh has.
AttributeRef bindNames(const MeshRecord &rec, const std::string &names) {
    AttributeRef ref;
    for (size_t begin = 0; begin <= names.size() && ref.source == AttributeSource::None;) {
        const size_t bar = std::min(names.find('|', begin), names.size());
        ref = bindName(rec, names.substr(begin, bar - begin));
        begin = bar + 1;
    }
    return ref;
}

} // namespace

AttributeRef Scene::attributeRef(MeshId mesh, const std::string &names) const {
    return mesh < m_meshes.size() && m_meshes[mesh].alive ? bindNames(m_meshes[mesh], names) : AttributeRef{};
}

void Scene::rebuildBindings() {
    std::vector<MaterialId> used;
    for (MeshRecord &rec : m_meshes) {
        rec.bindings.clear();
        if (!rec.alive)
            continue;
        used.clear();
        if (rec.materialSlots.empty())
            used.push_back(kDefaultMaterialId);
        for (MaterialId id : rec.materialSlots) {
            const MaterialId valid = id < m_materials.size() && m_materials[id] ? id : kDefaultMaterialId;
            if (std::find(used.begin(), used.end(), valid) == used.end())
                used.push_back(valid);
        }
        for (MaterialId id : used) {
            const shading::Program *program = m_materials[id]->desc.program.get();
            if (!program || program->primvars.empty())
                continue;
            MeshRecord::MaterialBinding binding;
            binding.material = id;
            for (const std::string &names : program->primvars) // Houdini's Cd, else displayColor (M5.md 3.3)
                binding.refs.push_back(bindNames(rec, names));
            rec.bindings.push_back(std::move(binding));
        }
    }
    m_bindingsDirty = false;
}

LightId Scene::createLight() {
    std::lock_guard lock(m_mutex);
    LightId id;
    if (!m_freeLightIds.empty()) {
        id = m_freeLightIds.back();
        m_freeLightIds.pop_back();
    } else {
        id = LightId(m_lights.size());
        m_lights.emplace_back();
    }
    LightDesc desc;
    desc.visible = false; // until the first update arrives
    m_lights[id] = desc;
    m_lightsDirty = true;
    return id;
}

void Scene::updateLight(LightId id, const LightDesc &desc) {
    std::lock_guard lock(m_mutex);
    if (id >= m_lights.size() || !m_lights[id]) return;
    m_lights[id] = desc;
    m_lightsDirty = true;
    ++m_version;
}

void Scene::removeLight(LightId id) {
    std::lock_guard lock(m_mutex);
    if (id >= m_lights.size() || !m_lights[id]) return;
    m_lights[id].reset();
    m_freeLightIds.push_back(id);
    m_lightsDirty = true;
    ++m_version;
}

VolumeId Scene::createVolume() {
    std::lock_guard lock(m_mutex);
    VolumeId id;
    if (!m_freeVolumeIds.empty()) {
        id = m_freeVolumeIds.back();
        m_freeVolumeIds.pop_back();
    } else {
        id = VolumeId(m_volumes.size());
        m_volumes.emplace_back();
    }
    m_volumes[id] = VolumeRecord{};
    m_volumes[id].alive = true;
    m_dirtyVolumes.insert(id);
    m_volumesDirty = true;
    ++m_version;
    return id;
}

void Scene::updateVolume(VolumeId id, VolumeDesc desc) {
    std::lock_guard lock(m_mutex);
    if (id >= m_volumes.size() || !m_volumes[id].alive)
        return;
    m_volumes[id].desc = std::move(desc);
    m_dirtyVolumes.insert(id);
    m_volumesDirty = true;
    ++m_version;
}

void Scene::updateVolumeMotion(VolumeId id, const std::vector<Mat4> &steps) {
    std::lock_guard lock(m_mutex);
    if (id >= m_volumes.size() || !m_volumes[id].alive || steps.empty())
        return;
    VolumeRecord &rec = m_volumes[id];
    rec.objectToWorld = srtToAffine(decomposeSrt(steps.front()));
    rec.motionKeys.clear();
    rec.motionSteps = 1;
    if (steps.size() == 1) {
        const Xform34 x = toXform34(steps.front()); // exact, not through the decomposition
        rec.objectToWorld.x = {x.c[0][0], x.c[0][1], x.c[0][2]};
        rec.objectToWorld.y = {x.c[1][0], x.c[1][1], x.c[1][2]};
        rec.objectToWorld.z = {x.c[2][0], x.c[2][1], x.c[2][2]};
        rec.objectToWorld.t = {x.c[3][0], x.c[3][1], x.c[3][2]};
    } else {
        rec.motionSteps = uint32_t(std::min<size_t>(steps.size(), kMaxMotionSteps));
        rec.motionKeys.resize(rec.motionSteps);
        for (uint32_t s = 0; s < rec.motionSteps; ++s)
            rec.motionKeys[s] = decomposeSrt(steps[s]);
        alignQuaternions(rec.motionKeys.data(), rec.motionSteps);
    }
    m_dirtyVolumes.insert(id); // the world bounds change
    m_volumesDirty = true;
    ++m_version;
}

void Scene::setVolumeVisible(VolumeId id, bool visible) {
    std::lock_guard lock(m_mutex);
    if (id >= m_volumes.size() || !m_volumes[id].alive || m_volumes[id].desc.visible == visible)
        return;
    m_volumes[id].desc.visible = visible;
    m_volumesDirty = true;
    ++m_version;
}

void Scene::setShadeFrame(float frame, float fps) {
    std::lock_guard lock(m_mutex);
    if (m_shadeFrame == frame && m_shadeFps == fps)
        return;
    m_shadeFrame = frame;
    m_shadeFps = fps;
    // Only the volumes whose shaders read the frame or the time change.
    bool changed = false;
    for (VolumeId v = 0; v < m_volumes.size(); ++v)
        if (m_volumes[v].alive && m_volumes[v].frameDependent) {
            m_dirtyVolumes.insert(v);
            changed = true;
        }
    if (changed) {
        m_volumesDirty = true;
        ++m_version;
    }
}

void Scene::removeVolume(VolumeId id) {
    std::lock_guard lock(m_mutex);
    if (id >= m_volumes.size() || !m_volumes[id].alive)
        return;
    m_volumes[id] = VolumeRecord{};
    m_dirtyVolumes.erase(id);
    m_freeVolumeIds.push_back(id);
    m_volumesDirty = true;
    ++m_version;
}

// The majorant cells and world bounds of a volume (docs/design/M8.md D5-D7,
// core/volume/build.h): with its material's volume shader, if it has one.
void Scene::buildVolume(VolumeRecord &rec) const {
    const MaterialId m = rec.desc.material;
    const MaterialRecord *material =
        m < m_materials.size() && m_materials[m] && m_materials[m]->hasVolume() ? &*m_materials[m] : nullptr;
    buildVolumeRecord(rec, material, m_shadeFrame, m_shadeFps);
}

Affine3f Scene::volumeToWorld(VolumeId id, float time) const {
    const VolumeRecord &rec = m_volumes[id];
    if (rec.motionSteps > 1)
        return srtAtTime(rec.motionKeys.data(), rec.motionSteps, 1, time);
    return rec.objectToWorld;
}

void Scene::setMeshCategories(MeshId id, std::vector<std::string> categories,
                              std::vector<std::vector<std::string>> instanceCategories) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive)
        return;
    MeshRecord &rec = m_meshes[id];
    if (rec.categories == categories && rec.instanceCategories == instanceCategories)
        return;
    rec.categories = std::move(categories);
    rec.instanceCategories = std::move(instanceCategories);
    m_linksDirty = true;
    ++m_version;
}

// Light linking (docs/design/M9.md D15, after Cycles): the categories lights
// link to get bits; every object instance's bits form its receiver set
// (distinct combinations numbered, 0 = none, as many as there are); per
// light the bit of its category (kLinksEveryone if its link is empty), so a
// light lights a set exactly when the set holds its category
// (light_tree.h). Shadow-link categories number separately; each instance
// records which it is in. At most 64 categories of each.
void Scene::computeLinks() {
    m_linksDirty = false;
    std::unordered_map<std::string, int> linkCats, shadowCats;
    for (const auto &[link, shadow] : m_lightLinks) {
        if (!link.empty() && !linkCats.count(link))
            linkCats.emplace(link, int(linkCats.size()));
        if (!shadow.empty() && !shadowCats.count(shadow))
            shadowCats.emplace(shadow, int(shadowCats.size()));
    }
    m_shadowDomes.clear();
    for (size_t i = 0; i < m_lightLinks.size(); ++i) { // emissive triangles (after them) link everything
        const auto it = shadowCats.find(m_lightLinks[i].second);
        m_lightData[i].shadowCategory = it != shadowCats.end() && it->second < 64 ? uint16_t(it->second) : 0xFFFFu;
        if (m_lightData[i].type == LightType::Dome && m_lightData[i].shadowLinked() && m_lightData[i].illuminates())
            m_shadowDomes.push_back(uint32_t(i));
    }
    if (linkCats.size() > 64 || shadowCats.size() > 64)
        std::fprintf(stderr, "[Nova] more than 64 light-link or shadow-link categories: the rest are ignored\n");

    // Set 0 is the instances without categories (only unlinked lights reach
    // it).
    m_receiverCategories.assign(1, 0u); // receiver set -> its link-category bits
    std::unordered_map<uint64_t, uint32_t> setOf{{0, 0}};
    for (MeshRecord &rec : m_meshes) {
        rec.receiverSets.clear();
        rec.shadowMasks.clear();
        if (!rec.alive || (linkCats.empty() && shadowCats.empty()) ||
            (rec.categories.empty() && rec.instanceCategories.empty()))
            continue;
        const uint32_t n = std::max(rec.instanceCount(), 1u);
        const bool perInstance = !rec.instanceCategories.empty();
        if (!linkCats.empty())
            rec.receiverSets.assign(perInstance ? n : 1, 0);
        if (!shadowCats.empty())
            rec.shadowMasks.assign(perInstance ? n : 1, 0);
        for (uint32_t k = 0; k < (perInstance ? n : 1u); ++k) {
            uint64_t link = 0, shadow = 0;
            const auto add = [&](const std::string &c) {
                if (const auto it = linkCats.find(c); it != linkCats.end() && it->second < 64)
                    link |= uint64_t(1) << it->second;
                if (const auto it = shadowCats.find(c); it != shadowCats.end() && it->second < 64)
                    shadow |= uint64_t(1) << it->second;
            };
            for (const std::string &c : rec.categories)
                add(c);
            if (perInstance && k < rec.instanceCategories.size())
                for (const std::string &c : rec.instanceCategories[k])
                    add(c);
            if (!rec.shadowMasks.empty())
                rec.shadowMasks[k] = shadow;
            if (rec.receiverSets.empty())
                continue;
            const auto [it, added] = setOf.try_emplace(link, uint32_t(m_receiverCategories.size()));
            if (added)
                m_receiverCategories.push_back(link);
            rec.receiverSets[k] = it->second;
        }
    }
    m_linking = !linkCats.empty();
    m_lightLinkMask.assign(m_lightData.size(), kLinksEveryone);
    if (m_linking)
        for (size_t i = 0; i < m_lightLinks.size(); ++i) {
            const auto it = linkCats.find(m_lightLinks[i].first);
            if (m_lightLinks[i].first.empty() || it == linkCats.end() || it->second >= 64)
                continue; // lights everything
            m_lightLinkMask[i] = uint64_t(1) << it->second;
        }
}

bool Scene::commit() {
    std::lock_guard lock(m_mutex);
    bool geometryChanged = false;

    for (MeshId id : m_removed) {
        m_accel->removeMesh(id);
        m_freeMeshIds.push_back(id);
        geometryChanged = true;
    }
    m_removed.clear();

    // Order: triangles (BLAS), then instance arrays, then visibility and the
    // top-level commit.
    for (MeshId id : m_dirtyGeometry) {
        m_accel->setMesh(id, m_meshes[id].desc);
        m_dirtyInstances.insert(id);  // a new mesh gets its instance array here
        m_dirtyVisibility.insert(id); // and the array its visibility
        geometryChanged = true;
    }
    m_dirtyGeometry.clear();

    for (MeshId id : m_dirtyInstances) {
        const MeshRecord &rec = m_meshes[id];
        if (rec.motionSteps > 1)
            m_accel->setInstanceMotion(id, rec.motionKeys.data(), rec.instanceCount(), rec.motionSteps);
        else
            m_accel->setInstances(id, rec.instances.data(), uint32_t(rec.instances.size()));
        geometryChanged = true;
    }
    m_dirtyInstances.clear();

    for (MeshId id : m_dirtyVisibility) {
        m_accel->setEnabled(id, m_meshes[id].desc.visible);
        geometryChanged = true;
    }
    m_dirtyVisibility.clear();

    // Shadow linking filters every object's occlusion hits (M9 D15).
    {
        bool shadowLinking = false;
        for (const std::optional<LightDesc> &light : m_lights)
            shadowLinking |= light && light->visible && !light->shadowLink.empty();
        if (shadowLinking != m_shadowLinking)
            m_linksDirty = true;
        m_shadowLinking = shadowLinking;
    }
    updateFilterFlags(geometryChanged);
    if (geometryChanged)
        m_accel->commit();

    const bool bindingsChanged = m_bindingsDirty;
    if (m_bindingsDirty)
        rebuildBindings();

    const bool lightsChanged = m_lightsDirty;
    // Emissive triangles (M9 D14) whose meshes, materials and settings did
    // not change survive an edit of the other lights as they are.
    const bool emitterInputsChanged = m_emittersDirty || geometryChanged || bindingsChanged;
    std::vector<LightData> keptEmitters;
    const uint32_t keptBegin = m_emitterBegin;
    if (!emitterInputsChanged && (lightsChanged || m_volumesDirty || !m_dirtyVolumes.empty()) &&
        m_emitterBegin < m_lightData.size())
        keptEmitters.assign(m_lightData.begin() + m_emitterBegin, m_lightData.end());
    if (m_lightsDirty) {
        std::vector<TextureData> textures; // the textures still in use, with their cached data
        auto textureData = [&](const std::shared_ptr<const Image> &image) -> TextureData & {
            for (TextureData &t : textures)
                if (t.image == image)
                    return t;
            TextureData entry{image, nullptr, -1.0f};
            for (TextureData &t : m_textures) {
                if (t.image == image) {
                    entry.distribution = std::move(t.distribution);
                    entry.meanLuminance = t.meanLuminance;
                }
            }
            textures.push_back(std::move(entry));
            return textures.back();
        };

        m_lightData.clear();
        m_lightTags.clear();
        m_lightLinks.clear();
        for (const std::optional<LightDesc> &light : m_lights) { // id order keeps sampling deterministic
            if (!light || !light->visible)
                continue;
            LightData data = makeLightData(*light);
            if (data.texels && (data.type == LightType::Dome || data.type == LightType::Rect)) {
                TextureData &t = textureData(light->texture);
                if (data.type == LightType::Dome) {
                    if (!t.distribution) {
                        t.distribution = std::make_unique<Distribution2D>();
                        buildDomeDistribution(*t.image, *t.distribution);
                    }
                    data.distribution = t.distribution->view(); // stable: the distribution lives on the heap
                } else {
                    if (t.meanLuminance < 0.0f)
                        t.meanLuminance = textureMeanLuminance(*t.image);
                    data.texMeanLuminance = t.meanLuminance;
                }
            }
            m_lightData.push_back(data);
            m_lightTags.push_back(light->lpeTag);
            m_lightLinks.emplace_back(light->lightLink, light->shadowLink);
        }
        m_hostLightCount = m_lightData.size();
        m_textures = std::move(textures);
        m_lightsDirty = false;
    }

    // Volumes: majorants and bounds of the edited ones (in parallel), then
    // the list the integrator walks.
    const bool volumesChanged = m_volumesDirty || !m_dirtyVolumes.empty();
    if (!m_dirtyVolumes.empty()) {
        std::vector<VolumeId> ids(m_dirtyVolumes.begin(), m_dirtyVolumes.end());
        tbb::parallel_for(size_t(0), ids.size(), [&](size_t i) { buildVolume(m_volumes[ids[i]]); });
        m_dirtyVolumes.clear();
    }
    if (m_volumesDirty) {
        m_activeVolumes.clear();
        for (VolumeId id = 0; id < m_volumes.size(); ++id)
            if (m_volumes[id].alive && m_volumes[id].desc.visible && !m_volumes[id].empty())
                m_activeVolumes.push_back(id);
        m_volumesDirty = false;
    }
    if (volumesChanged) {
        m_levelDependentVolumes = false;
        for (VolumeId id : m_activeVolumes)
            m_levelDependentVolumes |= m_volumes[id].levelDependent;
    }
    // Emissive volumes that are light sources follow the host's lights
    // (docs/design/M8.md D13).
    if (lightsChanged || volumesChanged) {
        m_lightData.resize(m_hostLightCount);
        m_lightTags.resize(m_hostLightCount);
        m_lightLinks.resize(m_hostLightCount);
        for (VolumeRecord &v : m_volumes)
            v.lightIndex = kInvalidId;
        for (VolumeId id : m_activeVolumes) {
            VolumeRecord &v = m_volumes[id];
            if (!(v.lightPower > 0.0f))
                continue;
            LightData data;
            data.type = LightType::Volume;
            data.volume = id;
            data.power = v.lightPower;
            data.diffuse = v.desc.lightDiffuse;
            data.specular = v.desc.lightSpecular;
            v.lightIndex = uint32_t(m_lightData.size());
            m_lightData.push_back(data);
            m_lightLinks.emplace_back();
            m_lightTags.push_back(v.desc.lpeTag.empty() && hasMaterial(v.desc.material)
                                      ? material(v.desc.material).desc.lpeTag
                                      : v.desc.lpeTag);
        }
    }

    // Emissive triangles of treated meshes (M9 D14) after them: kept, or
    // built again.
    if (lightsChanged || volumesChanged)
        m_emitterBegin = uint32_t(m_lightData.size());
    bool emittersChanged = false;
    if (!keptEmitters.empty()) {
        m_lightData.insert(m_lightData.end(), keptEmitters.begin(), keptEmitters.end());
        for (MeshRecord &rec : m_meshes)
            if (!rec.emitterOf.empty())
                rec.emitterBase = rec.emitterBase - keptBegin + m_emitterBegin;
        emittersChanged = true;
    } else if (emitterInputsChanged || lightsChanged || volumesChanged) {
        bool treated = m_emitterBegin < m_lightData.size();
        for (const MeshRecord &rec : m_meshes)
            treated |= rec.alive && rec.lightSource.mode != 0;
        if (treated) {
            buildEmitters();
            emittersChanged = true;
        }
        m_emittersDirty = false;
    }

    // Light linking (M9 D15) before the tree, whose nodes carry the masks.
    const bool linksChanged = m_linksDirty || lightsChanged || volumesChanged || geometryChanged || emittersChanged;
    if (linksChanged)
        computeLinks();

    // The power of lights at infinity depends on the scene size.
    if (geometryChanged || lightsChanged || volumesChanged || linksChanged) {
        Vec3f lower, upper;
        m_accel->bounds(lower, upper);
        for (VolumeId id : m_activeVolumes) {
            const VolumeRecord &v = m_volumes[id];
            const bool none = !(lower.x <= upper.x) || !std::isfinite(lower.x) || !std::isfinite(upper.x);
            lower = none ? v.lower : Vec3f{std::min(lower.x, v.lower.x), std::min(lower.y, v.lower.y), std::min(lower.z, v.lower.z)};
            upper = none ? v.upper : Vec3f{std::max(upper.x, v.upper.x), std::max(upper.y, v.upper.y), std::max(upper.z, v.upper.z)};
        }
        const float radius = 0.5f * length(upper - lower);
        const bool valid = std::isfinite(radius) && radius > 0.0f && lower.x <= upper.x;
        m_sceneRadius = valid ? radius : 1.0f;
        m_intersectionTolerance =
            valid ? kIntersectionUlps * kFloatUlp * nova::fmax(maxComponent(absolute(lower)), maxComponent(absolute(upper)))
                  : 0.0f;

        std::vector<float> power(m_lightData.size());
        double infinite = 0.0, local = 0.0;
        auto atInfinity = [&](size_t i) {
            return m_lightData[i].type == LightType::Distant || m_lightData[i].type == LightType::Dome;
        };
        for (size_t i = 0; i < power.size(); ++i) {
            power[i] = lightPower(m_lightData[i], m_sceneRadius) * m_lightData[i].selection;
            (atInfinity(i) ? infinite : local) += power[i];
        }
        // The light tree (docs/design/M9.md D11, D12): every bounded light by
        // its box, power and emission cone; the lights at infinity by their
        // irradiance (their power is the flux through the scene's disk).
        {
            const size_t n = m_lightData.size();
            std::vector<LightBounds> bounds(n);
            std::vector<bool> infiniteLight(n);
            std::vector<float> infiniteE(n, 0.0f);
            std::vector<Vec3f> infiniteDir(n, Vec3f{0.0f, 0.0f, 0.0f});
            const float disk = kPi * m_sceneRadius * m_sceneRadius;
            for (size_t i = 0; i < n; ++i) {
                const LightData &l = m_lightData[i];
                // Karma's per-light sampling mode Uniform (D18): chosen with
                // the lights at infinity, by power, no direction. Lights with
                // no selection weight (sampling quality 0) illuminate nothing
                // (LightData::illuminates(), as Karma): no tree entry.
                infiniteLight[i] = atInfinity(i) || l.outsideTree;
                if (infiniteLight[i]) {
                    infiniteE[i] = power[i] / disk;
                    if (l.type == LightType::Distant)
                        infiniteDir[i] = l.direction;
                    continue;
                }
                const VolumeRecord *v = l.type == LightType::Volume && l.volume < m_volumes.size() ? &m_volumes[l.volume]
                                                                                                   : nullptr;
                bounds[i] = treeBounds(l, power[i], v ? v->lower : Vec3f{}, v ? v->upper : Vec3f{});
            }
            m_lightTree.update(bounds, infiniteLight, power, infiniteE, infiniteDir,
                               m_linking ? m_lightLinkMask : std::vector<uint64_t>{},
                               m_linking ? m_receiverCategories : std::vector<uint64_t>{});
        }
        // A large scene (e.g. a huge ground plane) inflates the power of the
        // lights at infinity until local lights get no samples; cap them at
        // half of the selection mass (until the light BVH of M9).
        if (local > 0.0 && infinite > local) {
            const float scale = float(local / infinite);
            for (size_t i = 0; i < power.size(); ++i)
                if (atInfinity(i))
                    power[i] *= scale;
        }
        m_lightSampler.build(power);

        // Equiangular sampling (M9 D17): the lights that opted in, by power.
        m_eqLights.clear();
        std::vector<float> eqPower;
        for (uint32_t i = 0; i < m_emitterBegin; ++i)
            if (m_lightData[i].equiangular && power[i] > 0.0f) {
                m_eqLights.push_back(i);
                eqPower.push_back(power[i]);
            }
        m_eqSampler.build(eqPower);
        m_eqPmf.assign(m_emitterBegin, 0.0f);
        const LightSamplerView eq = m_eqSampler.view();
        for (uint32_t s = 0; s < eq.count; ++s)
            m_eqPmf[m_eqLights[s]] = eq.pmf[s];
    }
    return geometryChanged || lightsChanged || bindingsChanged || volumesChanged || emittersChanged;
}

namespace {

// The inverse of the affine point map p -> x p.x + y p.y + z p.z + t
// (Cramer's rule on the cofactors).
Vec3f inversePoint(const Affine3f &a, Vec3f q) {
    const Vec3f d = q - a.t;
    const float det = a.determinant();
    const float inv = det != 0.0f ? 1.0f / det : 0.0f;
    return Vec3f{dot(d, cross(a.y, a.z)), dot(d, cross(a.z, a.x)), dot(d, cross(a.x, a.y))} * inv;
}

Affine3f toAffine(const Xform34 &x) {
    Affine3f a;
    a.x = {x.c[0][0], x.c[0][1], x.c[0][2]};
    a.y = {x.c[1][0], x.c[1][1], x.c[1][2]};
    a.z = {x.c[2][0], x.c[2][1], x.c[2][2]};
    a.t = {x.c[3][0], x.c[3][1], x.c[3][2]};
    return a;
}

// More emissive triangles than this cost a lot of memory and build time
// (docs/design/M9.md D14): the host is warned.
constexpr size_t kEmitterBudget = 2000000;

} // namespace

// Emissive meshes as lights (docs/design/M9.md D14): every triangle of a
// treated mesh whose material emits becomes a two-sided triangle light per
// instance, in world space, after the other lights. Its `emission` is the
// power estimate: the uniform inputs', or for varying emission the mean of
// the material at the triangle's centroid and corners (instance 0; any
// estimate keeps MIS unbiased). Deforming and moving meshes stay plain
// emitters (a static triangle cannot stand for them).
void Scene::buildEmitters() {
    m_emittersDirty = false;
    m_lightData.resize(m_emitterBegin);
    size_t total = 0;
    bool moving = false;
    for (MeshId id = 0; id < m_meshes.size(); ++id) {
        MeshRecord &rec = m_meshes[id];
        rec.emitterOf.clear();
        rec.emitterBase = rec.emitterStride = 0;
        const MeshDesc &m = rec.desc;
        if (!rec.alive || rec.lightSource.mode == 0 || !m.visible || m.shape != MeshShape::Triangles ||
            m.triangleCount() == 0 || rec.instanceCount() == 0)
            continue;
        if (m.pointSteps > 1 || rec.motionSteps > 1) {
            moving = true;
            continue;
        }
        const uint32_t tris = uint32_t(m.triangleCount());
        std::vector<Spectrum> estimate(tris, spectrum(0.0f));
        std::vector<uint32_t> varying; // triangles whose material's emission varies, by material
        uint32_t emitting = 0;
        rec.emitterOf.assign(tris, kInvalidId);
        for (uint32_t t = 0; t < tris; ++t) {
            const MaterialRecord &mat = materialOf(id, t);
            if (mat.emission == MaterialRecord::Emission::None)
                continue;
            rec.emitterOf[t] = emitting++;
            estimate[t] = mat.uniformEmission;
            if (mat.emission == MaterialRecord::Emission::Varying)
                varying.push_back(t);
        }
        if (emitting == 0) {
            rec.emitterOf.clear();
            continue;
        }
        if (!varying.empty()) {
            std::stable_sort(varying.begin(), varying.end(),
                             [&](uint32_t a, uint32_t b) { return materialIdOf(id, a) < materialIdOf(id, b); });
            constexpr size_t kChunk = 16; // triangles per evaluation, 4 points each
            tbb::parallel_for(size_t(0), (varying.size() + kChunk - 1) / kChunk, [&](size_t chunk) {
                const size_t begin = chunk * kChunk, end = std::min(begin + kChunk, varying.size());
                SurfaceHit hits[4 * kChunk];
                Vec3f dirs[4 * kChunk];
                Spectrum out[4 * kChunk];
                const Vec2f corners[4] = {{1.0f / 3.0f, 1.0f / 3.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
                for (size_t k = begin; k < end;) {
                    const MaterialId mat = materialIdOf(id, varying[k]);
                    size_t e = k;
                    int n = 0;
                    for (; e < end && materialIdOf(id, varying[e]) == mat; ++e)
                        for (const Vec2f &uv : corners) {
                            hits[n] = resolveHit(id, varying[e], uv, 1.0f, 0, 0.0f);
                            dirs[n] = -hits[n].Ng; // seen from the front
                            ++n;
                        }
                    evaluateEmission(*this, mat, hits, dirs, n, out);
                    for (size_t j = k; j < e; ++j) {
                        const Spectrum *o = out + 4 * (j - k);
                        estimate[varying[j]] = (o[0] + o[1] + o[2] + o[3]) * 0.25f;
                    }
                    k = e;
                }
            });
        }
        const uint32_t count = rec.instanceCount();
        rec.emitterBase = uint32_t(m_lightData.size());
        rec.emitterStride = emitting;
        total += size_t(emitting) * count;
        m_lightData.reserve(m_lightData.size() + size_t(emitting) * count);
        const float quality = std::isfinite(rec.lightSource.samplingQuality) && rec.lightSource.samplingQuality > 0.0f
                                  ? rec.lightSource.samplingQuality
                                  : 0.0f;
        for (uint32_t i = 0; i < count; ++i) {
            const Affine3f xf = toAffine(rec.instances[i]);
            for (uint32_t t = 0; t < tris; ++t) {
                if (rec.emitterOf[t] == kInvalidId)
                    continue;
                LightData d;
                d.type = LightType::Triangle;
                d.position = xf.point(m.points[m.indices[3 * t]]);
                d.axisU = xf.point(m.points[m.indices[3 * t + 1]]) - d.position;
                d.axisV = xf.point(m.points[m.indices[3 * t + 2]]) - d.position;
                const Vec3f nc = cross(d.axisU, d.axisV);
                const float len = length(nc);
                d.normal = len > 0.0f ? nc * (1.0f / len) : Vec3f{0.0f, 0.0f, 1.0f};
                d.area = 0.5f * len;
                d.twoSided = true;
                d.emission = isFinite(estimate[t]) ? estimate[t] : spectrum(0.0f);
                d.diffuse = rec.lightSource.diffuse;
                d.specular = rec.lightSource.specular;
                d.selection = quality;
                d.mesh = id;
                d.instance = i;
                d.prim = t;
                m_lightData.push_back(d);
            }
        }
    }
    if (moving)
        std::fprintf(stderr, "[Nova] moving or deforming meshes are not treated as light sources (motion blur)\n");
    if (total > kEmitterBudget)
        std::fprintf(stderr, "[Nova] %zu emissive triangles treated as lights: expect high memory use and build times\n",
                     total);
}

void Scene::setMeshLightSource(MeshId id, const MeshLightSource &settings) {
    std::lock_guard lock(m_mutex);
    if (id >= m_meshes.size() || !m_meshes[id].alive || m_meshes[id].lightSource == settings)
        return;
    m_meshes[id].lightSource = settings;
    m_emittersDirty = true;
    ++m_version;
}

const std::string &Scene::lightLpeTag(uint32_t i) const {
    if (i < m_lightTags.size())
        return m_lightTags[i];
    const LightData &l = m_lightData[i]; // an emissive triangle: its object's tag, else its material's
    const MeshRecord &rec = m_meshes[l.mesh];
    return rec.lpeTag.empty() ? material(materialIdOf(l.mesh, l.prim)).desc.lpeTag : rec.lpeTag;
}

// Hit reconstruction and curvature are shared with the GPU (core/scene/hit.h).
SurfaceHit Scene::resolveHit(MeshId meshId, uint32_t prim, Vec2f uv, float t, uint32_t instance, float time, Vec3f org,
                             Vec3f dir) const {
    return resolveMeshHit(m_meshes[meshId], meshId, prim, uv, t, instance, time, org, dir);
}

float Scene::curvature(const SurfaceHit &hit, float time) const { return meshCurvature(m_meshes[hit.mesh], hit, time); }

Affine3f Scene::objectToWorld(MeshId mesh, uint32_t instance, float time) const {
    const MeshRecord &rec = m_meshes[mesh];
    if (rec.motionSteps > 1)
        return srtAtTime(&rec.motionKeys[instance], rec.motionSteps, rec.instanceCount(), time);
    return toAffine(rec.instances[instance]);
}

bool Scene::intersect(const Ray &ray, SurfaceHit &hit, float time) const {
    float t = ray.tMax;
    uint32_t mesh = kInvalidId, prim = kInvalidId, instance = 0;
    Vec2f uv{};
    const RayBatch rays{&ray.org, &ray.dir, &ray.tMax, 1, &time};
    HitBatch hits{&t, &mesh, &prim, &uv, &instance};
    m_accel->intersect(rays, hits);
    if (mesh == kInvalidId)
        return false;
    hit = resolveHit(mesh, prim, uv, t, instance, time, ray.org, ray.dir);
    return true;
}

bool Scene::intersectLocal(MeshId mesh, uint32_t instance, float time, Vec3f org, Vec3f dir, float tMax, float &t,
                           uint32_t &prim, Vec2f &uv, float tMin) const {
    if (mesh >= m_meshes.size() || !m_meshes[mesh].alive || instance >= m_meshes[mesh].instanceCount())
        return false;
    // The object-space ray keeps the world ray's parameter (an affine map of
    // a unit direction, not renormalized).
    const Affine3f toObject = inverseAffine(objectToWorld(mesh, instance, time));
    return m_accel->intersectLocal(mesh, toObject.point(org), toObject.vector(dir), tMin, tMax, time, t, prim, uv);
}

bool Scene::occluded(const Ray &ray, float time) const {
    uint8_t blocked = 0;
    const RayBatch rays{&ray.org, &ray.dir, &ray.tMax, 1, &time};
    m_accel->occluded(rays, &blocked);
    return blocked != 0;
}

} // namespace nova
