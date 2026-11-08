#include "aov.h"

#include "diagnostics.h"

#include <pxr/imaging/hd/tokens.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

bool textValue(VtValue const &v, std::string &out) {
    if (v.IsHolding<TfToken>())
        out = v.UncheckedGet<TfToken>().GetString();
    else if (v.IsHolding<std::string>())
        out = v.UncheckedGet<std::string>();
    else
        return false;
    return true;
}

std::string setting(HdAovSettingsMap const &settings, char const *key) {
    std::string out;
    const auto it = settings.find(TfToken(key));
    if (it != settings.end())
        textValue(it->second, out);
    return out;
}

std::string withoutSpaces(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
    return s;
}

// Karma's ray:* and raw sources that are utility planes (conventions
// measured in M6.4, docs/design/M6.md).
bool utilityOf(std::string const &name, nova::UtilityRequest &out) {
    using U = nova::UtilityKind;
    static const std::pair<char const *, U> table[] = {
        {"ray:hitP", U::P},
        {"ray:hitP_camera", U::PCamera},
        {"ray:hitN", U::N}, // as authored (measured), unlike Hydra's normal
        {"ray:hitNg", U::Ng},
        {"ray:hitN_camera", U::NCamera},
        {"ray:hitNg_camera", U::NgCamera},
        {"ray:hitN_facingratio", U::FacingN},
        {"ray:hitNg_facingratio", U::FacingNg},
        {"ray:hitdist", U::HitDist},
        {"ray:primid", U::Face},  // Karma: the polygon of the gprim
        {"ray:element", U::Face}, // the same for meshes (measured)
        {"ray:motionvector", U::MotionVector},
        {"ray:velocity", U::Velocity},
        {"AO", U::Ao},
        {"export_basecolor", U::BaseColor},
    };
    for (auto const &[n, kind] : table)
        if (name == n) {
            out = {kind, {}};
            return true;
        }
    return false;
}

// The Karma Texture Baker's raw sources (husd/karmatexturebaker.py's AOV
// table): its maps, and export_<input> for the Standard Surface inputs.
bool bakeOf(std::string const &name, nova::UtilityRequest &out) {
    using U = nova::UtilityKind;
    static const std::pair<char const *, U> table[] = {
        {"Nt", U::BakeNormal},         {"Ds", U::BakeHeight},     {"Vd", U::BakeVector},
        {"Vdt", U::BakeVectorTangent}, {"Oc", U::BakeOcclusion},  {"Cv", U::BakeCavity},
        {"Th", U::BakeThickness},      {"Cu", U::BakeCurvature},  {"Eg", U::BakeEdge},
        {"Rn", U::BakeRoundedNormal},  {"Af", U::BakeAlpha},      {"__bake_P", U::BakePosition},
        {"N", U::BakeShadingNormal},   {"Ng", U::BakeGeometricNormal},
    };
    for (auto const &[n, kind] : table)
        if (name == n) {
            out = {kind, {}};
            return true;
        }
    if (name.rfind("export_", 0) == 0) {
        out = {U::BakeExport, name.substr(7)};
        return true;
    }
    return false;
}

// Hydra's names and Karma's ray:* sources Nova fills.
HdNovaAovKind namedKind(std::string const &name) {
    if (name == HdAovTokens->color.GetString())
        return HdNovaAovKind::Beauty;
    if (name == HdAovTokens->depth.GetString() || name == HdAovTokens->cameraDepth.GetString() ||
        name == "ray:hitPz")
        return HdNovaAovKind::Depth;
    if (name == HdAovTokens->primId.GetString() || name == "ray:objectid")
        return HdNovaAovKind::PrimId;
    if (name == HdAovTokens->instanceId.GetString())
        return HdNovaAovKind::InstanceId;
    if (name == HdAovTokens->normal.GetString())
        return HdNovaAovKind::Normal;
    if (name == "albedo")
        return HdNovaAovKind::Albedo;
    if (name == "ray:pixelsamples" || name == "nova:samplecount")
        return HdNovaAovKind::SampleCount;
    return HdNovaAovKind::None;
}

} // namespace

static HdNovaAovSource resolveAov(HdRenderPassAovBinding const &binding);

HdNovaAovSource HdNovaResolveAov(HdRenderPassAovBinding const &binding, bool bake) {
    HdNovaAovSource out = resolveAov(binding);
    if (!bake)
        return out;
    // A bake: the baker's raw sources are its maps; every plane is filtered.
    const std::string source = setting(binding.aovSettings, "sourceName");
    const std::string type = setting(binding.aovSettings, "sourceType");
    if ((type.empty() || type == "raw") && bakeOf(source, out.utility))
        out.kind = HdNovaAovKind::Utility;
    out.perSample = out.kind == HdNovaAovKind::Utility;
    return out;
}

static HdNovaAovSource resolveAov(HdRenderPassAovBinding const &binding) {
    HdNovaAovSource out;
    const std::string source = setting(binding.aovSettings, "sourceName");
    const std::string type = setting(binding.aovSettings, "sourceType");
    // An int var written as float: husk's own key, or the generic driver
    // parameter Karma honours as well (measured: Karma writes 0, 1, 2 for
    // ray:primid with either).
    std::string fileFormat = setting(binding.aovSettings, "driver:parameters:aov:husk:format");
    if (fileFormat.empty())
        fileFormat = setting(binding.aovSettings, "driver:parameters:aov:format");
    out.intAsFloat = !fileFormat.empty() && (fileFormat.rfind("float", 0) == 0 || fileFormat.rfind("half", 0) == 0);
    out.describe = binding.aovName.GetString();
    if (!source.empty())
        out.describe += " (" + (type.empty() ? std::string("raw") : type) + " " + source + ")";

    const std::string &name = binding.aovName.GetString();
    out.aovName = name;
    // Cryptomattes (Karma's driver parameters, with or without "karma:").
    const auto flag = [&](char const *key) {
        for (std::string prefix : {"driver:parameters:aov:karma:", "driver:parameters:aov:"}) {
            const auto it = binding.aovSettings.find(TfToken(prefix + key));
            if (it != binding.aovSettings.end())
                return it->second;
        }
        return VtValue();
    };
    const VtValue cryptomatte = flag("cryptomatte");
    if ((cryptomatte.IsHolding<bool>() && cryptomatte.UncheckedGet<bool>()) ||
        (cryptomatte.IsHolding<int>() && cryptomatte.UncheckedGet<int>() != 0)) {
        out.kind = HdNovaAovKind::Crypto;
        out.crypto = source == "__name"           ? nova::CryptoRequest{nova::CryptoSource::Object, {}}
                     : source == "__materialname" ? nova::CryptoRequest{nova::CryptoSource::Material, {}}
                                                  : nova::CryptoRequest{nova::CryptoSource::Primvar, source};
        const VtValue rank = VtValue::Cast<int>(flag("cryptomatterank"));
        if (rank.IsHolding<int>())
            out.cryptoRank = std::max(1, std::min(rank.UncheckedGet<int>(), 2 * 16));
        textValue(flag("cryptomattesidecar"), out.cryptoSidecar);
        return out;
    }
    if (source.empty() && name.rfind("lpe:", 0) == 0) { // Hydra's LPE names (HdAovTokensMakeLpe)
        out.expression = withoutSpaces(name.substr(4));
        out.kind = HdNovaAovKind::Lpe;
        if (out.expression == "C.*" || out.expression == "C.*[LO]" || out.expression == "C.*[OL]")
            out.kind = HdNovaAovKind::Beauty;
    } else if (source.empty()) { // Solaris' viewport: Hydra's AOV names
        out.kind = namedKind(name);
        if (out.kind == HdNovaAovKind::None) {
            using U = nova::UtilityKind;
            if (name == HdAovTokens->Peye.GetString())
                out.utility = {U::PCamera, {}}, out.kind = HdNovaAovKind::Utility;
            else if (name == HdAovTokens->Neye.GetString())
                out.utility = {U::NCamera, {}}, out.kind = HdNovaAovKind::Utility;
            else if (name == HdAovTokens->elementId.GetString())
                out.utility = {U::Face, {}}, out.kind = HdNovaAovKind::Utility;
            else if (name.rfind("primvars:", 0) == 0)
                out.utility = {U::Primvar, name.substr(9)}, out.kind = HdNovaAovKind::Utility;
        }
    } else if (type == "intrinsic" && source == "ray:pixelsamples") {
        out.kind = HdNovaAovKind::SampleCount;
    } else if (type == "primvar") {
        out.utility = {nova::UtilityKind::Primvar, source};
        out.kind = HdNovaAovKind::Utility;
    } else if (type == "lpe") {
        out.expression = withoutSpaces(source);
        if (out.expression == "C.*" || out.expression == "C.*[LO]" || out.expression == "C.*[OL]")
            out.kind = HdNovaAovKind::Beauty;
        else
            out.kind = HdNovaAovKind::Lpe;
    } else if (type.empty() || type == "raw") {
        out.kind = namedKind(source);
        if (out.kind == HdNovaAovKind::None && utilityOf(source, out.utility))
            out.kind = HdNovaAovKind::Utility;
        else if (out.kind == HdNovaAovKind::None && source.rfind("ray:", 0) != 0) {
            // Any other raw name is a shader AOV (kma_aov, Karma's exports);
            // materials without it leave 0, as in Karma.
            out.utility = {nova::UtilityKind::ShaderAov, source.rfind("export_", 0) == 0 ? source.substr(7) : source};
            out.kind = HdNovaAovKind::Utility;
        }
    }
    if (out.kind == HdNovaAovKind::None) {
        // Once per AOV and process: a viewport rebinds on every redraw.
        static std::mutex mutex;
        static std::set<std::string> warned;
        std::lock_guard lock(mutex);
        if (warned.insert(out.describe).second)
            HD_NOVA_WARN("AOV %s is not supported; it keeps its clear value", out.describe.c_str());
    }
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
