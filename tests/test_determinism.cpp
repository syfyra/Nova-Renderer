// Images must not depend on the order a host creates its objects in
// (docs/bug-audit-2026-11.md, D-1). Hydra syncs rprims in parallel and
// sprims out of hash maps, so the ids of Nova's meshes, lights and volumes
// differ from run to run; hosts key their objects by something stable
// (core/scene/object_key.h). The same keyed objects created in any order
// render bit-identical images: stochastic opacity, emissive triangles
// treated as lights, several lights, overlapping volumes and the
// per-strand random number. Without keys the order shows (the check that
// the scene is sensitive to it).

#include "test_helpers.h"

#include "core/geometry/curves.h"
#include "core/integrator/renderer.h"
#include "core/scene/object_key.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"
#include "core/volume/shader.h"
#include "core/volume/shell.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sh = nova::shading;

namespace {

constexpr int kVeils = 6, kEmitters = 2, kLights = 4, kVolumes = 2;

std::optional<uint64_t> keyOf(bool keyed, const char *kind, int i) {
    if (!keyed)
        return std::nullopt;
    return objectKey(std::string("/World/") + kind + std::to_string(i));
}

MaterialId addMaterial(Scene &scene, const MaterialDesc &desc) {
    const MaterialId id = scene.createMaterial();
    scene.updateMaterial(id, desc);
    return id;
}

// A closed box shell (outward winding).
MediumShellPtr boxShell(Vec3f lo, Vec3f hi) {
    const Vec3f p[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                        {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    const uint32_t quads[6][4] = {{0, 3, 2, 1}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}, {4, 5, 6, 7}};
    std::vector<uint32_t> indices;
    for (const auto &q : quads)
        indices.insert(indices.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
    return makeTriangleShell(p, 8, indices.data(), indices.size() / 3);
}

// Every object of the test scene, created in the orders given (a
// permutation of each kind's indices); keyed by name or by id.
void buildScene(Scene &scene, const std::vector<int> &meshOrder, const std::vector<int> &lightOrder,
                const std::vector<int> &volumeOrder, bool keyed) {
    MaterialDesc grey;
    grey.inputs = standardSurfaceDefaults();
    grey.inputs.set(surface_field::baseColor, spectrum(0.5f));
    grey.inputs.set(surface_field::specularWeight, 0.0f);
    const MaterialId backdrop = addMaterial(scene, grey);
    std::vector<MaterialId> veils;
    for (int i = 0; i < kVeils; ++i) {
        MaterialDesc veil = grey;
        veil.inputs.set(surface_field::baseColor, Spectrum{0.2f + 0.12f * float(i), 0.6f, 0.9f - 0.1f * float(i)});
        veil.inputs.set(surface_field::opacity, spectrum(0.5f));
        veils.push_back(addMaterial(scene, veil));
    }
    MaterialDesc glow;
    glow.inputs = standardSurfaceDefaults();
    glow.inputs.set(surface_field::baseWeight, 0.0f);
    glow.inputs.set(surface_field::specularWeight, 0.0f);
    glow.inputs.set(surface_field::emissionWeight, 1.0f);
    glow.inputs.set(surface_field::emissionColor, Spectrum{3.0f, 2.0f, 1.0f});
    const MaterialId emissive = addMaterial(scene, glow);
    // Two media with other colours and phase functions: where the volumes
    // overlap, which of them scatters is picked in the order of the list.
    std::vector<MaterialId> media;
    for (int i = 0; i < kVolumes; ++i) {
        sh::IrGraph g;
        sh::IrBuilder b(g);
        const auto field = [](uint16_t f) { return uint16_t(volume_field::kBase + f); };
        const sh::Ref scattering = i == 0 ? b.constant(1.2f, 0.6f, 0.3f) : b.constant(0.3f, 0.6f, 1.2f);
        b.output(field(volume_field::scattering), 3, scattering);
        b.output(field(volume_field::absorption), 3, b.constant(0.2f, 0.2f, 0.2f));
        b.output(field(volume_field::anisotropy), 1, b.constant(i == 0 ? 0.8f : -0.5f));
        MaterialDesc fog;
        fog.program = sh::compile(g).program;
        REQUIRE(fog.program);
        media.push_back(addMaterial(scene, fog));
    }

    // Meshes: the backdrop, the partly transparent veils, the emitters.
    const MeshId back = scene.createMesh(keyOf(keyed, "back", 0));
    MeshDesc backQuad = makeQuad(2.0f);
    backQuad.transform = translation({0.0f, 0.0f, -1.0f});
    scene.updateMesh(back, backQuad);
    scene.setMeshMaterials(back, {backdrop});
    for (int i : meshOrder) {
        if (i < kVeils) {
            const MeshId id = scene.createMesh(keyOf(keyed, "veil", i));
            MeshDesc q = makeQuad(0.45f);
            q.transform = translation({-0.9f + 0.36f * float(i), 0.3f * float(i % 3) - 0.3f, 0.1f * float(i)});
            scene.updateMesh(id, q);
            scene.setMeshMaterials(id, {veils[size_t(i)]});
        } else {
            const int e = i - kVeils;
            const MeshId id = scene.createMesh(keyOf(keyed, "glow", e));
            MeshDesc q = makeQuad(0.15f);
            q.transform = translation({-1.2f + 2.4f * float(e), 1.2f, 0.8f});
            scene.updateMesh(id, q);
            scene.setMeshMaterials(id, {emissive});
            MeshLightSource on;
            on.mode = 1;
            scene.setMeshLightSource(id, on);
        }
    }
    // Lights: two distant ones and two sphere lights sampled outside the
    // tree (Karma's Uniform mode): both kinds are picked from a list in its
    // order (the tree itself splits by position).
    for (int i : lightOrder) {
        const LightId id = scene.createLight(keyOf(keyed, "lamp", i));
        LightDesc l;
        l.color = Spectrum{i == 0 ? 1.0f : 0.3f, i == 1 ? 1.0f : 0.3f, i == 2 ? 1.0f : 0.3f};
        if (i < 2) {
            l.type = LightType::Distant;
            l.intensity = 1.5f;
            l.angleDeg = 5.0f;
            Mat4 tilt = Mat4::identity(); // -Z tilted towards -X or +Y
            const float s = 0.5f, c = std::sqrt(1.0f - s * s);
            if (i == 0) {
                tilt.m[0][0] = c, tilt.m[0][2] = -s, tilt.m[2][0] = s, tilt.m[2][2] = c;
            } else {
                tilt.m[1][1] = c, tilt.m[1][2] = s, tilt.m[2][1] = -s, tilt.m[2][2] = c;
            }
            l.transform = tilt;
        } else {
            l.type = LightType::Sphere;
            l.radius = 0.1f;
            l.intensity = 4.0f;
            l.outsideTree = true;
            l.transform = translation({-1.0f + 0.7f * float(i), 0.8f - 0.5f * float(i % 2), 2.0f});
        }
        scene.updateLight(id, l);
    }
    for (int i : volumeOrder) {
        VolumeDesc v;
        v.name = "fog" + std::to_string(i);
        v.shell = boxShell({-0.8f + 0.5f * float(i), -0.9f, -0.6f}, {0.3f + 0.5f * float(i), 0.2f, 0.4f});
        v.material = media[size_t(i)];
        const VolumeId id = scene.createVolume(keyOf(keyed, "fog", i));
        scene.updateVolume(id, v);
        scene.updateVolumeMotion(id, {Mat4::identity()});
    }
    scene.commit();
}

std::vector<int> iota(int n) {
    std::vector<int> v(size_t(n), 0);
    for (int i = 0; i < n; ++i)
        v[size_t(i)] = i;
    return v;
}

struct Image {
    std::vector<Spectrum> color;
    std::vector<float> alpha;
    bool gpu = true;
    bool operator==(const Image &o) const {
        return color.size() == o.color.size() &&
               std::memcmp(color.data(), o.color.data(), color.size() * sizeof(Spectrum)) == 0 &&
               std::memcmp(alpha.data(), o.alpha.data(), alpha.size() * sizeof(float)) == 0;
    }
};

Image render(const Scene &scene, RenderDevice device) {
    Renderer renderer;
    RenderSettings settings;
    settings.device = device;
    settings.maxSamples = 8;
    settings.maxDepth = 4;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.5f, 24, 24));
    renderer.resize(24, 24);
    Image img;
    while (!renderer.converged()) {
        renderer.renderPass({});
        img.gpu = img.gpu && renderer.lastPassOnGpu();
    }
    img.color = renderer.film().colorSum;
    img.alpha = renderer.film().alphaSum;
    return img;
}

Image renderIn(const std::vector<int> &meshOrder, const std::vector<int> &lightOrder,
               const std::vector<int> &volumeOrder, bool keyed, RenderDevice device) {
    Scene scene;
    buildScene(scene, meshOrder, lightOrder, volumeOrder, keyed);
    return render(scene, device);
}

void checkOrderIndependence(RenderDevice device) {
    const std::vector<int> meshes = iota(kVeils + kEmitters), lights = iota(kLights), volumes = iota(kVolumes);
    std::vector<int> meshesR(meshes.rbegin(), meshes.rend()), lightsR(lights.rbegin(), lights.rend()),
        volumesR(volumes.rbegin(), volumes.rend());
    std::vector<int> meshesS = meshes, lightsS = lights;
    std::rotate(meshesS.begin(), meshesS.begin() + 3, meshesS.end());
    std::rotate(lightsS.begin(), lightsS.begin() + 1, lightsS.end());

    const Image a = renderIn(meshes, lights, volumes, true, device);
    if (device == RenderDevice::Xpu && !a.gpu) {
        SKIP("no usable GPU");
    }
    double sum = 0.0;
    for (const Spectrum &c : a.color)
        sum += double(c.x + c.y + c.z);
    REQUIRE(sum > 0.0);
    SECTION("keyed objects render the same image in any creation order") {
        CHECK(renderIn(meshesR, lightsR, volumesR, true, device) == a);
        CHECK(renderIn(meshesS, lightsS, volumes, true, device) == a);
        CHECK(renderIn(meshes, lightsR, volumes, true, device) == a);
        CHECK(renderIn(meshes, lights, volumesR, true, device) == a);
        CHECK(renderIn(meshesS, lights, volumes, true, device) == a);
    }
    SECTION("without keys the creation order shows") {
        const Image ids = renderIn(meshes, lights, volumes, false, device);
        CHECK(!(renderIn(meshesS, lights, volumes, false, device) == ids));
        CHECK(!(renderIn(meshes, lightsS, volumes, false, device) == ids));
        CHECK(!(renderIn(meshes, lights, volumesR, false, device) == ids));
    }
}

} // namespace

TEST_CASE("Images do not depend on the order objects are created in", "[render][determinism]") {
    checkOrderIndependence(RenderDevice::Cpu);
}

TEST_CASE("XPU images do not depend on the order objects are created in", "[gpu][render][determinism]") {
    checkOrderIndependence(RenderDevice::Xpu);
}

TEST_CASE("Objects keyed by name keep their keys, unkeyed ones key by id", "[determinism]") {
    CHECK(objectKey("/World/a") == objectKey("/World/a"));
    CHECK(objectKey("/World/a") != objectKey("/World/b"));
    CHECK(objectKey("/World/a", 0) != objectKey("/World/a", 1));
    CHECK(objectHash(7u) == 7u);
    // The per-strand random number follows the curve object's key, not its id.
    const auto strandRandom = [](bool keyed, int padding) {
        Scene scene;
        for (int i = 0; i < padding; ++i)
            scene.updateMesh(scene.createMesh(), makeQuad(0.1f));
        MeshDesc c;
        c.shape = MeshShape::Curves;
        c.curveBasis = CurveBasis::Linear;
        c.points = {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
        c.curveVertexCounts = {2};
        c.curveRadius = {0.05f, 0.05f};
        REQUIRE(prepareCurves(c));
        const MeshId id = scene.createMesh(keyed ? std::optional<uint64_t>(objectKey("/World/hair")) : std::nullopt);
        scene.updateMesh(id, c);
        scene.commit();
        return scene.resolveHit(id, 0, {0.5f, 0.0f}, 4.95f, 0, 0.0f, {0.0f, 0.5f, 5.0f}, {0.0f, 0.0f, -1.0f})
            .strandRandom;
    };
    CHECK(strandRandom(true, 0) == strandRandom(true, 3));
    CHECK(strandRandom(false, 0) != strandRandom(false, 3));
}
