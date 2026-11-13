// Random edit sequences against a from-scratch rebuild (IPR). A scene that
// the host edits step by step (meshes and analytic spheres added, reshaped,
// moved, instanced, hidden and removed; materials made, changed, bound and
// removed; lights and volumes likewise; light-linking categories and the
// fake-caustics default; the camera, the image size, the depth, the light
// tree and the light path expressions changed) must render what a new scene
// with the same objects renders in a new renderer, bit for bit: the objects
// carry stable keys, and neither the scene's incremental commit nor the
// renderer's caches (the acceleration structure, the light lists and tree,
// the device's geometry) may leave anything of an earlier state behind. On
// the CPU and, with a GPU, on Nova XPU; every other seed with the scene in
// its interactive mode. The seed and the edits of a failing step are in the
// failure message (docs/bug-audit-2026-11.md, section 13).
//
// The default runs a few seeds; "[.][fuzz]" runs NOVA_FUZZ_SEEDS seeds of
// NOVA_FUZZ_STEPS steps from NOVA_FUZZ_FIRST on NOVA_FUZZ_DEVICE (0: the CPU,
// 1: Nova XPU) for a longer hunt.

#if defined(_MSC_VER)
#pragma warning(disable : 4702) // unreachable code in NanoVDB's grid builder templates
#endif

#include "test_helpers.h"

#include "core/film/lpe_compile.h"
#include "core/integrator/renderer.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"
#if NOVA_HAS_GPU
#include "core/gpu/device.h"
#endif

#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/GridBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;

namespace {

int environmentInt(const char *name, int fallback) {
#if defined(_MSC_VER)
    char *value = nullptr;
    size_t size = 0;
    const bool has = _dupenv_s(&value, &size, name) == 0 && value && *value;
    const int result = has ? std::atoi(value) : fallback;
    std::free(value);
    return result;
#else
    const char *value = std::getenv(name);
    return value && *value ? std::atoi(value) : fallback;
#endif
}

VolumeGridPtr cubeGrid(float value, int n, double h) {
    nanovdb::build::Grid<float> g(0.0f, "field", nanovdb::GridClass::FogVolume);
    g.setTransform(h, nanovdb::Vec3d(0.0));
    // Voxel by voxel: the builder's fill of a box turns a leaf of one value into a tile, and of the eight
    // such tiles around the origin (n = 8) the grid then lacked the one at (-8, -8, -8): 3584 voxels of 4096.
    auto acc = g.getAccessor();
    for (int z = -n; z < n; ++z)
        for (int y = -n; y < n; ++y)
            for (int x = -n; x < n; ++x)
                acc.setValue(nanovdb::Coord(x, y, z), value);
    const nanovdb::GridHandle<nanovdb::HostBuffer> handle = nanovdb::createNanoGrid(g);
    return VolumeGrid::copyOf(handle.data(), handle.size());
}

MaterialDesc medium(Vec3f s, Vec3f a, Vec3f e, float g) {
    using namespace shading;
    const auto field = [](uint16_t f) { return uint16_t(volume_field::kBase + f); };
    IrGraph graph;
    IrBuilder b(graph);
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    b.output(field(volume_field::scattering), 3, b.mul(d, b.constant(s.x, s.y, s.z)));
    b.output(field(volume_field::absorption), 3, b.mul(d, b.constant(a.x, a.y, a.z)));
    b.output(field(volume_field::emission), 3, b.mul(d, b.constant(e.x, e.y, e.z)));
    b.output(field(volume_field::anisotropy), 1, b.constant(g));
    MaterialDesc m;
    m.program = compile(graph).program;
    return m;
}

// What the host holds: the objects by key, as they are now.
struct MeshModel {
    uint64_t key = 0;
    MeshDesc desc;                   // with its transform and its visibility, as the delegate passes them
    bool instanced = false;
    std::vector<Xform34> instances;  // when instanced (may be empty: renders nothing)
    int material = -1;               // index into Model::materials (-1: none; a removed one: the default)
    std::vector<std::string> categories; // light linking
};
struct MaterialModel {
    MaterialDesc desc;
    bool alive = true;
};
struct LightModel {
    uint64_t key = 0;
    LightDesc desc;
};
struct VolumeModel {
    uint64_t key = 0;
    int medium = 0; // index into Model::media
    bool light = false;
    Mat4 transform = Mat4::identity();
};
struct Model {
    std::vector<MeshModel> meshes;
    std::vector<MaterialModel> materials;
    std::vector<MaterialDesc> media;
    std::vector<LightModel> lights;
    std::vector<VolumeModel> volumes;
    VolumeGridPtr grid;
    Vec3f eye{0.0f, 0.0f, 5.0f};
    int size = 32;
    uint32_t depth = 4;
    bool lightTree = false;
    bool fakeCaustics = false;
    int lpe = 0; // the light path expressions: none, or one of two sets
    // A light's power was edited alone: with the light tree the edited scene keeps the tree's
    // shape and sums the powers again (docs/design/M9.md D11), so it chooses lights with other
    // probabilities than a tree built anew. Both are right; the images agree only without it.
    bool powerEdited = false;
};

// A scene with the model's objects, made in one go.
struct Built {
    Scene scene;
    std::vector<MaterialId> materials, media;
};

void applyMesh(Scene &scene, MeshId id, const MeshModel &m, const std::vector<MaterialId> &materials,
               const std::vector<MaterialModel> &models) {
    scene.updateMesh(id, m.desc);
    if (m.instanced)
        scene.setMeshInstances(id, m.instances);
    scene.setMeshCategories(id, m.categories);
    if (m.material >= 0 && models[size_t(m.material)].alive)
        scene.setMeshMaterials(id, {materials[size_t(m.material)]});
    else
        scene.setMeshMaterials(id, {});
}

VolumeDesc volumeDesc(const Model &model, const VolumeModel &v, const std::vector<MaterialId> &media) {
    VolumeDesc d;
    d.fields.push_back({"density", "density", model.grid, Affine3f{}});
    d.material = media[size_t(v.medium)];
    d.light = v.light;
    return d;
}

void build(const Model &model, Built &out) {
    Scene &scene = out.scene;
    scene.setDefaultFakeCaustics(model.fakeCaustics);
    for (const MaterialDesc &m : model.media) {
        out.media.push_back(scene.createMaterial());
        scene.updateMaterial(out.media.back(), m);
    }
    for (const MaterialModel &m : model.materials) {
        // A removed material leaves no trace but its place in the list.
        out.materials.push_back(m.alive ? scene.createMaterial() : MaterialId(0));
        if (m.alive)
            scene.updateMaterial(out.materials.back(), m.desc);
    }
    for (const MeshModel &m : model.meshes)
        applyMesh(scene, scene.createMesh(m.key), m, out.materials, model.materials);
    for (const LightModel &l : model.lights)
        scene.updateLight(scene.createLight(l.key), l.desc);
    for (const VolumeModel &v : model.volumes) {
        const VolumeId id = scene.createVolume(v.key);
        scene.updateVolume(id, volumeDesc(model, v, out.media));
        scene.updateVolumeMotion(id, {v.transform});
    }
    scene.commit();
}

// The host: a model and the scene it edits step by step.
class Host {
public:
    explicit Host(uint32_t seed) : m_rng(seed), m_staleTree(environmentInt("NOVA_FUZZ_STALE_TREE", 0) != 0) {
        m_model.grid = cubeGrid(1.0f, 8, 1.0 / 16.0);
        m_scene.setDefaultFakeCaustics(m_model.fakeCaustics);
        m_model.media.push_back(medium({1.2f, 1.2f, 1.2f}, {0.2f, 0.2f, 0.2f}, {0.0f, 0.0f, 0.0f}, 0.3f));
        m_model.media.push_back(medium({0.2f, 0.2f, 0.2f}, {0.6f, 0.6f, 0.6f}, {1.5f, 0.8f, 0.3f}, 0.0f));
        for (const MaterialDesc &m : m_model.media) {
            m_media.push_back(m_scene.createMaterial());
            m_scene.updateMaterial(m_media.back(), m);
        }
        // A ground and a light that stay, so that there is always an image.
        MeshModel ground;
        ground.key = nextKey();
        ground.desc = makeQuad(2.5f);
        ground.desc.setDisplayColor(spectrum(0.6f));
        ground.desc.transform = translation({0.0f, 0.0f, -0.6f});
        addMesh(ground);
        LightModel key;
        key.key = nextKey();
        key.desc.type = LightType::Rect;
        key.desc.width = key.desc.height = 1.5f;
        key.desc.intensity = 4.0f;
        key.desc.transform = translation({0.0f, 0.0f, 3.0f});
        addLight(key);
        m_scene.commit();
    }

    const Model &model() const { return m_model; }
    Scene &scene() { return m_scene; }

    // One random edit; its name for the log.
    std::string edit() {
        Model &m = m_model;
        switch (pick(24)) {
        case 0:
        case 1: { // a mesh
            if (m.meshes.size() >= 9)
                return removeMesh();
            MeshModel mesh;
            mesh.key = nextKey();
            mesh.desc = shape();
            mesh.desc.transform = place();
            mesh.material = m.materials.empty() || chance(0.3f) ? -1 : pick(int(m.materials.size()));
            addMesh(mesh);
            return "add mesh, material " + std::to_string(mesh.material);
        }
        case 2: { // new geometry
            if (m.meshes.size() < 2)
                return "none";
            const size_t i = movable();
            const Mat4 keep = m.meshes[i].desc.transform;
            const bool visible = m.meshes[i].desc.visible;
            m.meshes[i].desc = shape();
            m.meshes[i].desc.transform = keep;
            m.meshes[i].desc.visible = visible;
            m_scene.updateMesh(m_meshIds[i], m.meshes[i].desc);
            // updateMesh takes the description's transform: an instanced mesh keeps its instances.
            return "reshape mesh " + std::to_string(i);
        }
        case 3: { // a transform
            if (m.meshes.size() < 2)
                return "none";
            const size_t i = movable();
            m.meshes[i].desc.transform = place();
            m.meshes[i].instanced = false;
            m.meshes[i].instances.clear();
            m_scene.updateMeshTransform(m_meshIds[i], m.meshes[i].desc.transform);
            return "move mesh " + std::to_string(i);
        }
        case 4:
            return removeMesh();
        case 5: { // instances
            if (m.meshes.size() < 2)
                return "none";
            const size_t i = movable();
            m.meshes[i].instanced = true;
            m.meshes[i].instances.clear();
            for (int k = pick(4); k > 0; --k)
                m.meshes[i].instances.push_back(toXform34(place()));
            m_scene.setMeshInstances(m_meshIds[i], m.meshes[i].instances);
            return "instance mesh " + std::to_string(i) + " x" + std::to_string(m.meshes[i].instances.size());
        }
        case 6: { // visibility
            if (m.meshes.size() < 2)
                return "none";
            const size_t i = movable();
            m.meshes[i].desc.visible = !m.meshes[i].desc.visible;
            m_scene.setMeshVisible(m_meshIds[i], m.meshes[i].desc.visible);
            return m.meshes[i].desc.visible ? "show mesh" : "hide mesh";
        }
        case 7:
        case 8: { // a material
            if (m.materials.size() >= 8)
                return removeMaterial();
            m.materials.push_back({surface(), true});
            m_materialIds.push_back(m_scene.createMaterial());
            m_scene.updateMaterial(m_materialIds.back(), m.materials.back().desc);
            return "add material " + std::to_string(m.materials.size() - 1) + " (" + m_kind + ")";
        }
        case 9: { // its parameters
            const int i = aliveMaterial();
            if (i < 0)
                return "none";
            m.materials[size_t(i)].desc = surface();
            m_scene.updateMaterial(m_materialIds[size_t(i)], m.materials[size_t(i)].desc);
            return "change material " + std::to_string(i) + " (" + m_kind + ")";
        }
        case 10:
        case 11: { // a binding
            if (m.meshes.empty())
                return "none";
            const size_t i = size_t(pick(int(m.meshes.size())));
            m.meshes[i].material = m.materials.empty() || chance(0.2f) ? -1 : pick(int(m.materials.size()));
            const int mat = m.meshes[i].material;
            if (mat >= 0 && m.materials[size_t(mat)].alive)
                m_scene.setMeshMaterials(m_meshIds[i], {m_materialIds[size_t(mat)]});
            else
                m_scene.setMeshMaterials(m_meshIds[i], {});
            return "bind material " + std::to_string(mat) + " to mesh " + std::to_string(i);
        }
        case 12:
            return removeMaterial();
        case 13: { // a light
            if (m.lights.size() >= 5)
                return removeLight();
            LightModel l;
            l.key = nextKey();
            l.desc = light();
            addLight(l);
            m.powerEdited = false; // the tree is built anew
            return "add light";
        }
        case 14: {
            const size_t i = size_t(pick(int(m.lights.size())));
            m.lights[i].desc = light();
            m_scene.updateLight(m_lightIds[i], m.lights[i].desc);
            // Built anew unless the new light happens to sit where the old one did: the tree stays off.
            return "change light " + std::to_string(i);
        }
        case 15:
            return removeLight();
        case 20: { // a light's power alone
            const size_t i = size_t(pick(int(m.lights.size())));
            m.lights[i].desc.intensity *= uniform(0.5f, 1.5f);
            m.lights[i].desc.color = Spectrum{uniform(0.5f, 1.0f), uniform(0.5f, 1.0f), uniform(0.5f, 1.0f)};
            m_scene.updateLight(m_lightIds[i], m.lights[i].desc);
            if (!m_staleTree) {
                m.powerEdited = true;
                m.lightTree = false;
            }
            return "dim light " + std::to_string(i);
        }
        case 16: { // a volume
            if (m.volumes.size() >= 2) {
                const size_t i = size_t(pick(int(m.volumes.size())));
                m_scene.removeVolume(m_volumeIds[i]);
                m.volumes.erase(m.volumes.begin() + ptrdiff_t(i));
                m_volumeIds.erase(m_volumeIds.begin() + ptrdiff_t(i));
                return "remove volume";
            }
            VolumeModel v;
            v.key = nextKey();
            v.medium = pick(2);
            v.light = v.medium == 1 && chance(0.5f);
            v.transform = place();
            m.volumes.push_back(v);
            m_volumeIds.push_back(m_scene.createVolume(v.key));
            m_scene.updateVolume(m_volumeIds.back(), volumeDesc(m, v, m_media));
            m_scene.updateVolumeMotion(m_volumeIds.back(), {v.transform});
            return "add volume";
        }
        case 17: {
            if (m.volumes.empty())
                return "none";
            const size_t i = size_t(pick(int(m.volumes.size())));
            m.volumes[i].transform = place();
            m_scene.updateVolumeMotion(m_volumeIds[i], {m.volumes[i].transform});
            return "move volume";
        }
        case 18: { // the view
            m.eye = {uniform(-0.3f, 0.3f), uniform(-0.3f, 0.3f), 5.0f};
            static const int sizes[3] = {24, 32, 40};
            m.size = sizes[pick(3)];
            return "view " + std::to_string(m.size);
        }
        case 21: { // light-linking categories
            const size_t i = size_t(pick(int(m.meshes.size())));
            static const char *const names[2] = {"a", "b"};
            m.meshes[i].categories.clear();
            for (int k = 0; k < 2; ++k)
                if (chance(0.5f))
                    m.meshes[i].categories.push_back(names[k]);
            m_scene.setMeshCategories(m_meshIds[i], m.meshes[i].categories);
            return "categories of mesh " + std::to_string(i) + ": " + std::to_string(m.meshes[i].categories.size());
        }
        case 22: // the objects' default for fake caustics
            m.fakeCaustics = !m.fakeCaustics;
            m_scene.setDefaultFakeCaustics(m.fakeCaustics);
            return m.fakeCaustics ? "fake caustics on" : "fake caustics off";
        default: // the settings
            m.depth = uint32_t(1 + pick(5));
            m.lpe = pick(3);
            m.lightTree = chance(0.5f) && !m.powerEdited;
            return std::string("depth ") + std::to_string(m.depth) + (m.lightTree ? ", light tree" : "") + ", lpe set " +
                   std::to_string(m.lpe);
        }
    }

private:
    uint64_t nextKey() { return 0x9E3779B97F4A7C15ull * ++m_keys; }
    int pick(int n) { return int(std::uniform_int_distribution<int>(0, n - 1)(m_rng)); }
    float uniform(float a, float b) { return std::uniform_real_distribution<float>(a, b)(m_rng); }
    bool chance(float p) { return uniform(0.0f, 1.0f) < p; }
    // Any mesh but the ground.
    size_t movable() { return size_t(1 + pick(int(m_model.meshes.size()) - 1)); }
    int aliveMaterial() {
        std::vector<int> alive;
        for (size_t i = 0; i < m_model.materials.size(); ++i)
            if (m_model.materials[i].alive)
                alive.push_back(int(i));
        return alive.empty() ? -1 : alive[size_t(pick(int(alive.size())))];
    }

    MeshDesc shape() {
        MeshDesc d = chance(0.7f) ? makeSphere(uniform(0.15f, 0.45f), 8 + 4 * pick(5), 4 + 2 * pick(5))
                                  : makeQuad(uniform(0.2f, 0.6f));
        if (chance(0.15f)) { // an analytic sphere
            d = MeshDesc{};
            d.shape = MeshShape::Sphere;
            d.points = {{0.0f, 0.0f, 0.0f}};
            d.sphereRadius = {uniform(0.15f, 0.4f)};
        }
        if (chance(0.5f))
            d.setDisplayColor(Spectrum{uniform(0.1f, 0.9f), uniform(0.1f, 0.9f), uniform(0.1f, 0.9f)});
        return d;
    }
    Mat4 place() {
        return translation({uniform(-1.1f, 1.1f), uniform(-1.1f, 1.1f), uniform(-0.3f, 0.9f)}) *
               rotationAbout({uniform(-1.0f, 1.0f), uniform(-1.0f, 1.0f), 0.3f}, double(uniform(0.0f, 180.0f)));
    }
    MaterialDesc surface() {
        namespace sf = surface_field;
        MaterialDesc m;
        m.inputs = standardSurfaceDefaults();
        m.inputs.set(sf::baseColor, Spectrum{uniform(0.1f, 0.9f), uniform(0.1f, 0.9f), uniform(0.1f, 0.9f)});
        m.inputs.set(sf::specularRoughness, uniform(0.05f, 0.8f));
        static const char *const kinds[7] = {"metal", "glass", "cut out", "emitter", "subsurface", "thin glass", "coat"};
        const int kind = pick(7);
        m_kind = kinds[kind];
        switch (kind) {
        case 0:
            m.inputs.set(sf::metalness, 1.0f);
            break;
        case 1: // glass
            m.inputs.set(sf::transmissionWeight, 1.0f);
            m.inputs.set(sf::specularRoughness, chance(0.5f) ? 0.0f : 0.2f);
            break;
        case 2: // cut out
            m.inputs.set(sf::opacity, spectrum(uniform(0.2f, 0.8f)));
            break;
        case 3: // an emitter (a mesh light)
            m.inputs.set(sf::emissionWeight, uniform(0.5f, 3.0f));
            m.inputs.set(sf::emissionColor, Spectrum{1.0f, uniform(0.3f, 1.0f), uniform(0.3f, 1.0f)});
            break;
        case 4: // subsurface
            m.inputs.set(sf::subsurfaceWeight, 1.0f);
            m.inputs.set(sf::subsurfaceColor, Spectrum{0.9f, 0.6f, 0.4f});
            m.inputs.set(sf::subsurfaceRadius, Spectrum{1.0f, 0.4f, 0.2f});
            m.inputs.set(sf::subsurfaceScale, uniform(0.05f, 0.3f));
            break;
        case 5: // thin glass
            m.inputs.set(sf::transmissionWeight, 1.0f);
            m.inputs.set(sf::thinWalled, 1.0f);
            break;
        default:
            m.inputs.set(sf::coatWeight, uniform(0.0f, 1.0f));
            break;
        }
        return m;
    }
    LightDesc light() {
        LightDesc l;
        static const LightType kinds[5] = {LightType::Rect, LightType::Sphere, LightType::Disk, LightType::Distant,
                                           LightType::Dome};
        l.type = kinds[pick(5)];
        l.color = Spectrum{uniform(0.5f, 1.0f), uniform(0.5f, 1.0f), uniform(0.5f, 1.0f)};
        l.intensity = l.type == LightType::Dome ? uniform(0.2f, 0.8f) : uniform(1.0f, 6.0f);
        l.width = uniform(0.3f, 1.5f);
        l.height = uniform(0.3f, 1.5f);
        l.radius = uniform(0.1f, 0.5f);
        l.cameraVisible = chance(0.3f);
        // Light linking: most lights light and are shadowed by everything.
        static const char *const links[3] = {"", "a", "b"};
        l.lightLink = chance(0.75f) ? "" : links[1 + pick(2)];
        l.shadowLink = chance(0.85f) ? "" : links[1 + pick(2)];
        l.transform = translation({uniform(-1.5f, 1.5f), uniform(-1.5f, 1.5f), uniform(1.5f, 3.0f)});
        return l;
    }

    void addMesh(const MeshModel &mesh) {
        m_model.meshes.push_back(mesh);
        m_meshIds.push_back(m_scene.createMesh(mesh.key));
        applyMesh(m_scene, m_meshIds.back(), mesh, m_materialIds, m_model.materials);
    }
    void addLight(const LightModel &l) {
        m_model.lights.push_back(l);
        m_lightIds.push_back(m_scene.createLight(l.key));
        m_scene.updateLight(m_lightIds.back(), l.desc);
    }
    std::string removeMesh() {
        if (m_model.meshes.size() < 2)
            return "none";
        const size_t i = movable();
        m_scene.removeMesh(m_meshIds[i]);
        m_model.meshes.erase(m_model.meshes.begin() + ptrdiff_t(i));
        m_meshIds.erase(m_meshIds.begin() + ptrdiff_t(i));
        return "remove mesh " + std::to_string(i);
    }
    // Meshes keep referring to a removed material: they render the default.
    std::string removeMaterial() {
        const int i = aliveMaterial();
        if (i < 0)
            return "none";
        m_model.materials[size_t(i)].alive = false;
        m_scene.removeMaterial(m_materialIds[size_t(i)]);
        return "remove material " + std::to_string(i);
    }
    std::string removeLight() {
        if (m_model.lights.size() < 2)
            return "none";
        const size_t i = size_t(1 + pick(int(m_model.lights.size()) - 1));
        m_scene.removeLight(m_lightIds[i]);
        m_model.lights.erase(m_model.lights.begin() + ptrdiff_t(i));
        m_lightIds.erase(m_lightIds.begin() + ptrdiff_t(i));
        return "remove light";
    }

    std::mt19937 m_rng;
    uint64_t m_keys = 0;
    std::string m_kind; // of the material surface() made last
    bool m_staleTree;   // NOVA_FUZZ_STALE_TREE: the light tree stays on after a power edit
    Model m_model;
    Scene m_scene;
    std::vector<MeshId> m_meshIds;
    std::vector<MaterialId> m_materialIds, m_media;
    std::vector<LightId> m_lightIds;
    std::vector<VolumeId> m_volumeIds;
};

// The two sets of light path expressions the settings switch between.
std::shared_ptr<const lpe::Compiled> expressions(int set) {
    static const std::shared_ptr<const lpe::Compiled> sets[2] = {
        std::make_shared<lpe::Compiled>(lpe::compile({"C<RD>L", "C<RD>.+L", "C<TS>.*", "CV.*L"})),
        std::make_shared<lpe::Compiled>(lpe::compile({"C.*L", "C<RG>.*L", "CO", "shadow;C<RD>L", "unoccluded;C<RD>L"}))};
    return set > 0 ? sets[set - 1] : nullptr;
}

void configure(Renderer &r, const Scene &scene, const Model &m, RenderDevice device) {
    RenderSettings settings;
    settings.lpe = expressions(m.lpe);
    settings.device = device;
    settings.maxSamples = 4;
    settings.maxDepth = m.depth;
    settings.lightTree = m.lightTree;
    r.setScene(&scene);
    r.setSettings(settings);
    r.setCamera(makeOrthoCamera(m.eye, 1.6f, m.size, m.size));
    r.resize(m.size, m.size);
}

struct Difference {
    size_t pixels = 0, nonFinite = 0;
    float largest = 0.0f;
    bool lpe = false; // the light path expressions' sums differ
};

Difference compare(const Film &a, const Film &b) {
    Difference d;
    if (a.colorSum.size() != b.colorSum.size()) {
        d.pixels = ~size_t(0);
        return d;
    }
    // The light path expressions' sums, whole: a plane too many or too few counts as every pixel.
    const auto same = [](const std::vector<Spectrum> &x, const std::vector<Spectrum> &y) {
        return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(Spectrum)) == 0);
    };
    if (!same(a.lpeSum, b.lpeSum) || !same(a.lpeLit, b.lpeLit)) {
        d.pixels = a.colorSum.size();
        d.lpe = true;
    }
    for (size_t i = 0; i < a.colorSum.size(); ++i) {
        const Spectrum x = a.colorSum[i], y = b.colorSum[i];
        if (!(std::isfinite(x.x) && std::isfinite(x.y) && std::isfinite(x.z)))
            ++d.nonFinite;
        if (std::memcmp(&x, &y, sizeof(Spectrum)) != 0 || a.alphaSum[i] != b.alphaSum[i] || a.count[i] != b.count[i]) {
            ++d.pixels;
            d.largest = std::max({d.largest, std::fabs(x.x - y.x), std::fabs(x.y - y.y), std::fabs(x.z - y.z)});
        }
    }
    return d;
}

// One seed: `steps` steps of one to three edits; after each the edited
// scene in the renderer that saw every step against a rebuild. False at the
// first difference, with the story in `log`.
bool runSeed(uint32_t seed, int steps, RenderDevice device, std::string &log) {
    Host host(seed);
    // Every other seed edits as a viewport does: the acceleration structure refits where it can.
    host.scene().setInteractive((seed & 1u) != 0);
    std::mt19937 counts(seed ^ 0x5bd1e995u);
    Renderer live;
    std::ostringstream story;
    for (int step = 0; step < steps; ++step) {
        story << "  step " << step << ":";
        for (int n = 1 + int(counts() % 3u); n > 0; --n)
            story << " [" << host.edit() << "]";
        story << "\n";
        host.scene().commit();
        configure(live, host.scene(), host.model(), device);
        live.reset();
        renderToConvergence(live);
        Built rebuilt;
        build(host.model(), rebuilt);
        Renderer fresh;
        configure(fresh, rebuilt.scene, host.model(), device);
        renderToConvergence(fresh);
        const bool wantGpu = device == RenderDevice::Xpu;
        const Difference d = compare(live.film(), fresh.film());
        if (d.pixels != 0 || d.nonFinite != 0 || live.lastPassOnGpu() != wantGpu || fresh.lastPassOnGpu() != wantGpu) {
            story << (d.lpe ? "  -> the light path expressions' sums differ\n" : "");
            story << "  -> " << d.pixels << " of " << live.film().colorSum.size() << " pixels differ (largest "
                  << d.largest << "), " << d.nonFinite << " not finite; on the GPU: " << live.lastPassOnGpu() << " / "
                  << fresh.lastPassOnGpu() << "\n";
            // Where it comes from: a new renderer on the edited scene renders
            // the rebuild's image if the old renderer kept something, and
            // the old renderer's if the scene did.
            Renderer second;
            configure(second, host.scene(), host.model(), device);
            renderToConvergence(second);
            const Difference kept = compare(second.film(), fresh.film());
            story << "  -> a new renderer on the edited scene differs from the rebuild in " << kept.pixels
                  << " pixels: " << (kept.pixels == 0 ? "the renderer kept something" : "the scene kept something")
                  << "\n";
            log = "seed " + std::to_string(seed) + " (" + (wantGpu ? "xpu" : "cpu") + "):\n" + story.str();
            return false;
        }
    }
    return true;
}

bool gpuUsable() {
#if NOVA_HAS_GPU
    gpu::Device device;
    std::string why;
    return device.init(why);
#else
    return false;
#endif
}

} // namespace

TEST_CASE("Edited scenes render as rebuilt ones", "[render][ipr][fuzz]") {
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        std::string log;
        const bool same = runSeed(seed, 10, RenderDevice::Cpu, log);
        INFO(log);
        CHECK(same);
    }
}

TEST_CASE("XPU edited scenes render as rebuilt ones", "[gpu][render][ipr][fuzz]") {
    if (!gpuUsable())
        SKIP("no usable GPU");
    for (uint32_t seed = 1; seed <= 4; ++seed) {
        std::string log;
        const bool same = runSeed(seed, 8, RenderDevice::Xpu, log);
        INFO(log);
        CHECK(same);
    }
}

// The hunt: NOVA_FUZZ_SEEDS seeds from NOVA_FUZZ_FIRST, NOVA_FUZZ_STEPS steps
// each, on NOVA_FUZZ_DEVICE (0: CPU, 1: Nova XPU). Every failing seed is
// reported, the first few with their story.
TEST_CASE("Edited scenes render as rebuilt ones over many seeds", "[.][fuzz]") {
    const int seeds = environmentInt("NOVA_FUZZ_SEEDS", 100), steps = environmentInt("NOVA_FUZZ_STEPS", 20);
    const int first = environmentInt("NOVA_FUZZ_FIRST", 1000);
    const RenderDevice device = environmentInt("NOVA_FUZZ_DEVICE", 0) ? RenderDevice::Xpu : RenderDevice::Cpu;
    if (device == RenderDevice::Xpu && !gpuUsable())
        SKIP("no usable GPU");
    int failed = 0;
    for (int i = 0; i < seeds; ++i) {
        std::string log;
        if (!runSeed(uint32_t(first + i), steps, device, log)) {
            if (++failed <= 5)
                WARN(log);
            else
                WARN(log.substr(0, log.find('\n')));
        }
    }
    INFO(failed << " of " << seeds << " seeds differ");
    CHECK(failed == 0);
}
