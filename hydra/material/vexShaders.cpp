#include "vexShaders.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/assetPath.h>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// The value of `name` or of `<sourceType>:name` (the universal one first).
VtValue const *find(VtDictionary const &info, std::string const &name) {
    const auto plain = info.find(name);
    if (plain != info.end())
        return &plain->second;
    const std::string suffix = ":" + name;
    for (auto const &[key, value] : info)
        if (TfStringEndsWith(key, suffix))
            return &value;
    return nullptr;
}

std::string text(VtValue const *v) {
    if (!v)
        return {};
    if (v->IsHolding<SdfAssetPath>())
        return v->UncheckedGet<SdfAssetPath>().GetAssetPath();
    if (v->IsHolding<std::string>())
        return v->UncheckedGet<std::string>();
    if (v->IsHolding<TfToken>())
        return v->UncheckedGet<TfToken>().GetString();
    return {};
}

} // namespace

HdNovaVexShader HdNovaVexShaderOf(VtDictionary const &typeInfo) {
    HdNovaVexShader out;
    const std::string source = text(find(typeInfo, "implementationSource"));
    if (source == "sourceCode") {
        out.code = text(find(typeInfo, "sourceCode"));
        if (!out.code.empty())
            out.kind = HdNovaVexKind::Code;
        return out;
    }
    if (source != "sourceAsset")
        return out;
    out.asset = text(find(typeInfo, "sourceAsset"));
    if (out.asset.empty())
        return out;
    static const std::string principled = "opdef:/Vop/principledshader::2.0?";
    if (TfStringStartsWith(out.asset, principled)) {
        const std::string section = out.asset.substr(principled.size());
        if (section == "SurfaceVexCode") {
            out.kind = HdNovaVexKind::PrincipledSurface;
            return out;
        }
        if (section == "DisplacementVexCode") {
            out.kind = HdNovaVexKind::PrincipledDisplacement;
            return out;
        }
    }
    out.kind = HdNovaVexKind::Asset;
    return out;
}

std::string HdNovaDescribeVexShader(HdNovaVexShader const &shader) {
    return shader.kind == HdNovaVexKind::Code ? std::string("VEX source code") : shader.asset;
}

PXR_NAMESPACE_CLOSE_SCOPE
