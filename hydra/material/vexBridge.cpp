#include "vexBridge.h"

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/assetPath.h>

#include <filesystem>
#include <mutex>

#include <windows.h> // WIN32_LEAN_AND_MEAN and NOMINMAX come with the build

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// The directory of the module holding this code.
std::filesystem::path moduleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleDirectory), &module))
        return {};
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(module, path, MAX_PATH))
        return {};
    return std::filesystem::path(path).parent_path();
}

// Floats of a parameter value, `width` of them (false: not convertible).
bool floatsOf(VtValue const &v, int width, std::vector<float> &out) {
    out.assign(size_t(width), 0.0f);
    auto put = [&](auto const *data, int n) {
        for (int k = 0; k < width && k < n; ++k)
            out[size_t(k)] = float(data[k]);
        return true;
    };
    if (v.IsHolding<float>())
        return put(&v.UncheckedGet<float>(), 1);
    if (v.IsHolding<double>())
        return put(&v.UncheckedGet<double>(), 1);
    if (v.IsHolding<int>())
        return put(&v.UncheckedGet<int>(), 1);
    if (v.IsHolding<bool>()) {
        out[0] = v.UncheckedGet<bool>() ? 1.0f : 0.0f;
        return true;
    }
    if (v.IsHolding<GfVec2f>())
        return put(v.UncheckedGet<GfVec2f>().data(), 2);
    if (v.IsHolding<GfVec2d>())
        return put(v.UncheckedGet<GfVec2d>().data(), 2);
    if (v.IsHolding<GfVec3f>())
        return put(v.UncheckedGet<GfVec3f>().data(), 3);
    if (v.IsHolding<GfVec3d>())
        return put(v.UncheckedGet<GfVec3d>().data(), 3);
    if (v.IsHolding<GfVec4f>())
        return put(v.UncheckedGet<GfVec4f>().data(), 4);
    if (v.IsHolding<GfVec4d>())
        return put(v.UncheckedGet<GfVec4d>().data(), 4);
    if (v.IsHolding<GfMatrix3d>())
        return put(v.UncheckedGet<GfMatrix3d>().data(), 9);
    if (v.IsHolding<GfMatrix4d>())
        return put(v.UncheckedGet<GfMatrix4d>().data(), 16);
    return false;
}

bool isText(VtValue const &v) {
    return v.IsHolding<std::string>() || v.IsHolding<TfToken>() || v.IsHolding<SdfAssetPath>();
}

std::string textOf(VtValue const &v) {
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    if (v.IsHolding<SdfAssetPath>()) {
        SdfAssetPath const &a = v.UncheckedGet<SdfAssetPath>();
        return a.GetResolvedPath().empty() ? a.GetAssetPath() : a.GetResolvedPath();
    }
    return {};
}

// The values of `params` on node `shader`: authored, else the shader's
// defaults (`values` per float parameter, `strings` per string parameter).
void bindValues(HdNovaNetworkSource const &src, SdfPath const &shader, std::vector<nova::ExternalParam> const &params,
                std::vector<std::vector<float>> &values, std::vector<std::string> &strings) {
    const auto node = src.net.nodes.find(shader);
    values.assign(params.size(), {});
    strings.assign(params.size(), {});
    for (size_t i = 0; i < params.size(); ++i) {
        nova::ExternalParam const &p = params[i];
        VtValue const *authored = nullptr;
        if (node != src.net.nodes.end()) {
            const auto it = node->second.parameters.find(TfToken(p.name));
            if (it != node->second.parameters.end())
                authored = &it->second;
        }
        if (p.width == 0) {
            // An authored "" stays empty (the bridge passes it as is, as
            // Karma does); a value that is no text keeps the default.
            strings[i] = authored && isText(*authored) ? textOf(*authored) : p.stringDefault;
            continue;
        }
        if (!authored || !floatsOf(*authored, p.width, values[i]))
            values[i] = p.defaults;
        values[i].resize(size_t(p.width), 0.0f);
    }
}

// The primvar a parameter of node `shader` reads (D10): the varname of a
// UsdPrimvarReader connected to it, else the parameter's own name.
std::string primvarOf(HdNovaNetworkSource const &src, SdfPath const &shader, std::string const &param) {
    const auto node = src.net.nodes.find(shader);
    if (node == src.net.nodes.end())
        return param;
    const auto in = node->second.inputConnections.find(TfToken(param));
    if (in == node->second.inputConnections.end() || in->second.empty())
        return param;
    const auto up = src.net.nodes.find(in->second.front().upstreamNode);
    if (up == src.net.nodes.end() || !TfStringStartsWith(up->second.nodeTypeId.GetString(), "UsdPrimvarReader"))
        return param;
    const auto varname = up->second.parameters.find(TfToken("varname"));
    const std::string name = varname != up->second.parameters.end() ? textOf(varname->second) : std::string();
    return name.empty() ? param : name;
}

uint64_t hashBytes(uint64_t h, void const *p, size_t n) {
    auto const *b = static_cast<unsigned char const *>(p);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

} // namespace

nova::ExternalBridge *HdNovaCvexBridge(std::string *error) {
    static std::once_flag once;
    static nova::ExternalBridge *bridge = nullptr;
    static std::string failure;
    std::call_once(once, [] {
        const std::string env = TfGetenv("NOVA_CVEX");
        const std::filesystem::path path = env.empty() ? moduleDirectory() / "nova_cvex.dll" : std::filesystem::path(env);
        HMODULE dll = LoadLibraryW(path.wstring().c_str());
        if (!dll) {
            failure = "the CVEX bridge " + path.string() + " cannot be loaded";
            return;
        }
        auto factory = reinterpret_cast<nova::ExternalBridgeFactory>(GetProcAddress(dll, nova::kExternalBridgeEntry));
        bridge = factory ? factory() : nullptr;
        if (!bridge)
            failure = "the CVEX bridge " + path.string() + " has no entry point";
    });
    if (!bridge && error)
        *error = failure;
    return bridge;
}

bool HdNovaBuildExternal(HdNovaNetworkSource const &src, SdfPath const &shader, HdNovaVexShader const &vex,
                         HdNovaExternalMaterial *out, std::string *error) {
    nova::ExternalBridge *bridge = HdNovaCvexBridge(error);
    if (!bridge)
        return false;
    std::string compileError;
    std::shared_ptr<nova::ExternalShader> s = bridge->compileSurface(vex.code, &compileError);
    if (!s) {
        if (error)
            *error = compileError;
        return false;
    }
    auto binding = std::make_shared<nova::ExternalBinding>();
    binding->shader = s;
    binding->bounces = bridge->bounces();
    const std::vector<nova::ExternalParam> &params = s->params();
    binding->slots.assign(params.size(), nova::kNoExternalSlot);
    bindValues(src, shader, params, binding->values, binding->strings);

    *out = HdNovaExternalMaterial{};
    out->graph.model = nova::SurfaceModel::External;
    nova::shading::IrBuilder b(out->graph);
    // The parameters follow the named fields, so a material's opacity and
    // the other fields keep their defaults.
    uint16_t offset = nova::surface_field::kCount;
    for (size_t i = 0; i < params.size(); ++i) {
        nova::ExternalParam const &p = params[i];
        if (p.width == 0)
            continue;
        std::vector<float> const &value = binding->values[i];
        if (offset + p.width > int(nova::surface_field::kBlockSize))
            continue; // no room: uniform, the material's value
        // The primvar of the same name, else the material's value.
        uint32_t at = 0;
        const nova::shading::Ref fallback = b.newParam(p.width, &at);
        out->params.resize(at + uint32_t(p.width), 0.0f);
        for (int k = 0; k < p.width; ++k)
            out->params[at + uint32_t(k)] = value[size_t(k)];
        out->graph.outputs.push_back(
            {offset, uint8_t(p.width), b.primvar(primvarOf(src, shader, p.name), p.width, fallback)});
        binding->slots[i] = offset;
        offset = uint16_t(offset + p.width);
    }
    out->binding = std::move(binding);
    out->blockSize = offset;
    return true;
}

HdNovaDisplacementPtr HdNovaBuildExternalDisplacement(HdNovaNetworkSource const &src, std::string *error) {
    const auto terminal = src.net.terminals.find(HdMaterialTerminalTokens->displacement);
    if (terminal == src.net.terminals.end())
        return nullptr;
    SdfPath const &shader = terminal->second.upstreamNode;
    const auto info = src.nodeTypeInfo.find(shader);
    const HdNovaVexShader vex = info != src.nodeTypeInfo.end() ? HdNovaVexShaderOf(info->second) : HdNovaVexShader{};
    if (vex.kind != HdNovaVexKind::Code)
        return nullptr;
    nova::ExternalBridge *bridge = HdNovaCvexBridge(error);
    if (!bridge)
        return nullptr;
    std::shared_ptr<nova::ExternalDisplacer> d = bridge->compileDisplacement(vex.code, error);
    if (!d)
        return nullptr;
    auto out = std::make_shared<HdNovaDisplacement>();
    out->shader.external = d;
    bindValues(src, shader, d->params(), out->shader.externalValues, out->shader.externalStrings);
    for (nova::ExternalParam const &p : d->params())
        out->shader.externalPrimvars.push_back(p.width > 0 ? primvarOf(src, shader, p.name) : std::string());
    // The hash covers the code, the values and the primvar names.
    uint64_t h = hashBytes(1469598103934665603ull, vex.code.data(), vex.code.size());
    for (std::string const &s : out->shader.externalPrimvars)
        h = hashBytes(h, s.data(), s.size() + 1);
    for (std::vector<float> const &v : out->shader.externalValues)
        h = hashBytes(h, v.data(), v.size() * sizeof(float));
    for (std::string const &s : out->shader.externalStrings)
        h = hashBytes(h, s.data(), s.size() + 1);
    out->hash = h | 1;
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
