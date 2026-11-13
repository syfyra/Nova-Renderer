#include "registry.h"

#include "../diagnostics.h"

PXR_NAMESPACE_OPEN_SCOPE

nova::MaterialDesc HdNovaMaterialRegistry::DefaultMaterial(bool displayOpacity) const {
    return PlasticDefault() ? nova::plasticDefaultMaterial(displayOpacity) : nova::defaultMaterial(displayOpacity);
}

nova::MaterialId HdNovaMaterialRegistry::AcquireOpacityDefault(nova::Scene &scene) {
    std::lock_guard lock(_mutex);
    if (_opacityDefault == nova::kInvalidId) {
        _opacityDefault = scene.createMaterial();
        scene.updateMaterial(_opacityDefault, DefaultMaterial(true));
    }
    return _opacityDefault;
}

nova::MaterialId HdNovaMaterialRegistry::OpacityDefault() const {
    std::lock_guard lock(_mutex);
    return _opacityDefault;
}

nova::MaterialId HdNovaMaterialRegistry::Acquire(SdfPath const &path, nova::Scene &scene) {
    if (path.IsEmpty())
        return nova::kDefaultMaterialId;
    std::lock_guard lock(_mutex);
    const auto it = _ids.find(path);
    if (it != _ids.end())
        return it->second;
    const nova::MaterialId id = scene.createMaterial();
    scene.updateMaterial(id, DefaultMaterial());
    _ids.emplace(path, id);
    return id;
}

nova::shading::ProgramPtr HdNovaMaterialRegistry::Intern(nova::shading::ProgramPtr program) {
    if (!program)
        return program;
    std::lock_guard lock(_mutex);
    std::weak_ptr<const nova::shading::Program> &slot = _programs[program->structureHash];
    if (nova::shading::ProgramPtr existing = slot.lock())
        return existing;
    slot = program;
    return program;
}

void HdNovaMaterialRegistry::SetDisplacement(SdfPath const &material, HdNovaDisplacementPtr displacement) {
    std::lock_guard lock(_displacementMutex);
    HdNovaDisplacementPtr &slot = _displacements[material];
    const uint64_t before = slot ? slot->hash : 0, after = displacement ? displacement->hash : 0;
    slot = std::move(displacement);
    if (before == after)
        return;
    const auto it = _meshesOfMaterial.find(material);
    if (it != _meshesOfMaterial.end())
        _pendingMeshes.insert(it->second.begin(), it->second.end());
}

HdNovaDisplacementPtr HdNovaMaterialRegistry::GetDisplacement(SdfPath const &material) const {
    std::lock_guard lock(_displacementMutex);
    const auto it = _displacements.find(material);
    return it != _displacements.end() ? it->second : nullptr;
}

void HdNovaMaterialRegistry::BindMesh(SdfPath const &mesh, std::vector<SdfPath> const &materials) {
    std::lock_guard lock(_displacementMutex);
    std::vector<SdfPath> &bound = _materialsOfMesh[mesh];
    for (SdfPath const &m : bound)
        if (const auto it = _meshesOfMaterial.find(m); it != _meshesOfMaterial.end())
            it->second.erase(mesh);
    bound = materials;
    for (SdfPath const &m : bound)
        _meshesOfMaterial[m].insert(mesh);
    if (materials.empty())
        _materialsOfMesh.erase(mesh);
}

void HdNovaMaterialRegistry::SetVolumeFields(SdfPath const &material, std::vector<std::string> names) {
    std::lock_guard lock(_displacementMutex);
    std::vector<std::string> &slot = _volumeFields[material];
    if (slot == names)
        return;
    slot = std::move(names);
    const auto it = _meshesOfMaterial.find(material);
    if (it != _meshesOfMaterial.end())
        _pendingMeshes.insert(it->second.begin(), it->second.end());
}

std::vector<std::string> HdNovaMaterialRegistry::GetVolumeFields(SdfPath const &material) const {
    std::lock_guard lock(_displacementMutex);
    const auto it = _volumeFields.find(material);
    return it != _volumeFields.end() ? it->second : std::vector<std::string>{};
}

void HdNovaMaterialRegistry::SetMaterialKind(SdfPath const &material, HdNovaMaterialKind kind) {
    std::lock_guard lock(_displacementMutex);
    HdNovaMaterialKind &slot = _kinds[material];
    if (slot == kind)
        return;
    slot = kind;
    const auto it = _meshesOfMaterial.find(material);
    if (it != _meshesOfMaterial.end())
        _pendingMeshes.insert(it->second.begin(), it->second.end());
}

HdNovaMaterialKind HdNovaMaterialRegistry::GetMaterialKind(SdfPath const &material) const {
    std::lock_guard lock(_displacementMutex);
    const auto it = _kinds.find(material);
    return it != _kinds.end() ? it->second : HdNovaMaterialKind{};
}

std::vector<SdfPath> HdNovaMaterialRegistry::TakePendingMeshes() {
    std::lock_guard lock(_displacementMutex);
    std::vector<SdfPath> out(_pendingMeshes.begin(), _pendingMeshes.end());
    _pendingMeshes.clear();
    return out;
}

void HdNovaMaterialRegistry::Warn(SdfPath const &material, std::string const &message) {
    {
        std::lock_guard lock(_mutex);
        if (!_warned.insert(material.GetString() + "\n" + message).second)
            return;
    }
    HD_NOVA_WARN("material %s: %s", material.GetText(), message.c_str());
}

bool HdNovaMaterialRegistry::Warned(SdfPath const &material, std::string const &needle) const {
    std::lock_guard lock(_mutex);
    const std::string prefix = material.GetString() + "\n";
    for (std::string const &w : _warned)
        if (w.compare(0, prefix.size(), prefix) == 0 && w.find(needle, prefix.size()) != std::string::npos)
            return true;
    return false;
}

PXR_NAMESPACE_CLOSE_SCOPE
