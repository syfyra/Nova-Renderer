#include "field.h"

#include "diagnostics.h"

#include <pxr/base/tf/getenv.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/usd/sdf/assetPath.h>

#include <chrono>
#include <filesystem>

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

std::string pathOf(VtValue const &v) {
    if (v.IsHolding<SdfAssetPath>()) {
        // op: paths come back unresolved as well (research H 2.2)
        SdfAssetPath const &a = v.UncheckedGet<SdfAssetPath>();
        return a.GetResolvedPath().empty() ? a.GetAssetPath() : a.GetResolvedPath();
    }
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    return {};
}

std::string tokenOf(VtValue const &v) {
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    return {};
}

int intOf(VtValue const &v, int fallback) {
    if (v.IsHolding<int>())
        return v.UncheckedGet<int>();
    if (v.IsHolding<int64_t>())
        return int(v.UncheckedGet<int64_t>());
    return fallback;
}

} // namespace

// Houdini's libraries the bridge pulls in load onnxruntime.dll. Outside
// Houdini's bin directory (nova_cli, tests) Windows' own, older copy in
// System32 would be found first ("The requested API version ... is not
// available"); Houdini's, next to its USD libraries, is loaded first.
static void preloadHoudiniOnnxRuntime() {
    if (GetModuleHandleW(L"onnxruntime.dll"))
        return;
    HMODULE tf = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&TfGetenv), &tf))
        return;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(tf, path, MAX_PATH))
        return;
    const std::filesystem::path dll = std::filesystem::path(path).parent_path() / "onnxruntime.dll";
    std::error_code ec;
    if (std::filesystem::exists(dll, ec))
        LoadLibraryExW(dll.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

nova::VolumeBridge *HdNovaVolumeBridge(std::string *error) {
    static std::once_flag once;
    static nova::VolumeBridge *bridge = nullptr;
    static std::string failure;
    std::call_once(once, [] {
        preloadHoudiniOnnxRuntime();
        const std::string env = TfGetenv("NOVA_VOLUME_BRIDGE");
        const std::filesystem::path path =
            env.empty() ? moduleDirectory() / "nova_volume.dll" : std::filesystem::path(env);
        HMODULE dll = LoadLibraryW(path.wstring().c_str());
        if (!dll) {
            failure = "the volume bridge " + path.string() + " cannot be loaded";
            return;
        }
        auto factory = reinterpret_cast<nova::VolumeBridgeFactory>(GetProcAddress(dll, nova::kVolumeBridgeEntry));
        bridge = factory ? factory() : nullptr;
        if (!bridge)
            failure = "the volume bridge " + path.string() + " has no entry point";
    });
    if (!bridge && error)
        *error = failure;
    return bridge;
}

TfToken const &HdNovaField::HoudiniFieldAssetToken() {
    static const TfToken token("houdiniFieldAsset");
    return token;
}

TfToken const &HdNovaField::OpenVdbAssetToken() {
    static const TfToken token("openvdbAsset");
    return token;
}

HdNovaField::HdNovaField(SdfPath const &id, TfToken const &primType) : HdField(id), _primType(primType) {}

HdDirtyBits HdNovaField::GetInitialDirtyBitsMask() const { return AllDirty; }

void HdNovaField::Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *dirtyBits) {
    const SdfPath &id = GetId();
    nova::VolumeFieldRequest request;
    GfMatrix4d transform(1.0);
    if (*dirtyBits & DirtyParams) {
        request.filePath = pathOf(sd->Get(id, HdFieldTokens->filePath));
        request.fieldName = tokenOf(sd->Get(id, HdFieldTokens->fieldName));
        request.fieldIndex = intOf(sd->Get(id, TfToken("fieldIndex")), 0);
        request.houdiniField = _primType == HoudiniFieldAssetToken();
    }
    if (*dirtyBits & DirtyTransform)
        transform = sd->GetTransform(id);
    {
        std::lock_guard lock(_mutex);
        bool changed = false;
        if (*dirtyBits & DirtyParams) {
            changed |= request.filePath != _request.filePath || request.fieldName != _request.fieldName ||
                       request.fieldIndex != _request.fieldIndex || request.houdiniField != _request.houdiniField;
            _request = request;
        }
        if ((*dirtyBits & DirtyTransform) && transform != _transform) {
            changed = true;
            _transform = transform;
        }
        // A recooked SOP keeps its path when the frame does not change; a
        // params edit always reloads.
        if (changed || (*dirtyBits & DirtyParams)) {
            ++_version;
            _loaded = false;
            _grids.clear();
        }
    }
    *dirtyBits = Clean;
}

uint64_t HdNovaField::GetVersion() const {
    std::lock_guard lock(_mutex);
    return _version;
}

std::string HdNovaField::GetFieldName() const {
    std::lock_guard lock(_mutex);
    return _request.fieldName;
}

HdNovaField::Grids HdNovaField::Load() {
    std::lock_guard lock(_mutex);
    if (!_loaded) {
        _loaded = true;
        _grids.clear();
        if (_primType != HoudiniFieldAssetToken() && _primType != OpenVdbAssetToken()) {
            HD_NOVA_WARN("volume field %s: %s fields are not supported", GetId().GetText(), _primType.GetText());
        } else if (_request.filePath.empty()) {
            HD_NOVA_WARN("volume field %s has no file path", GetId().GetText());
        } else {
            std::string error;
            nova::VolumeBridge *bridge = HdNovaVolumeBridge(&error);
            nova::VolumeFieldGrids grids;
            const auto t0 = std::chrono::steady_clock::now();
            if (!bridge || !bridge->load(_request, grids, &error)) {
                HD_NOVA_WARN("volume field %s: %s", GetId().GetText(), error.c_str());
            } else {
                _grids = std::move(grids.grids);
                TF_DEBUG(HDNOVA_VOLUME)
                    .Msg("[Nova] field %s: '%s' of %s, %zu grid(s) in %.1f ms\n", GetId().GetText(),
                         _request.fieldName.c_str(), _request.filePath.c_str(), _grids.size(),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            }
        }
    }
    return {_grids, _request.fieldName, _transform, _version};
}

PXR_NAMESPACE_CLOSE_SCOPE
