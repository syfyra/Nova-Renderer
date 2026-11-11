// nova_cli: renders a USD stage with the Nova Hydra delegate into EXR files,
// without Houdini, husk, Hgi or OpenGL. Used by tests and CI.
//
//   UsdStage -> UsdImagingCreateSceneIndices -> HdRenderIndex (scene index
//   emulation; applies the "Nova" scene index plugins) -> HdNovaRenderDelegate,
//   driven by HdEngine::Execute with one untracked HdTask (no hdx).
//
// hdNova is linked statically (hdNova_objects). Plug learns about it, and
// about the UsdImaging camera adapter that Houdini only registers through its
// HDK plugin USD_HD.dll, from plugInfo.json files with an empty LibraryPath in
// <exe dir>/usd_plugins (written by tools/nova_cli/CMakeLists.txt).
//
// Exit codes: 0 ok, 1 render or I/O failure, 2 usage/configuration error,
// 3 timeout before convergence.

#include "crash.h"
#include "sampler.h"

#include <pxr/pxr.h>

#include <pxr/base/arch/systemInfo.h>
#include <pxr/base/gf/range2f.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/warning.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/utils.h>
#include <pxr/imaging/hdsi/legacyDisplayStyleOverrideSceneIndex.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/spec.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <UT/UT_HUSDExtraAOVResource.h>

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr const char *kRendererPluginId = "HdNovaRendererPlugin";

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct Options {
    std::string scene;
    std::string output;              // -o: replaces the first product's name
    SdfPath settingsPath;            // --settings
    SdfPath cameraPath;              // --camera
    std::optional<double> frame;     // --frame
    GfVec2i resolution{0, 0};        // --res WxH
    int samples = 0;                 // --spp: nova:samples
    int threads = 0;                 // --threads
    double timeout = 0.0;            // --timeout seconds, 0 = none
    std::vector<std::string> purposes; // --purposes render,proxy,guide
    int refineLevel = 8;             // --complexity: husk's default "veryhigh"
    bool quiet = false;
    std::string profile;             // --profile: a sampling profile of the renders (sampler.h)
    std::string device;              // --device cpu|xpu: nova:device
};

void usage() {
    std::fprintf(stderr,
                 "usage: nova_cli scene.usd [-o out.exr] [--settings /Render/rs] [--camera /cam] [--frame F]\n"
                 "                [--res WxH] [--spp N] [--threads N] [--timeout S] [--purposes render,proxy,guide]\n"
                 "                [--complexity low|medium|high|veryhigh|N] [--quiet] [--profile report.txt]\n"
                 "                [--device cpu|xpu]\n");
}

bool parsePath(const char *text, SdfPath &out) {
    std::string err;
    if (!SdfPath::IsValidPathString(text, &err) || !SdfPath(text).IsAbsolutePath()) {
        // Git Bash turns "/World/cam" into "C:/Program Files/Git/World/cam":
        // run with MSYS_NO_PATHCONV=1.
        std::fprintf(stderr, "nova_cli: '%s' is not an absolute prim path\n", text);
        return false;
    }
    out = SdfPath(text);
    return true;
}

bool parseArgs(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        const char *v = nullptr;
        if (a == "-o" || a == "--output") {
            if (!(v = value())) return false;
            o.output = v;
        } else if (a == "-s" || a == "--settings") {
            if (!(v = value()) || !parsePath(v, o.settingsPath)) return false;
        } else if (a == "-c" || a == "--camera") {
            if (!(v = value()) || !parsePath(v, o.cameraPath)) return false;
        } else if (a == "-f" || a == "--frame") {
            if (!(v = value())) return false;
            o.frame = std::atof(v);
        } else if (a == "--res") {
            if (!(v = value()) || std::sscanf(v, "%dx%d", &o.resolution[0], &o.resolution[1]) != 2) return false;
        } else if (a == "--spp") {
            if (!(v = value())) return false;
            o.samples = std::atoi(v);
        } else if (a == "--threads") {
            if (!(v = value())) return false;
            o.threads = std::atoi(v);
        } else if (a == "--timeout") {
            if (!(v = value())) return false;
            o.timeout = std::atof(v);
        } else if (a == "--purposes") {
            if (!(v = value())) return false;
            o.purposes = TfStringSplit(v, ",");
        } else if (a == "--complexity") {
            // husk's mapping to HdDisplayStyle::refineLevel (measured in Houdini 22).
            if (!(v = value())) return false;
            const std::string c = v;
            if (c == "low")
                o.refineLevel = 0;
            else if (c == "medium")
                o.refineLevel = 2;
            else if (c == "high")
                o.refineLevel = 4;
            else if (c == "veryhigh")
                o.refineLevel = 8;
            else
                o.refineLevel = std::clamp(std::atoi(v), 0, 8);
        } else if (a == "-q" || a == "--quiet") {
            o.quiet = true;
        } else if (a == "--profile") {
            if (!(v = value())) return false;
            o.profile = v;
        } else if (a == "--device") {
            if (!(v = value())) return false;
            o.device = v;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "nova_cli: unknown option %s\n", a.c_str());
            return false;
        } else if (o.scene.empty()) {
            o.scene = a;
        } else {
            return false;
        }
    }
    return !o.scene.empty();
}

// ---------------------------------------------------------------------------
// Render settings -> products, AOVs and delegate settings
// ---------------------------------------------------------------------------

struct Aov {
    std::string partName;      // EXR part name
    TfToken aovName;           // binding name; RenderVar prim name, as in husk
    HdAovSettingsMap settings; // sourceName, sourceType, dataType, driver:parameters:*
    TfToken dataType;
    HdFormat format = HdFormatFloat32Vec4;
};

struct Product {
    std::string outputPath;
    SdfPath camera;
    GfVec2i resolution{1280, 720}; // husk's resolution without a RenderSettings prim
    float pixelAspectRatio = 1.0f;
    TfToken conformPolicy = UsdRenderTokens->expandAperture;
    GfRange2f dataWindowNDC{GfVec2f(0.0f), GfVec2f(1.0f)};
    std::vector<Aov> aovs;
};

struct RenderJob {
    std::vector<Product> products;
    HdRenderSettingsMap delegateSettings;
    TfTokenVector renderTags;
    UsdTimeCode time;
};

// RenderVar data types and husk's AOV format tokens (the menu of
// $HFS/houdini/soho/parameters/Husk_Aov.ds, plus Karma's float3h): component
// count and scalar type ('f' float, 'h' half, 'i' 32-bit int, 'x' an 8/16-bit
// integer type, which EXR cannot store and which keeps the data type's
// scalar).
struct FormatToken {
    const char *name;
    int components;
    char scalar;
};
const FormatToken *findFormatToken(std::string const &t) {
    static const FormatToken table[] = {
        {"float", 1, 'f'},     {"float2", 2, 'f'},    {"float3", 3, 'f'},    {"float4", 4, 'f'},
        {"color2f", 2, 'f'},   {"color3f", 3, 'f'},   {"color4f", 4, 'f'},   {"point3f", 3, 'f'},
        {"normal3f", 3, 'f'},  {"vector3f", 3, 'f'},  {"double", 1, 'f'},    {"double2", 2, 'f'},
        {"double3", 3, 'f'},   {"double4", 4, 'f'},   {"color3d", 3, 'f'},   {"color4d", 4, 'f'},
        {"point3d", 3, 'f'},   {"normal3d", 3, 'f'},  {"vector3d", 3, 'f'},  {"half", 1, 'h'},
        {"float16", 1, 'h'},   {"half2", 2, 'h'},     {"half3", 3, 'h'},     {"half4", 4, 'h'},
        {"color2h", 2, 'h'},   {"color3h", 3, 'h'},   {"color4h", 4, 'h'},   {"point3h", 3, 'h'},
        {"normal3h", 3, 'h'},  {"vector3h", 3, 'h'},  {"float2h", 2, 'h'},   {"float3h", 3, 'h'},
        {"float4h", 4, 'h'},   {"int", 1, 'i'},       {"int2", 2, 'i'},      {"int3", 3, 'i'},
        {"int4", 4, 'i'},      {"uint", 1, 'i'},      {"uint2", 2, 'i'},     {"uint3", 3, 'i'},
        {"uint4", 4, 'i'},     {"u8", 1, 'x'},        {"uint8", 1, 'x'},     {"color2u8", 2, 'x'},
        {"color3u8", 3, 'x'},  {"color4u8", 4, 'x'},  {"i8", 1, 'x'},        {"int8", 1, 'x'},
        {"color2i8", 2, 'x'},  {"color3i8", 3, 'x'},  {"color4i8", 4, 'x'},  {"u16", 1, 'x'},
        {"uint16", 1, 'x'},    {"color2u16", 2, 'x'}, {"color3u16", 3, 'x'}, {"color4u16", 4, 'x'},
        {"i16", 1, 'x'},       {"int16", 1, 'x'},     {"color2i16", 2, 'x'}, {"color3i16", 3, 'x'},
        {"color4i16", 4, 'x'},
    };
    for (const FormatToken &e : table)
        if (t == e.name)
            return &e;
    return nullptr;
}

// The buffer format of a RenderVar, as husk writes it: the component count of
// its dataType (what the renderer produces) and the scalar type of the file
// format token (driver:parameters:aov:husk:format, e.g. color4h or float16).
// Unknown tokens keep the data type's scalar; an unknown data type takes the
// token's count, else four floats.
HdFormat aovFormat(TfToken const &dataType, std::string const &fileFormat) {
    const FormatToken *type = findFormatToken(dataType.GetString());
    const FormatToken *file = fileFormat.empty() ? nullptr : findFormatToken(fileFormat);
    const int n = type ? type->components : file ? file->components : 4;
    char scalar = type && type->scalar != 'x' ? type->scalar : 'f';
    if (file && file->scalar != 'x')
        scalar = file->scalar;
    static const HdFormat floats[] = {HdFormatFloat32, HdFormatFloat32Vec2, HdFormatFloat32Vec3, HdFormatFloat32Vec4};
    static const HdFormat halves[] = {HdFormatFloat16, HdFormatFloat16Vec2, HdFormatFloat16Vec3, HdFormatFloat16Vec4};
    static const HdFormat ints[] = {HdFormatInt32, HdFormatInt32Vec2, HdFormatInt32Vec3, HdFormatInt32Vec4};
    return (scalar == 'h' ? halves : scalar == 'i' ? ints : floats)[n - 1];
}

std::string dictString(VtDictionary const &d, const char *key) {
    const auto it = d.find(key);
    if (it == d.end()) return {};
    if (it->second.IsHolding<std::string>()) return it->second.UncheckedGet<std::string>();
    if (it->second.IsHolding<TfToken>()) return it->second.UncheckedGet<TfToken>().GetString();
    return {};
}

SdfPath firstCamera(UsdStageRefPtr const &stage) {
    for (const UsdPrim &prim : stage->Traverse())
        if (prim.IsA<UsdGeomCamera>())
            return prim.GetPath();
    return SdfPath();
}

// The delegate settings husk passes: every RenderSettings attribute (schema
// fallbacks included) and relationship, plus a few husk keys.
void addSettingsPrim(UsdPrim const &prim, UsdTimeCode time, HdRenderSettingsMap &out) {
    for (const UsdAttribute &attr : prim.GetAttributes()) {
        VtValue v;
        if (attr.Get(&v, time))
            out[attr.GetName()] = v;
    }
    for (const UsdRelationship &rel : prim.GetRelationships()) {
        SdfPathVector targets;
        rel.GetForwardedTargets(&targets);
        out[rel.GetName()] = VtValue(targets);
    }
}

bool computeJob(UsdStageRefPtr const &stage, Options const &opt, RenderJob &job) {
    UsdRenderSettings settings;
    if (!opt.settingsPath.IsEmpty()) {
        settings = UsdRenderSettings(stage->GetPrimAtPath(opt.settingsPath));
        if (!settings) {
            std::fprintf(stderr, "nova_cli: no RenderSettings prim at %s\n", opt.settingsPath.GetText());
            return false;
        }
    } else {
        settings = UsdRenderSettings::GetStageRenderSettings(stage); // renderSettingsPrimPath metadata
    }

    // husk renders stage startTimeCode (0 when unauthored) unless told otherwise.
    job.time = UsdTimeCode(opt.frame ? *opt.frame : stage->GetStartTimeCode());

    Product base; // RenderSettings-level values, used by the default product
    VtArray<TfToken> purposes{UsdGeomTokens->default_, UsdGeomTokens->render};
    if (settings) {
        UsdRenderSettingsBase rs(settings.GetPrim());
        SdfPathVector cams;
        rs.GetCameraRel().GetForwardedTargets(&cams);
        if (!cams.empty()) base.camera = cams[0];
        rs.GetResolutionAttr().Get(&base.resolution);
        rs.GetPixelAspectRatioAttr().Get(&base.pixelAspectRatio);
        rs.GetAspectRatioConformPolicyAttr().Get(&base.conformPolicy);
        GfVec4f dw(0.0f, 0.0f, 1.0f, 1.0f);
        rs.GetDataWindowNDCAttr().Get(&dw);
        base.dataWindowNDC = GfRange2f(GfVec2f(dw[0], dw[1]), GfVec2f(dw[2], dw[3]));
        settings.GetIncludedPurposesAttr().Get(&purposes);
        addSettingsPrim(settings.GetPrim(), job.time, job.delegateSettings);
        job.delegateSettings[TfToken("usdSettingsPrim")] = VtValue(settings.GetPath());

        // Products and RenderVars. UsdRenderComputeSpec merges product
        // overrides over the settings and applies aspectRatioConformPolicy to
        // the pixel aspect ratio (adjustPixelAspectRatio).
        const UsdRenderSpec spec = UsdRenderComputeSpec(settings, /*namespaces=*/{});
        for (const UsdRenderSpec::Product &sp : spec.products) {
            Product p;
            p.outputPath = sp.name.GetString();
            p.camera = sp.cameraPath;
            p.resolution = sp.resolution;
            p.pixelAspectRatio = sp.pixelAspectRatio;
            p.conformPolicy = sp.aspectRatioConformPolicy;
            p.dataWindowNDC = sp.dataWindowNDC;
            for (size_t vi : sp.renderVarIndices) {
                const UsdRenderSpec::RenderVar &rv = spec.renderVars[vi];
                Aov a;
                a.aovName = rv.renderVarPath.GetNameToken();
                a.partName = dictString(rv.namespacedSettings, "driver:parameters:aov:name");
                if (a.partName.empty())
                    a.partName = dictString(rv.namespacedSettings, "driver:parameters:aov:husk:name");
                if (a.partName.empty())
                    a.partName = a.aovName.GetString();
                a.dataType = rv.dataType;
                // The file's pixel type, as husk: the format token Solaris'
                // Karma Render Settings write (driver:parameters:aov:husk:format,
                // e.g. color4h), else driver:parameters:aov:format.
                std::string fileFormat = dictString(rv.namespacedSettings, "driver:parameters:aov:husk:format");
                if (fileFormat.empty())
                    fileFormat = dictString(rv.namespacedSettings, "driver:parameters:aov:format");
                a.format = aovFormat(rv.dataType, fileFormat);
                a.settings[TfToken("sourceName")] = VtValue(TfToken(rv.sourceName));
                a.settings[TfToken("sourceType")] = VtValue(rv.sourceType);
                a.settings[TfToken("dataType")] = VtValue(rv.dataType);
                for (const auto &kv : rv.namespacedSettings)
                    a.settings[TfToken(kv.first)] = kv.second;
                p.aovs.push_back(std::move(a));
            }
            if (p.aovs.empty()) { // husk rejects it too ("No orderedVars")
                std::fprintf(stderr, "nova_cli: no RenderVars for product %s\n", p.outputPath.c_str());
                return false;
            }
            // An EXR's parts need unique names: said now, not after the render
            // when the file cannot be written.
            for (size_t i = 0; i < p.aovs.size(); ++i)
                for (size_t j = 0; j < i; ++j)
                    if (p.aovs[i].partName == p.aovs[j].partName) {
                        std::fprintf(stderr, "nova_cli: product %s has two RenderVars named '%s' (%s and %s)\n",
                                     p.outputPath.c_str(), p.aovs[i].partName.c_str(), p.aovs[j].aovName.GetText(),
                                     p.aovs[i].aovName.GetText());
                        return false;
                    }
            // husk writes the beauty "C" as the first part.
            std::stable_partition(p.aovs.begin(), p.aovs.end(), [](Aov const &a) { return a.partName == "C"; });
            job.products.push_back(std::move(p));
        }
        // Deprecated; husk does not pass it (Karma blurs as without it).
        job.delegateSettings.erase(UsdRenderTokens->instantaneousShutter);
    }

    if (job.products.empty()) {
        // husk's default product: half RGBA beauty "C".
        Product p = base;
        p.outputPath = "nova.exr";
        Aov a;
        a.partName = "C";
        a.aovName = HdAovTokens->color;
        a.dataType = TfToken("color4h");
        a.format = HdFormatFloat16Vec4;
        p.aovs.push_back(std::move(a));
        job.products.push_back(std::move(p));
    }

    for (Product &p : job.products) {
        if (!opt.cameraPath.IsEmpty()) p.camera = opt.cameraPath;
        if (p.camera.IsEmpty()) p.camera = firstCamera(stage);
        if (opt.resolution[0] > 0 && opt.resolution[1] > 0) p.resolution = opt.resolution;
        if (p.camera.IsEmpty() || p.resolution[0] <= 0 || p.resolution[1] <= 0) {
            std::fprintf(stderr, "nova_cli: product %s has no camera or resolution\n", p.outputPath.c_str());
            return false;
        }
        // adjustPixelAspectRatio stretches the pixels to the aperture's aspect
        // at the final resolution (also after --res and for the default product).
        if (p.conformPolicy == UsdRenderTokens->adjustPixelAspectRatio) {
            float apW = 0.0f, apH = 0.0f;
            if (const UsdGeomCamera cam{stage->GetPrimAtPath(p.camera)};
                cam && cam.GetHorizontalApertureAttr().Get(&apW, job.time) &&
                cam.GetVerticalApertureAttr().Get(&apH, job.time) && apW > 0.0f && apH > 0.0f)
                p.pixelAspectRatio = (apW / apH) / (float(p.resolution[0]) / float(p.resolution[1]));
        }
        // Values no image has: OpenEXR rejects the header of a pixel aspect
        // ratio that is not positive, and a NaN window has no pixels.
        if (!(std::isfinite(p.pixelAspectRatio) && p.pixelAspectRatio > 0.0f)) {
            std::fprintf(stderr, "nova_cli: product %s: pixelAspectRatio %g is not a positive number, using 1\n",
                         p.outputPath.c_str(), double(p.pixelAspectRatio));
            p.pixelAspectRatio = 1.0f;
        }
        // Likewise a window whose pixel coordinates leave int's range (the
        // image itself is the limit of what fits in memory long before).
        const GfVec2f lo = p.dataWindowNDC.GetMin(), hi = p.dataWindowNDC.GetMax();
        const auto pixels = [](float ndc, int size) { return std::fabs(double(ndc) * double(size)); };
        constexpr double kMaxPixel = 1e9;
        if (!std::isfinite(lo[0]) || !std::isfinite(lo[1]) || !std::isfinite(hi[0]) || !std::isfinite(hi[1]) ||
            pixels(lo[0], p.resolution[0]) > kMaxPixel || pixels(hi[0], p.resolution[0]) > kMaxPixel ||
            pixels(lo[1], p.resolution[1]) > kMaxPixel || pixels(hi[1], p.resolution[1]) > kMaxPixel) {
            std::fprintf(stderr, "nova_cli: product %s: dataWindowNDC (%g, %g, %g, %g) is no window of the image, "
                                 "using the whole image\n",
                         p.outputPath.c_str(), double(lo[0]), double(lo[1]), double(hi[0]), double(hi[1]));
            p.dataWindowNDC = GfRange2f(GfVec2f(0.0f), GfVec2f(1.0f));
        }
    }
    if (!opt.output.empty()) job.products.front().outputPath = opt.output;

    HdRenderSettingsMap &ds = job.delegateSettings;
    ds[TfToken("renderCameraPath")] = VtValue(job.products.front().camera);
    ds[TfToken("resolution")] = VtValue(job.products.front().resolution);
    ds[TfToken("houdini:frame")] = VtValue(job.time.IsNumeric() ? job.time.GetValue() : 0.0);
    ds[TfToken("houdini:fps")] = VtValue(stage->GetTimeCodesPerSecond());
    ds[TfToken("renderMode")] = VtValue(std::string("batch"));
    ds[HdRenderSettingsTokens->enableInteractive] = VtValue(false);
    if (opt.samples > 0) ds[TfToken("nova:samples")] = VtValue(opt.samples);
    if (!opt.device.empty()) ds[TfToken("nova:device")] = VtValue(TfToken(opt.device));
    // The render camera's shutter, as husk passes it: the first sync then
    // already samples motion over it.
    if (const UsdGeomCamera cam{stage->GetPrimAtPath(job.products.front().camera)}) {
        double open = 0.0, close = 0.0;
        if (cam.GetShutterOpenAttr().Get(&open, job.time) && cam.GetShutterCloseAttr().Get(&close, job.time)) {
            ds[TfToken("shutter:open")] = VtValue(open);
            ds[TfToken("shutter:close")] = VtValue(close);
        }
    }

    // includedPurposes -> render tags; USD "default" is Hydra's "geometry".
    // husk (Nova and Karma alike) renders "render" whenever includedPurposes
    // contains "default". --purposes replaces them: exactly the listed
    // purposes plus default geometry, like husk's --purpose geometry,<list>
    // (husk does not add "render" for its command-line flag).
    std::vector<std::string> names(purposes.begin(), purposes.end());
    if (!opt.purposes.empty()) {
        names = opt.purposes;
        names.push_back("default");
    } else if (std::find(names.begin(), names.end(), "default") != names.end()) {
        names.push_back("render");
    }
    for (const std::string &n : names) {
        const TfToken tag = n == "default" ? HdRenderTagTokens->geometry : TfToken(n);
        if (std::find(job.renderTags.begin(), job.renderTags.end(), tag) == job.renderTags.end())
            job.renderTags.push_back(tag);
    }
    return true;
}

// dataWindowNDC (y-up, [0,1]^2) -> inclusive pixel rect, y-down image rows.
// RenderMan crop-window rounding in y-up NDC (first = ceil(N*min),
// last = ceil(N*max) - 1), then flipped. Matches husk bit for bit:
// 640x427 (0.25,0.3,0.7,0.8) -> (160,85)-(447,297);
// 101x77 (0.1,0.25,0.55,0.61) -> (11,30)-(55,56).
// Not clamped: a window beyond [0, 1] is overscan, as in husk
// (160x107 (-0.1,-0.1,1.1,1.1) -> (-16,-11)-(175,116)).
GfRect2i dataWindowPixels(GfRange2f const &ndc, GfVec2i const &res) {
    const int w = res[0], h = res[1];
    const int x0 = int(std::ceil(float(w) * ndc.GetMin()[0]));
    const int x1 = std::max(x0, int(std::ceil(float(w) * ndc.GetMax()[0])) - 1);
    const int y0 = h - int(std::ceil(float(h) * ndc.GetMax()[1]));
    const int y1 = std::max(y0, h - 1 - int(std::ceil(float(h) * ndc.GetMin()[1])));
    return GfRect2i(GfVec2i(x0, y0), GfVec2i(x1, y1));
}

// The render buffers cover the display window and the data window (with
// overscan the data window reaches beyond the image).
GfRect2i bufferRect(GfRect2i const &dataWindow, GfVec2i const &res) {
    return GfRect2i(GfVec2i(std::min(dataWindow.GetMinX(), 0), std::min(dataWindow.GetMinY(), 0)),
                    GfVec2i(std::max(dataWindow.GetMaxX(), res[0] - 1), std::max(dataWindow.GetMaxY(), res[1] - 1)));
}

// ---------------------------------------------------------------------------
// The render task. Not added to the render index: SyncAll calls Sync with a
// null scene delegate on every Execute (HdRenderIndex::SyncAll, "untracked
// task"), which re-enqueues the collection so dirty rprims get synced.
// ---------------------------------------------------------------------------

class CliRenderTask final : public HdTask {
public:
    CliRenderTask(HdRenderPassSharedPtr pass, HdRenderPassStateSharedPtr state, TfTokenVector renderTags)
        : HdTask(SdfPath::EmptyPath()), _pass(std::move(pass)), _state(std::move(state)),
          _renderTags(std::move(renderTags)) {}

    void SetCameraPath(SdfPath const &path) { _cameraPath = path; }

    void Sync(HdSceneDelegate *, HdTaskContext *, HdDirtyBits *dirtyBits) override {
        _pass->Sync();
        *dirtyBits = HdChangeTracker::Clean;
    }
    void Prepare(HdTaskContext *, HdRenderIndex *index) override {
        // Sprims, including the camera, are synced before tasks are prepared.
        _state->SetCamera(static_cast<const HdCamera *>(index->GetSprim(HdPrimTypeTokens->camera, _cameraPath)));
        _state->Prepare(index->GetResourceRegistry());
    }
    void Execute(HdTaskContext *) override { _pass->Execute(_state, _renderTags); }
    bool IsConverged() const override { return _pass->IsConverged(); }
    const TfTokenVector &GetRenderTags() const override { return _renderTags; }

private:
    HdRenderPassSharedPtr _pass;
    HdRenderPassStateSharedPtr _state;
    TfTokenVector _renderTags;
    SdfPath _cameraPath;
};

// ---------------------------------------------------------------------------
// EXR output: one part per AOV (husk layout: "C" (R,G,B,A) first, "depth"
// (depth.Z), "N" (N.x,N.y,N.z), "albedo" (albedo.R,...), "primId" (primId.Z,
// uint)). The beauty, "C" or else the first color AOV, has bare channel names.
// ---------------------------------------------------------------------------

bool isColor(Aov const &a) { return a.dataType.GetString().rfind("color", 0) == 0; }

size_t beautyIndex(Product const &p) {
    for (size_t i = 0; i < p.aovs.size(); ++i)
        if (p.aovs[i].partName == "C")
            return i;
    for (size_t i = 0; i < p.aovs.size(); ++i)
        if (isColor(p.aovs[i]))
            return i;
    return p.aovs.size();
}

std::vector<std::string> channelNames(Aov const &a, int n, bool bare) {
    static const char *rgba[] = {"R", "G", "B", "A"};
    static const char *xyzw[] = {"x", "y", "z", "w"};
    const std::string &t = a.dataType.GetString();
    const bool vector = t.rfind("normal", 0) == 0 || t.rfind("vector", 0) == 0 || t.rfind("point", 0) == 0;
    const std::string prefix = bare ? std::string() : a.partName + ".";
    std::vector<std::string> names;
    if (n == 1)
        names.push_back(prefix + "Z");
    else
        for (int c = 0; c < n; ++c)
            names.push_back(prefix + (vector ? xyzw[c] : rgba[c]));
    return names;
}

// `dw` and `buffer` (the buffers' extent) in image pixels, y-down.
bool writeExr(Product const &p, GfRect2i const &dw, GfRect2i const &buffer, std::vector<HdRenderBuffer *> const &buffers,
              VtDictionary const &stats) {
    const int w = dw.GetWidth(), h = dw.GetHeight();
    const size_t beauty = beautyIndex(p);
    std::vector<OIIO::ImageSpec> specs;
    std::vector<std::vector<uint8_t>> pixels;
    std::map<std::string, std::string> metadata; // the buffers' extra metadata (cryptomatte)
    // The data window of a bottom-up buffer, top-down.
    const auto cropRows = [&](const uint8_t *src, size_t pixelSize, int bw, int bh) {
        std::vector<uint8_t> data(size_t(w) * size_t(h) * pixelSize);
        for (int y = 0; y < h; ++y) {
            const int row = bh - 1 - (dw.GetMinY() + y - buffer.GetMinY());
            std::memcpy(data.data() + size_t(y) * size_t(w) * pixelSize,
                        src + (size_t(row) * size_t(bw) + size_t(dw.GetMinX() - buffer.GetMinX())) * pixelSize,
                        size_t(w) * pixelSize);
        }
        return data;
    };
    const auto baseSpec = [&](int n, OIIO::TypeDesc type, std::string const &name) {
        OIIO::ImageSpec spec(w, h, n, type);
        spec.x = dw.GetMinX();
        spec.y = dw.GetMinY();
        spec.full_x = 0;
        spec.full_y = 0;
        spec.full_width = p.resolution[0];
        spec.full_height = p.resolution[1];
        spec.attribute("compression", "zip");
        spec.attribute("oiio:subimagename", name);
        spec.attribute("PixelAspectRatio", p.pixelAspectRatio);
        return spec;
    };
    for (size_t i = 0; i < p.aovs.size(); ++i) {
        HdRenderBuffer *rb = buffers[i];
        rb->Resolve();
        const HdFormat format = rb->GetFormat();
        const HdFormat component = HdGetComponentFormat(format);
        const int n = int(HdGetComponentCount(format));
        OIIO::TypeDesc type = OIIO::TypeDesc::FLOAT;
        if (component == HdFormatFloat16)
            type = OIIO::TypeDesc::HALF;
        else if (component == HdFormatInt32)
            type = OIIO::TypeDesc::UINT32; // EXR has no int32; keeps the bits (-1 -> 0xffffffff, as husk)
        else if (component != HdFormatFloat32)
            return false;

        // Hydra buffers are bottom-up, EXR scanlines top-down.
        const size_t pixelSize = HdDataSizeOfFormat(format);
        const int bw = int(rb->GetWidth()), bh = int(rb->GetHeight());
        std::vector<uint8_t> data = cropRows(static_cast<const uint8_t *>(rb->Map()), pixelSize, bw, bh);
        rb->Unmap();

        OIIO::ImageSpec spec = baseSpec(n, type, p.aovs[i].partName);
        spec.channelnames = channelNames(p.aovs[i], n, i == beauty);
        spec.alpha_channel = n == 4 ? 3 : -1;
        spec.attribute("oiio:ColorSpace", isColor(p.aovs[i]) ? "lin_rec709" : "raw");
        if (i == 0) {
            for (const auto &kv : stats) {
                if (kv.second.IsHolding<int>()) spec.attribute(kv.first, kv.second.UncheckedGet<int>());
                else if (kv.second.IsHolding<double>()) spec.attribute(kv.first, float(kv.second.UncheckedGet<double>()));
                else if (kv.second.IsHolding<std::string>()) spec.attribute(kv.first, kv.second.UncheckedGet<std::string>());
            }
        }
        specs.push_back(std::move(spec));
        pixels.push_back(std::move(data));

        // Planes the buffer affiliates (cryptomatte ranks), as husk writes
        // them: after the buffer's own part, the metadata into the header
        // of the first part (docs/research/m6/cryptomatte.md 4).
        const VtValue resource = rb->GetResource(true);
        if (!resource.IsHolding<HdAovSettingsMap>())
            continue;
        const HdAovSettingsMap &settings = resource.UncheckedGet<HdAovSettingsMap>();
        const auto entry = settings.find(TfToken("extra_aov_resource"));
        if (entry == settings.end() || !entry->second.IsHolding<std::shared_ptr<void>>())
            continue;
        const auto extra =
            std::static_pointer_cast<UT_HUSDExtraAOVResource>(entry->second.UncheckedGet<std::shared_ptr<void>>());
        for (size_t e = 0; e < extra->myNames.size(); ++e) {
            const HdFormat ef = HdFormat(extra->myFormats[e]);
            if (HdGetComponentFormat(ef) != HdFormatFloat32)
                continue;
            const int en = int(HdGetComponentCount(ef));
            std::vector<uint8_t> edata =
                cropRows(static_cast<const uint8_t *>(extra->myMap(int(e))), HdDataSizeOfFormat(ef), bw, bh);
            extra->myUnmap(int(e));
            OIIO::ImageSpec es = baseSpec(en, OIIO::TypeDesc::FLOAT, extra->myNames[e]);
            es.channelnames.clear();
            for (int c = 0; c < en; ++c)
                es.channelnames.push_back(extra->myNames[e] + "." + "RGBA"[c]);
            es.attribute("oiio:ColorSpace", "raw");
            specs.push_back(std::move(es));
            pixels.push_back(std::move(edata));
        }
        for (auto const &[key, value] : extra->myMetadata)
            metadata[key] = value;
    }
    // A manifest with a file name goes into that file next to the image.
    for (auto it = metadata.begin(); it != metadata.end();) {
        const std::string suffix = "/manif_file";
        if (it->first.size() < suffix.size() ||
            it->first.compare(it->first.size() - suffix.size(), suffix.size(), suffix)) {
            ++it;
            continue;
        }
        const std::string manifestKey = it->first.substr(0, it->first.size() - suffix.size()) + "/manifest";
        const auto manifest = metadata.find(manifestKey);
        if (manifest == metadata.end()) {
            ++it;
            continue;
        }
        // The product's directory, none for a bare name (the image goes to
        // the working directory; TfStringCatPaths("", x) would give "/x", the
        // drive's root). An absolute sidecar name stays as it is.
        // Plain UTF-8 strings: no conversion through the ANSI code page.
        const std::string dir = TfGetPathName(p.outputPath);
        const std::string path =
            dir.empty() || !TfIsRelativePath(it->second) ? it->second : TfStringCatPaths(dir, it->second);
        bool written = false;
        if (FILE *f = std::fopen(path.c_str(), "wb")) {
            written = std::fwrite(manifest->second.data(), 1, manifest->second.size(), f) == manifest->second.size();
            written = std::fclose(f) == 0 && written;
        }
        if (written) {
            metadata.erase(manifest);
            ++it;
        } else {
            // The manifest stays in the header, which then names no file.
            std::fprintf(stderr, "nova_cli: cannot write the cryptomatte sidecar %s; the manifest stays in %s\n",
                         path.c_str(), p.outputPath.c_str());
            it = metadata.erase(it);
        }
    }
    if (!specs.empty())
        for (auto const &[key, value] : metadata)
            specs[0].attribute(key, value);

    auto out = OIIO::ImageOutput::create(p.outputPath);
    if (!out) {
        std::fprintf(stderr, "nova_cli: %s\n", OIIO::geterror().c_str());
        return false;
    }
    if (specs.size() > 1 && !out->supports("multiimage")) {
        std::fprintf(stderr, "nova_cli: %s cannot hold %zu AOVs\n", p.outputPath.c_str(), specs.size());
        return false;
    }
    bool ok = out->open(p.outputPath, int(specs.size()), specs.data());
    for (size_t i = 0; ok && i < specs.size(); ++i) {
        if (i > 0)
            ok = out->open(p.outputPath, specs[i], OIIO::ImageOutput::AppendSubimage);
        ok = ok && out->write_image(specs[i].format, pixels[i].data());
    }
    ok = out->close() && ok;
    if (!ok)
        std::fprintf(stderr, "nova_cli: %s: %s\n", p.outputPath.c_str(), out->geterror().c_str());
    return ok;
}

// ---------------------------------------------------------------------------
// Diagnostics: Tf prints nothing itself once a delegate is installed. Nova's
// own warnings already went to stderr (HdNovaWarn writes them for husk,
// which drops Tf diagnostics), so they are not printed a second time.
// ---------------------------------------------------------------------------

class CliDiagnostics final : public TfDiagnosticMgr::Delegate {
public:
    void IssueStatus(TfStatus const &status) override { print("Status", status); }
    void IssueWarning(TfWarning const &warning) override {
        if (!TfStringStartsWith(warning.GetCommentary(), "Nova: "))
            print("Warning", warning);
    }
    void IssueError(TfError const &error) override {
        print("Error", error);
        // Where it was posted from (USD's errors name only their own line).
        static const bool stacks = std::getenv("NOVA_CLI_ERROR_STACKS") != nullptr;
        if (stacks)
            nova_cli::printCallStack();
    }
    void IssueFatalError(TfCallContext const &, std::string const &msg) override {
        std::fprintf(stderr, "Fatal error: %s\n", msg.c_str());
        _UnhandledAbort();
    }

private:
    static void print(const char *kind, TfDiagnosticBase const &d) {
        std::fprintf(stderr, "%s: %s\n", kind, d.GetCommentary().c_str());
    }
};

// ---------------------------------------------------------------------------
// Plugins
// ---------------------------------------------------------------------------

// Must run before the first use of HdRendererPluginRegistry,
// HdSceneIndexPluginRegistry or UsdImagingAdapterRegistry: they discover
// plugins once.
bool registerPlugins() {
    PlugRegistry &reg = PlugRegistry::GetInstance(); // also registers PXR_PLUGINPATH_NAME
    if (PlugPluginPtr other = reg.GetPluginWithName("hdNova")) {
        // A second copy of hdNova (an installed package on
        // PXR_PLUGINPATH_NAME) would define the same TfTypes again.
        std::fprintf(stderr, "nova_cli: PXR_PLUGINPATH_NAME contains another hdNova (%s); unset it\n",
                     other->GetPath().c_str());
        return false;
    }
    const std::string dir = TfStringCatPaths(TfGetPathName(ArchGetExecutablePath()), "usd_plugins");
    reg.RegisterPlugins(TfStringCatPaths(dir, "hdNova/resources/plugInfo.json"));
    if (!reg.GetPluginWithName("hdNova")) {
        std::fprintf(stderr, "nova_cli: %s/hdNova/resources/plugInfo.json is missing\n", dir.c_str());
        return false;
    }
    // Houdini 22 declares the UsdImaging Camera adapter only in the plugInfo
    // of its HDK plugin USD_HD.dll (as HD_CameraAdapter); without it cameras
    // get no Hydra prim type. Re-declare it unless a plugin already does.
    if (!reg.GetPluginForType(TfType::FindByName("UsdImagingCameraAdapter")))
        reg.RegisterPlugins(TfStringCatPaths(dir, "novaUsdImagingAdapters/resources/plugInfo.json"));
    if (!reg.GetPluginForType(TfType::FindByName("UsdImagingCameraAdapter"))) {
        std::fprintf(stderr, "nova_cli: no UsdImaging camera adapter (%s/novaUsdImagingAdapters missing)\n", dir.c_str());
        return false;
    }
    // Karma's USD schemas (Houdini's resource-only usdKarma plugin), so typed
    // Karma prims such as the KarmaSkyDomeLight render as in husk.
    if (!reg.GetPluginWithName("usdKarma"))
        reg.RegisterPlugins(TfStringCatPaths(dir, "usdKarma/resources/plugInfo.json"));
    return true;
}

} // namespace

int main(int argc, char **argv) {
    nova_cli::installCrashHandler();
    nova_cli::useUtf8Console();
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        usage();
        return 2;
    }
    if (opt.threads > 0)
        WorkSetConcurrencyLimitArgument(opt.threads); // before the delegate sizes its thread pool
    static CliDiagnostics diagnostics;
    TfDiagnosticMgr::GetInstance().AddDelegate(&diagnostics);
    if (!registerPlugins())
        return 2;

    UsdStageRefPtr stage = UsdStage::Open(opt.scene);
    if (!stage) {
        std::fprintf(stderr, "nova_cli: cannot open %s\n", opt.scene.c_str());
        return 2;
    }
    RenderJob job;
    if (!computeJob(stage, opt, job))
        return 2;

    // Through the registry, so the delegate gets its display name "Nova": the
    // render index appends the scene index plugins registered for "Nova" (and
    // for all renderers) only if the name is set.
    HdPluginRenderDelegateUniqueHandle delegate =
        HdRendererPluginRegistry::GetInstance().CreateRenderDelegate(TfToken(kRendererPluginId), job.delegateSettings);
    if (!delegate) {
        std::fprintf(stderr, "nova_cli: cannot create %s\n", kRendererPluginId);
        return 2;
    }
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(delegate.Get(), HdDriverVector(), "nova_cli"));

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    info.addDrawModeSceneIndex = false; // UsdRenderers.json: "drawmodesupport": false
    // The render complexity, as UsdImagingGLEngine and husk apply it: every
    // prim's display style gets the refine level (subdivision surfaces).
    info.overridesSceneIndexCallback = [&opt](HdSceneIndexBaseRefPtr const &input) -> HdSceneIndexBaseRefPtr {
        HdsiLegacyDisplayStyleOverrideSceneIndexRefPtr displayStyle = HdsiLegacyDisplayStyleOverrideSceneIndex::New(input);
        displayStyle->SetRefineLevelFallback(opt.refineLevel);
        return displayStyle;
    };
    UsdImagingSceneIndices sceneIndices = UsdImagingCreateSceneIndices(info);
    sceneIndices.stageSceneIndex->SetTime(job.time);
    index->InsertSceneIndex(sceneIndices.finalSceneIndex, SdfPath::AbsoluteRootPath(), /*needsPrefixing=*/false);

    const HdRprimCollection collection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined));
    HdRenderPassSharedPtr pass = delegate->CreateRenderPass(index.get(), collection);
    auto state = std::make_shared<HdRenderPassState>();
    auto task = std::make_shared<CliRenderTask>(pass, state, job.renderTags);
    HdTaskSharedPtrVector tasks{task};
    HdEngine engine;

    int status = 0;
    for (const Product &p : job.products) {
        const GfRect2i dw = dataWindowPixels(p.dataWindowNDC, p.resolution);
        const GfRect2i extent = bufferRect(dw, p.resolution);
        if (!opt.quiet)
            std::printf("%s: camera %s, %dx%d, data window (%d,%d)-(%d,%d), %zu AOV(s)\n", p.outputPath.c_str(),
                        p.camera.GetText(), p.resolution[0], p.resolution[1], dw.GetMinX(), dw.GetMinY(),
                        dw.GetMaxX(), dw.GetMaxY(), p.aovs.size());

        // Buffers over the display window (plus overscan) with the data window
        // as the framing's dataWindow: crop pixels keep their full-frame raster
        // positions (and sampler seeds), so crops reproduce full-frame renders.
        // Framing windows are relative to the buffer's top-left corner.
        const GfVec2f origin(float(extent.GetMinX()), float(extent.GetMinY()));
        std::vector<HdRenderBuffer *> buffers;
        HdRenderPassAovBindingVector bindings;
        bool allocated = true;
        for (size_t i = 0; i < p.aovs.size(); ++i) {
            const SdfPath id(TfStringPrintf("/__nova_cli/aov%zu", i));
            auto *rb = static_cast<HdRenderBuffer *>(delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, id));
            allocated = rb->Allocate(GfVec3i(extent.GetWidth(), extent.GetHeight(), 1), p.aovs[i].format, false) &&
                        allocated;
            buffers.push_back(rb);

            HdRenderPassAovBinding b;
            b.aovName = p.aovs[i].aovName;
            b.renderBuffer = rb;
            b.aovSettings = p.aovs[i].settings;
            TfToken source = b.aovName;
            if (const auto it = b.aovSettings.find(TfToken("sourceName"));
                it != b.aovSettings.end() && it->second.IsHolding<TfToken>())
                source = it->second.UncheckedGet<TfToken>();
            b.clearValue = delegate->GetDefaultAovDescriptor(source).clearValue; // empty = no clear
            bindings.push_back(std::move(b));
        }
        if (!allocated) { // the delegate said why
            std::fprintf(stderr, "nova_cli: product %s: no image of %dx%d pixels with %zu AOV(s)\n",
                         p.outputPath.c_str(), extent.GetWidth(), extent.GetHeight(), p.aovs.size());
            for (HdRenderBuffer *rb : buffers) {
                rb->Finalize(delegate->GetRenderParam());
                delegate->DestroyBprim(rb);
            }
            status = 2;
            break;
        }
        state->SetAovBindings(bindings);
        GfRect2i dataWindow = dw;
        dataWindow.Translate(GfVec2i(-extent.GetMinX(), -extent.GetMinY()));
        state->SetFraming(CameraUtilFraming(
            GfRange2f(-origin, GfVec2f(float(p.resolution[0]), float(p.resolution[1])) - origin), dataWindow,
            p.pixelAspectRatio));
        state->SetOverrideWindowPolicy(HdUtils::ToConformWindowPolicy(p.conformPolicy));
        task->SetCameraPath(p.camera);

        // Nova renders on its HdRenderThread; Execute only syncs and
        // (re)starts it. Poll like UsdAppUtilsFrameRecorder (10 -> 100 ms).
        NovaSampler sampler;
        if (!opt.profile.empty())
            sampler.start();
        const auto start = std::chrono::steady_clock::now();
        int sleepMs = 10, result = 0;
        for (;;) {
            engine.Execute(index.get(), &tasks);
            if (!state->GetCamera()) {
                std::fprintf(stderr, "nova_cli: camera %s is not in the render index\n", p.camera.GetText());
                result = 2;
                break;
            }
            if (task->IsConverged())
                break;
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (opt.timeout > 0.0 && elapsed > opt.timeout) {
                std::fprintf(stderr, "nova_cli: not converged after %.1f s\n", elapsed);
                result = 3;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            sleepMs = std::min(100, sleepMs + 5);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!opt.profile.empty() && !sampler.stop(opt.profile))
            std::fprintf(stderr, "nova_cli: cannot write %s\n", opt.profile.c_str());
        const VtDictionary stats = delegate->GetRenderStats();
        // nova:abortOnMissingTexture: the delegate reports an aborted render as
        // converged so hosts finish; its image is not written.
        if (result == 0 && dictString(stats, "rendererStage") == "Aborted") {
            std::fprintf(stderr, "nova_cli: the render was aborted (missing textures)\n");
            result = 4;
        }
        if (!opt.quiet && result == 0) {
            const auto rps = stats.find("raysPerSecond");
            const double mrays = rps != stats.end() && rps->second.IsHolding<double>()
                                     ? rps->second.UncheckedGet<double>() * 1e-6
                                     : 0.0;
            std::printf("  converged in %.3f s (%s, %.1f Mrays/s)\n", seconds,
                        dictString(stats, "renderProgressAnnotation").c_str(), mrays);
        }
        if (result == 0 && !writeExr(p, dw, extent, buffers, stats))
            result = 1;

        // Unbind before the buffers go away: an Execute without bindings
        // makes the pass stop the render thread and forget them.
        state->SetAovBindings({});
        engine.Execute(index.get(), &tasks);
        for (HdRenderBuffer *rb : buffers) {
            rb->Finalize(delegate->GetRenderParam());
            delegate->DestroyBprim(rb);
        }
        if (result != 0) {
            status = result;
            break;
        }
    }

    // Reverse order of construction (as UsdImagingGLEngine::_DestroyHydraObjects).
    tasks.clear();
    task.reset();
    pass.reset();
    index.reset();
    delegate = nullptr;
    sceneIndices = UsdImagingSceneIndices();
    return status;
}
