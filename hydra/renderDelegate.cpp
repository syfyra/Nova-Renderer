#include "renderDelegate.h"

#include "camera.h"
#include "diagnostics.h"
#include "field.h"
#include "instancer.h"
#include "light.h"
#include "lightFilter.h"
#include "material/material.h"
#include "sphere.h"
#include "curves.h"
#include "points.h"
#include "material/networkSource.h"
#include "material/nodeDefDatabase.h"
#include "mesh.h"
#include "primvarUtil.h"
#include "renderBuffer.h"
#include "renderPass.h"
#include "textureConvert.h"
#include "volume.h"

#include "core/film/lpe_compile.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/tokens.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <thread>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdNovaSettingsTokens, HDNOVA_SETTINGS_TOKENS);

namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

const TfTokenVector HdNovaRenderDelegate::SUPPORTED_RPRIM_TYPES = {
    HdPrimTypeTokens->mesh,
    HdPrimTypeTokens->sphere, // analytic (the implicit-surface scene index keeps spheres)
    HdPrimTypeTokens->basisCurves,
    HdPrimTypeTokens->points,
    HdPrimTypeTokens->volume,
};

const TfTokenVector HdNovaRenderDelegate::SUPPORTED_SPRIM_TYPES = {
    HdPrimTypeTokens->camera,
    HdPrimTypeTokens->material,
    HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->domeLight,
    HdPrimTypeTokens->sphereLight,
    HdPrimTypeTokens->rectLight,
    HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->cylinderLight,
    HdPrimTypeTokens->lightFilter, // docs/design/M11-P4.md
    HdPrimTypeTokens->light, // Karma's sky dome (docs/design/M11-P3.md)
};

const TfTokenVector HdNovaRenderDelegate::SUPPORTED_BPRIM_TYPES = {
    HdPrimTypeTokens->renderBuffer,
    // Volume fields (docs/design/M8.md D3); field3dAsset is accepted to warn.
    HdNovaField::OpenVdbAssetToken(),
    HdNovaField::HoudiniFieldAssetToken(),
    TfToken("field3dAsset"),
};

std::mutex HdNovaRenderDelegate::_mutexResourceRegistry;
std::atomic_int HdNovaRenderDelegate::_counterResourceRegistry;
HdResourceRegistrySharedPtr HdNovaRenderDelegate::_resourceRegistry;

HdNovaRenderDelegate::HdNovaRenderDelegate(nova::RenderDevice device)
    : HdRenderDelegate(), _device(device), _scene(int(WorkGetConcurrencyLimit())) {
    _Initialize();
}

HdNovaRenderDelegate::HdNovaRenderDelegate(HdRenderSettingsMap const &settingsMap, nova::RenderDevice device)
    : HdRenderDelegate(settingsMap), _device(device), _scene(int(WorkGetConcurrencyLimit())) {
    _Initialize();
}

void HdNovaRenderDelegate::_Initialize() {
    // Labels and defaults as in houdini/tools/nova_settings.json, the table of
    // Nova's Solaris UI (tests/houdini/test_solaris_ui.py checks the defaults).
    const nova::RenderSettings defaults;
    _settingDescriptors = {
        {"Samples", HdNovaSettingsTokens->samples, VtValue(int(defaults.maxSamples))},
        {"Max Ray Depth", HdNovaSettingsTokens->maxDepth, VtValue(64)},
        // Per-ray-type limits, Karma's defaults (docs/design/M4.md D13); the
        // same names as constant primvars override them per object.
        {"Diffuse Limit", HdNovaSettingsTokens->diffuseLimit, VtValue(1.0f)},
        {"Reflection Limit", HdNovaSettingsTokens->reflectLimit, VtValue(4.0f)},
        {"Refraction Limit", HdNovaSettingsTokens->refractLimit, VtValue(4.0f)},
        {"SSS Limit", HdNovaSettingsTokens->sssLimit, VtValue(1.0f)},
        {"Volume Limit", HdNovaSettingsTokens->volumeLimit, VtValue(0.0f)},
        {"Thin Walls at the Refraction Limit", HdNovaSettingsTokens->thinWallAtRefractLimit, VtValue(true)},
        {"Fake Caustics", HdNovaSettingsTokens->fakeCaustics, VtValue(false)},
        {"Clamp Indirect", HdNovaSettingsTokens->clampIndirect, VtValue(defaults.clampIndirect)},
        {"Seed", HdNovaSettingsTokens->seed, VtValue(int(defaults.seed))},
        // The device (docs/design/M10.md D1): the plugin's by default.
        {"Device", HdNovaSettingsTokens->device,
         VtValue(_device == nova::RenderDevice::Xpu ? HdNovaSettingsTokens->xpu : HdNovaSettingsTokens->cpu)},
        {"Pixel Filter", HdNovaSettingsTokens->pixelFilter, VtValue(HdNovaSettingsTokens->gauss)},
        {"Pixel Filter Scale", HdNovaSettingsTokens->pixelFilterScale, VtValue(defaults.pixelFilterScale)},
        {"Light Sampling", HdNovaSettingsTokens->lightStrategy, VtValue(HdNovaSettingsTokens->mis)},
        {"Light Selection", HdNovaSettingsTokens->lightSamplingMode, VtValue(HdNovaSettingsTokens->lighttree)},
        {"Max Motion Steps", HdNovaSettingsTokens->maxMotionSteps, VtValue(8)},
        {"IPR Proxy Passes", HdNovaSettingsTokens->iprProxyDepth, VtValue(2)},
        {"Pixel Oracle", HdNovaSettingsTokens->pixelOracle, VtValue(HdNovaSettingsTokens->uniform)},
        {"Minimum Samples", HdNovaSettingsTokens->oracleMinSamples, VtValue(-1)},
        {"Noise Threshold", HdNovaSettingsTokens->oracleThreshold, VtValue(defaults.oracleThreshold)},
        {"Denoise", HdNovaSettingsTokens->denoise, VtValue(false)},
        {"Denoised AOVs", HdNovaSettingsTokens->denoiseAovs, VtValue(std::string("C"))},
        {"Denoise the Viewport", HdNovaSettingsTokens->viewportDenoise, VtValue(false)},
        {"Denoise Start Passes", HdNovaSettingsTokens->denoiseStartPasses, VtValue(4)},
        {"Subdivision Level", HdNovaSettingsTokens->subdivLevel, VtValue(-1)},
        {"Max Triangles per Mesh", HdNovaSettingsTokens->subdivMaxTriangles, VtValue(8000000)},
        {"Dicing Quality Scale", HdNovaSettingsTokens->dicingQualityScale, VtValue(1.0f)},
        {"Off-screen Dicing Quality", HdNovaSettingsTokens->offscreenQuality, VtValue(0.1f)},
        {"Convert Textures", HdNovaSettingsTokens->textureAutoTx, VtValue(true)},
        {"Texture Cache Directory", HdNovaSettingsTokens->textureCacheDir, VtValue(std::string())},
        {"Texture Blur", HdNovaSettingsTokens->textureBlur, VtValue(0.0f)},
        {"Texture Filter Width", HdNovaSettingsTokens->textureFilterWidth, VtValue(0.25f)},
        {"Abort on Missing Texture", HdNovaSettingsTokens->abortOnMissingTexture, VtValue(false)},
        {"Texture Statistics", HdNovaSettingsTokens->textureStats, VtValue(0)},
        {"GPU Texture Budget", HdNovaSettingsTokens->gpuTextureBudget, VtValue(0)},
    };
    _PopulateDefaultSettings(_settingDescriptors);

    _renderer.setScene(&_scene);
    _renderParam = std::make_unique<HdNovaRenderParam>(&_scene, &_renderThread);
    // husk passes the render camera's shutter with the initial settings, so
    // the first sync already samples motion with it (the render pass keeps
    // it up to date afterwards).
    const bool disableMotionBlur = VtValue::Cast<bool>(GetRenderSetting(TfToken("disableMotionBlur"))).GetWithDefault(false);
    const bool instantaneous = VtValue::Cast<bool>(GetRenderSetting(TfToken("instantaneousShutter"))).GetWithDefault(false);
    _renderParam->SetMotionFlags(disableMotionBlur, instantaneous);
    _renderParam->SetMaxMotionSteps(GetMaxMotionSteps());
    _renderParam->SetSubdivSettings(GetSubdivLevel(), GetSubdivMaxTriangles());
    _renderParam->SetDicingSettings(GetDicingQualityScale(), GetOffscreenQuality());
    {
        float frame = 1.0f, fps = 24.0f;
        _FrameAndFps(frame, fps);
        _renderParam->SetFrame(frame, fps);
        _renderParam->TakeFrameChanged(); // nothing displaced yet
    }
    _renderParam->GetSurfaceTextures().SetOptions(GetTextureOptions());
    const VtValue open = GetRenderSetting(TfToken("shutter:open")), close = GetRenderSetting(TfToken("shutter:close"));
    if (!open.IsEmpty() && !close.IsEmpty())
        _renderParam->SetShutter(HdNovaResolveShutter(VtValue::Cast<double>(open).GetWithDefault(0.0),
                                                      VtValue::Cast<double>(close).GetWithDefault(0.0),
                                                      disableMotionBlur, instantaneous));
    // The MaterialX libraries load lazily with the first material; with the
    // debug output on they load here, so every run logs where they came from.
    if (TfDebug::IsEnabled(HDNOVA_MATERIAL))
        HdNovaNodeDefDatabase::Get();
    _renderThread.SetRenderCallback([this] { _RenderLoop(); });
    _renderThread.StartThread();

    std::lock_guard<std::mutex> guard(_mutexResourceRegistry);
    if (_counterResourceRegistry.fetch_add(1) == 0)
        _resourceRegistry = std::make_shared<HdResourceRegistry>();
}

HdNovaRenderDelegate::~HdNovaRenderDelegate() {
    _renderThread.StopThread();
    {
        std::lock_guard<std::mutex> guard(_mutexResourceRegistry);
        if (_counterResourceRegistry.fetch_sub(1) == 1)
            _resourceRegistry.reset();
    }
    _renderParam.reset();
}

// ---------------------------------------------------------------------------
// Render loop (runs on the HdRenderThread)
// ---------------------------------------------------------------------------

std::shared_ptr<const nova::lpe::Compiled>
HdNovaRenderDelegate::GetLpeProgram(HdRenderPassAovBindingVector const &bindings) {
    std::vector<std::string> expressions;
    std::vector<std::string> names; // the first AOV of each expression, for messages
    for (const HdRenderPassAovBinding &b : bindings) {
        const HdNovaAovSource s = HdNovaResolveAov(b);
        if (s.kind == HdNovaAovKind::Lpe &&
            std::find(expressions.begin(), expressions.end(), s.expression) == expressions.end()) {
            expressions.push_back(s.expression);
            names.push_back(s.describe);
        }
    }
    if (expressions == _lpeExpressions)
        return _lpeProgram;
    _lpeExpressions = expressions;
    _lpeProgram.reset();
    if (expressions.empty())
        return _lpeProgram;
    auto compiled = std::make_shared<nova::lpe::Compiled>(nova::lpe::compile(expressions));
    // Once per expression and process (a viewport rebinds often).
    static std::mutex mutex;
    static std::set<std::string> warned;
    std::lock_guard lock(mutex);
    for (size_t i = 0; i < expressions.size(); ++i) {
        if (!warned.insert(expressions[i]).second)
            continue;
        if (!compiled->errors[i].empty())
            HD_NOVA_WARN("AOV %s: %s; it keeps its clear value", names[i].c_str(), compiled->errors[i].c_str());
    }
    if (compiled->ok())
        _lpeProgram = std::move(compiled);
    return _lpeProgram;
}

std::shared_ptr<const std::vector<nova::UtilityRequest>>
HdNovaRenderDelegate::GetUtilityRequests(HdRenderPassAovBindingVector const &bindings) {
    std::vector<nova::UtilityRequest> requests;
    for (const HdRenderPassAovBinding &b : bindings) {
        const HdNovaAovSource s = HdNovaResolveAov(b);
        if (s.kind == HdNovaAovKind::Utility && std::find(requests.begin(), requests.end(), s.utility) == requests.end())
            requests.push_back(s.utility);
    }
    if (requests.empty())
        _utility.reset();
    else if (!_utility || *_utility != requests)
        _utility = std::make_shared<const std::vector<nova::UtilityRequest>>(std::move(requests));
    return _utility;
}

std::shared_ptr<const std::vector<nova::CryptoRequest>>
HdNovaRenderDelegate::GetCryptoRequests(HdRenderPassAovBindingVector const &bindings) {
    std::vector<nova::CryptoRequest> requests;
    for (const HdRenderPassAovBinding &b : bindings) {
        const HdNovaAovSource s = HdNovaResolveAov(b);
        if (s.kind == HdNovaAovKind::Crypto && std::find(requests.begin(), requests.end(), s.crypto) == requests.end())
            requests.push_back(s.crypto);
    }
    if (requests.empty())
        _crypto.reset();
    else if (!_crypto || *_crypto != requests)
        _crypto = std::make_shared<const std::vector<nova::CryptoRequest>>(std::move(requests));
    return _crypto;
}

void HdNovaRenderDelegate::SetAovBindings(HdRenderPassAovBindingVector const &bindings) {
    _aovBindings = bindings;
    _aovSources.clear();
    for (const HdRenderPassAovBinding &b : bindings) {
        HdNovaAovSource s = HdNovaResolveAov(b);
        if (s.kind == HdNovaAovKind::Lpe && _lpeProgram) {
            const auto it = std::find(_lpeExpressions.begin(), _lpeExpressions.end(), s.expression);
            const size_t i = size_t(it - _lpeExpressions.begin());
            if (it != _lpeExpressions.end() && _lpeProgram->errors[i].empty()) {
                s.output = int(i);
                s.invert = (_lpeProgram->outputFlags[i] & nova::lpe::OutputInvert) != 0;
                s.shadowSlot = _lpeProgram->shadowSlot(i);
                s.holdoutSlot = _lpeProgram->holdoutShadow(i) ? _lpeProgram->holdoutTotalSlot() : -1;
            }
        }
        if (s.intAsFloat)
            if (auto *rb = static_cast<HdNovaRenderBuffer *>(b.renderBuffer))
                rb->ReportFloatIds();
        if (s.kind == HdNovaAovKind::Crypto && _crypto) {
            const auto it = std::find(_crypto->begin(), _crypto->end(), s.crypto);
            s.cryptoIndex = it != _crypto->end() ? int(it - _crypto->begin()) : -1;
            if (auto *rb = static_cast<HdNovaRenderBuffer *>(b.renderBuffer))
                rb->SetCrypto(s);
        }
        if (s.kind == HdNovaAovKind::Utility && _utility) {
            const auto it = std::find(_utility->begin(), _utility->end(), s.utility);
            s.utilityIndex = it != _utility->end() ? int(it - _utility->begin()) : -1;
        }
        if (TfDebug::IsEnabled(HDNOVA_AOV)) {
            static const char *const kinds[] = {"none",   "beauty", "lpe",     "depth",   "primId",      "instanceId",
                                                "normal", "albedo", "utility", "crypto", "sampleCount"};
            std::string settings;
            for (auto const &[key, value] : b.aovSettings)
                settings += TfStringPrintf(" %s=%s", key.GetText(), TfStringify(value).c_str());
            const HdRenderBuffer *rb = b.renderBuffer;
            std::fprintf(stderr, "[Nova] AOV %s: format %d, clear %s ->%s%s%s,%s\n", b.aovName.GetText(),
                         rb ? int(rb->GetFormat()) : -1, TfStringify(b.clearValue).c_str(), kinds[size_t(s.kind)],
                         s.expression.empty() ? "" : " ", s.expression.c_str(), settings.c_str());
            std::fflush(stderr);
        }
        _aovSources.push_back(std::move(s));
    }
    // Karma's background image filter (M11 P2): the shadow catcher's alpha
    // from the bound shadow AOV; its plate composite is not applied.
    const auto text = [&](TfToken const &key) {
        const VtValue v = GetRenderSetting(key);
        return v.IsHolding<std::string>() ? v.UncheckedGet<std::string>()
               : v.IsHolding<TfToken>()   ? v.UncheckedGet<TfToken>().GetString()
                                          : std::string();
    };
    const HdNovaBackgroundFilter filter = HdNovaParseKarmaBackgroundFilter(text(HdNovaSettingsTokens->karmaImageFilter));
    if (filter.shadowCatcher) {
        int output = -1, slot = -1, holdoutSlot = -1;
        for (size_t i = 0; i < bindings.size(); ++i)
            if (bindings[i].aovName.GetString() == filter.shadows && _aovSources[i].kind == HdNovaAovKind::Lpe &&
                _aovSources[i].shadowSlot >= 0) {
                output = _aovSources[i].output;
                slot = _aovSources[i].shadowSlot;
                holdoutSlot = _aovSources[i].holdoutSlot;
            }
        for (size_t i = 0; i < bindings.size(); ++i)
            if (bindings[i].aovName.GetString() == filter.destination && _aovSources[i].kind == HdNovaAovKind::Beauty) {
                _aovSources[i].catcherOutput = output;
                _aovSources[i].catcherSlot = slot;
                _aovSources[i].catcherHoldoutSlot = holdoutSlot;
                _aovSources[i].catcherAlpha = filter.alpha;
            }
        if (output < 0) {
            static std::once_flag warned;
            std::call_once(warned, [&] {
                HD_NOVA_WARN("Karma's shadow catcher filter reads AOV '%s', which is not a bound shadow; expression; "
                             "the beauty's alpha keeps no shadows",
                             filter.shadows.c_str());
            });
        }
    }
    // The plate composite (docs/design/M11-P2.md D16): an image filter's, or
    // the Background Plate's IPR filter where karma:global:use_background is
    // On, or Auto (its default) in the viewport (Karma, measured).
    static const TfToken useBackground("karma:global:use_background"), iprFilter("karma:global:background_iprfilter");
    const std::string use = TfStringToLower(text(useBackground));
    const HdNovaBackgroundFilter ipr = HdNovaParseKarmaBackgroundFilter(text(iprFilter));
    const bool viewport = GetRenderSetting<bool>(HdNovaSettingsTokens->houdiniViewport, false);
    const HdNovaBackgroundFilter *comp = filter.plate                                           ? &filter
                                         : ipr.plate && (use == "on" || (use != "off" && viewport)) ? &ipr
                                                                                                     : nullptr;
    if (!comp) {
        _plate.reset();
        _plateKey.clear();
        return;
    }
    // The plate, read once per file version and placement.
    const std::string key = comp->plateFile + "|" + HdNovaTextureSignature(comp->plateFile) + "|" +
                            TfStringPrintf("%g %g %g %g", comp->scale[0], comp->scale[1], comp->translate[0],
                                           comp->translate[1]);
    if (key != _plateKey) {
        _plateKey = key;
        std::string error;
        _plate = HdNovaLoadPlate(*comp, error);
        if (!_plate)
            HD_NOVA_WARN("Karma's background plate %s cannot be read (%s); the composite has no plate",
                         comp->plateFile.c_str(), error.c_str());
    }
    const auto find = [&](std::string const &name) -> const HdNovaAovSource * {
        for (size_t i = 0; i < bindings.size(); ++i)
            if (bindings[i].aovName.GetString() == name && _aovSources[i].kind == HdNovaAovKind::Lpe &&
                _aovSources[i].output >= 0)
                return &_aovSources[i];
        return nullptr;
    };
    const HdNovaAovSource *shadows = comp->shadows.empty() ? nullptr : find(comp->shadows);
    if (!comp->shadows.empty() && (!shadows || shadows->shadowSlot < 0)) {
        static std::once_flag warned;
        std::call_once(warned, [&] {
            HD_NOVA_WARN("Karma's background plate composite reads shadows from AOV '%s', which is not a bound "
                         "shadow; expression; the plate stays unshadowed",
                         comp->shadows.c_str());
        });
        shadows = nullptr;
    }
    std::vector<int> extras;
    for (std::string const &name : comp->extras)
        if (const HdNovaAovSource *s = find(name))
            extras.push_back(s->output);
    for (size_t i = 0; i < bindings.size(); ++i)
        if (bindings[i].aovName.GetString() == comp->destination && _aovSources[i].kind == HdNovaAovKind::Beauty) {
            HdNovaAovSource &s = _aovSources[i];
            s.plate = _plate ? _plate : std::make_shared<const HdNovaPlate>(); // unread: black
            s.plateDecode = viewport; // Karma's viewport shows the plate as an sRGB image (measured)
            s.plateShadowOutput = shadows ? shadows->output : -1;
            s.plateShadowSlot = shadows ? shadows->shadowSlot : -1;
            s.plateHoldoutSlot = shadows ? shadows->holdoutSlot : -1;
            s.plateExtras = extras;
        }
}

HdNovaDenoiseConfig HdNovaRenderDelegate::GetDenoiseConfig() const {
    HdNovaDenoiseConfig config;
    const auto text = [&](TfToken const &key) {
        const VtValue v = GetRenderSetting(key);
        return v.IsHolding<std::string>() ? v.UncheckedGet<std::string>()
               : v.IsHolding<TfToken>()   ? v.UncheckedGet<TfToken>().GetString()
                                          : std::string();
    };
    if (GetRenderSetting<bool>(HdNovaSettingsTokens->denoise, false)) {
        config.final.enabled = true;
        const VtValue aovs = GetRenderSetting(HdNovaSettingsTokens->denoiseAovs);
        config.final.aovs = HdNovaSplitAovNames(aovs.IsEmpty() ? std::string("C") : text(HdNovaSettingsTokens->denoiseAovs));
    } else {
        // Karma's built-in image filter (the Karma Render Settings LOP's Denoiser).
        config.final = HdNovaParseKarmaImageFilter(text(HdNovaSettingsTokens->karmaImageFilter));
        if (config.final.enabled && config.final.engine == "oidn") {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
                HD_NOVA_WARN("Karma's denoiser asks for OIDN; Nova denoises with OptiX");
        }
    }
    config.viewport = GetRenderSetting<bool>(HdNovaSettingsTokens->houdiniViewport, false);
    config.viewportDenoise = GetRenderSetting<bool>(HdNovaSettingsTokens->viewportDenoise, false);
    config.startPasses = std::max(1, GetRenderSetting<int>(HdNovaSettingsTokens->denoiseStartPasses, 4));
    return config;
}

void HdNovaRenderDelegate::OnRenderStarted() {
    _completedSamples = 0;
    _completedPasses = 0;
    _targetSamples = _renderer.settings().maxSamples;
    _converged = false;
    // The missing-texture error is printed once per run of aborted renders,
    // again after a render that was not aborted.
    if (!_aborted.exchange(false))
        _abortReported = false;
    _renderStartNs = nowNs();
    _renderEndNs = 0;
}

void HdNovaRenderDelegate::_RenderLoop() {
    // IsStopRequested() may only be called from the render thread and is not
    // thread-safe, but tiles poll it from TBB workers: serialize the polls.
    std::mutex pollMutex;
    const auto stopRequested = [this, &pollMutex] {
        std::lock_guard<std::mutex> lock(pollMutex);
        return _renderThread.IsStopRequested();
    };
    // The first proxy pass after a restart is never interrupted: it is short
    // by design, and while the camera moves (or a value is dragged) every
    // viewport frame restarts the render, so each restart still shows one
    // image instead of none until the motion stops.
    const auto interrupted = [&] { return !_renderer.firstProxyPass() && stopRequested(); };

    for (;;) {
        if (interrupted()) {
            // Interrupted before converging: the next Execute restarts.
            if (!_renderer.converged())
                _needsReset = true;
            return;
        }
        if (_renderThread.IsPauseRequested()) {
            // The first-proxy-pass exemption is for a running pass only: a
            // paused loop must honour a stop request, since StopRender()
            // blocks until this callback returns.
            if (stopRequested()) {
                if (!_renderer.converged())
                    _needsReset = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (_renderer.converged())
            return;
        HdNovaTextureSystem &textures = _renderParam->GetSurfaceTextures();
        if (textures.GetOptions().abortOnMissing && textures.MissingCount() > 0) {
            if (!_abortReported) {
                std::fprintf(stderr, "[Nova] Error: %u texture(s) missing and nova:abortOnMissingTexture is on; "
                                     "the render is aborted\n",
                             textures.MissingCount());
                std::fflush(stderr);
                _abortReported = true;
            }
            // Hosts wait for the buffers to converge (husk, nova_cli): an
            // aborted render is finished. It does not restart by itself;
            // the stats report it as aborted. The state is published before
            // the buffers converge, so a host that sees them converged also
            // sees the abort.
            _aborted = true;
            _converged = true;
            _renderEndNs = nowNs();
            {
                auto lock = _renderThread.LockFramebuffer();
                for (const HdRenderPassAovBinding &binding : _aovBindings)
                    if (auto *rb = static_cast<HdNovaRenderBuffer *>(binding.renderBuffer))
                        rb->SetConverged(true);
            }
            return;
        }

        if (!_renderer.renderPass(interrupted)) {
            _needsReset = true; // the film holds a partial pass
            return;
        }

        auto lock = _renderThread.LockFramebuffer();
        const bool converged = _renderer.converged();
        // In parallel (the workers are idle now); the first-hit AOVs only
        // after the pass that produced them.
        const nova::TileRect region = _renderer.region();
        const bool firstHitAovs = _renderer.firstHitPass();
        // The beauty every pass; the other AOVs after the first pass, at the
        // end, and after a pass when a reader mapped them since their last
        // copy (the viewport's shown AOV, husk's monitor), docs/design/M6.md D11.
        // After a GPU pass the beauty too only then: the film comes back from
        // the device when read (docs/design/M10.md G7).
        const bool all = converged || firstHitAovs;
        const bool beautyEveryPass = !_renderer.lastPassOnGpu();
        // The viewport's denoised beauty (D3): after the start passes, at
        // most every 50 ms and always at the end; in between the displayed
        // beauty keeps the last denoised image.
        const int64_t now = nowNs();
        const bool viewportDenoise = _denoise.viewport && _denoise.viewportDenoise &&
                                     _renderer.fillStride() == 1 && _renderer.passes() >= uint32_t(_denoise.startPasses);
        const bool denoiseNow = viewportDenoise && (converged || now - _lastDenoiseNs >= 50000000);
        std::vector<HdNovaRenderBuffer *> copies(_aovBindings.size(), nullptr);
        for (size_t i = 0; i < _aovBindings.size(); ++i) {
            auto *rb = static_cast<HdNovaRenderBuffer *>(_aovBindings[i].renderBuffer);
            if (!rb)
                continue;
            const bool beauty = _aovSources[i].kind == HdNovaAovKind::Beauty;
            if (viewportDenoise && beauty) {
                // Only denoise passes copy (and denoise) the displayed beauty,
                // whether or not it was mapped: the passes in between must
                // not consume a reader's request (after GPU passes it would
                // rarely be pending at a denoise pass).
                if (denoiseNow) {
                    rb->TakeMapRequest();
                    copies[i] = rb;
                }
                continue;
            }
            if (rb->TakeMapRequest() || all || (beautyEveryPass && beauty))
                copies[i] = rb;
        }
        const bool anyCopy = std::any_of(copies.begin(), copies.end(), [](HdNovaRenderBuffer *rb) { return rb; });
        static const nova::Film noFilm;
        const nova::Film &film = anyCopy ? _renderer.film() : noFilm;
        if (anyCopy && film.samples == 0) {
            // Every pass adds to the film, so an empty one means its read-back
            // from the GPU failed and the render restarted on the CPU
            // (Renderer::syncFilm): nothing to copy or denoise, and the
            // converged state decided before the read-back no longer holds.
            for (const HdRenderPassAovBinding &binding : _aovBindings)
                if (auto *rb = static_cast<HdNovaRenderBuffer *>(binding.renderBuffer))
                    rb->SetConverged(false);
            _completedSamples = _renderer.samplesDone();
            _completedPasses = _renderer.passes();
            continue;
        }
        _renderer.parallelRows(region.y0, region.y1, [&](int y0, int y1) {
            const nova::TileRect rows{region.x0, y0, region.x1, y1};
            for (size_t i = 0; i < copies.size(); ++i)
                if (copies[i])
                    copies[i]->WriteFromFilm(film, rows, _aovSources[i], firstHitAovs, _renderer.fillStride(),
                                             region.x0, region.y0);
        });
        // Denoising: the viewport's beauty, or the final frame's listed AOVs.
        std::vector<HdNovaRenderBuffer *> denoised;
        bool guidesAlbedo = true, guidesNormal = true;
        if (denoiseNow) {
            for (size_t i = 0; i < _aovBindings.size(); ++i)
                if (_aovSources[i].kind == HdNovaAovKind::Beauty && copies[i])
                    denoised.push_back(copies[i]);
            _lastDenoiseNs = now;
        } else if (converged && !_denoise.viewport && _denoise.final.enabled) {
            for (size_t i = 0; i < _aovBindings.size(); ++i) {
                const std::string &name = _aovBindings[i].aovName.GetString();
                const HdNovaAovKind kind = _aovSources[i].kind;
                if ((kind == HdNovaAovKind::Beauty || kind == HdNovaAovKind::Lpe) && copies[i] &&
                    std::find(_denoise.final.aovs.begin(), _denoise.final.aovs.end(), name) != _denoise.final.aovs.end())
                    denoised.push_back(copies[i]);
            }
            guidesAlbedo = _denoise.final.albedo;
            guidesNormal = _denoise.final.normal;
        }
        if (!denoised.empty())
            HdNovaDenoiseBuffers(film, denoised, guidesAlbedo, guidesNormal);
        // The stats first: a host that sees the buffers converged reads the
        // final counts and the "Converged" stage.
        _completedSamples = _renderer.samplesDone();
        _completedPasses = _renderer.passes();
        if (converged) {
            _converged = true;
            _renderEndNs = nowNs();
        }
        for (const HdRenderPassAovBinding &binding : _aovBindings)
            if (auto *rb = static_cast<HdNovaRenderBuffer *>(binding.renderBuffer))
                rb->SetConverged(converged);
        if (converged) {
            if (const int level = _renderParam->GetSurfaceTextures().GetOptions().stats; level > 0)
                _renderParam->GetSurfaceTextures().PrintStats(level);
        }
    }
}

// ---------------------------------------------------------------------------
// Settings / stats
// ---------------------------------------------------------------------------

nova::RenderSettings HdNovaRenderDelegate::GetNovaSettings() const {
    nova::RenderSettings s;
    s.maxSamples = uint32_t(std::max(1, GetRenderSetting<int>(HdNovaSettingsTokens->samples, int(s.maxSamples))));
    s.maxDepth = uint32_t(std::max(1, GetRenderSetting<int>(HdNovaSettingsTokens->maxDepth, 64)));
    {
        const TfToken limits[nova::kLimitClasses] = {HdNovaSettingsTokens->diffuseLimit, HdNovaSettingsTokens->reflectLimit,
                                                     HdNovaSettingsTokens->refractLimit, HdNovaSettingsTokens->sssLimit,
                                                     HdNovaSettingsTokens->volumeLimit};
        const float defaults[nova::kLimitClasses] = {1.0f, 4.0f, 4.0f, 1.0f, 0.0f};
        for (int k = 0; k < nova::kLimitClasses; ++k) {
            const float v = VtValue::Cast<float>(GetRenderSetting(limits[k])).GetWithDefault(defaults[k]);
            s.limits.limit[k] = std::isfinite(v) ? std::max(v, 0.0f) : defaults[k];
        }
        // Karma's limits stand in for Nova's while those are at their
        // defaults (docs/design/M8.md D16, M9): pyro and groom setups author
        // them on the render settings.
        static const TfToken karmaLimits[nova::kLimitClasses] = {
            TfToken("karma:object:diffuselimit"), TfToken("karma:object:reflectlimit"),
            TfToken("karma:object:refractlimit"), TfToken("karma:object:ssslimit"), TfToken("karma:object:volumelimit")};
        for (int k = 0; k < nova::kLimitClasses; ++k) {
            const float karma = VtValue::Cast<float>(GetRenderSetting(karmaLimits[k])).GetWithDefault(-1.0f);
            if (s.limits.limit[k] == defaults[k] && std::isfinite(karma) && karma >= 0.0f)
                s.limits.limit[k] = karma;
        }
        s.limits.thinWallAtRefractLimit =
            VtValue::Cast<bool>(GetRenderSetting(HdNovaSettingsTokens->thinWallAtRefractLimit)).GetWithDefault(true);
        s.limits.fakeCaustics =
            VtValue::Cast<bool>(GetRenderSetting(HdNovaSettingsTokens->fakeCaustics)).GetWithDefault(false);
    }
    // Ray visibility and holdouts of objects without their own (M11 P2):
    // Karma's object defaults on the render settings (the holdout mode as its
    // menu label there), and whether background holdouts occlude shadows.
    {
        static const TfToken visibility("karma:object:rendervisibility"), holdout("karma:object:holdoutmode"),
            bgOcclude("karma:global:bgoccludeshadow");
        s.objectVisibility.mask = int16_t(HdNovaRenderVisibilityValue(GetRenderSetting(visibility), "render settings"));
        s.objectVisibility.holdout = int8_t(HdNovaHoldoutModeValue(GetRenderSetting(holdout), "render settings"));
        s.backgroundOccludesShadows = VtValue::Cast<bool>(GetRenderSetting(bgOcclude)).GetWithDefault(false);
    }
    // Light selection (docs/design/M9.md D11, D18): the light tree, unless
    // Nova's light sampling mode asks for Uniform ("uniform" or 0), or while
    // it is at its default, Karma's light_sampling_mode.
    s.lightTree = true;
    for (const TfToken &key : {HdNovaSettingsTokens->lightSamplingMode, TfToken("karma:global:light_sampling_mode")}) {
        const VtValue mode = GetRenderSetting(key);
        if (mode.IsEmpty())
            continue;
        const std::string text = mode.IsHolding<TfToken>()     ? mode.UncheckedGet<TfToken>().GetString()
                                 : mode.IsHolding<std::string>() ? mode.UncheckedGet<std::string>()
                                                                 : std::string();
        if (!text.empty())
            s.lightTree = TfStringToLower(text).find("uniform") == std::string::npos;
        else
            s.lightTree = VtValue::Cast<int>(mode).GetWithDefault(1) != 0;
        if (!s.lightTree)
            break;
    }
    // The device (docs/design/M10.md D1): "xpu" renders on the GPU, the CPU
    // being its fallback; "cpu" on the CPU; otherwise the plugin's.
    {
        const VtValue device = GetRenderSetting(HdNovaSettingsTokens->device);
        const std::string text = TfStringToLower(device.IsHolding<TfToken>()       ? device.UncheckedGet<TfToken>().GetString()
                                                 : device.IsHolding<std::string>() ? device.UncheckedGet<std::string>()
                                                                                   : std::string());
        s.device = text == "xpu" ? nova::RenderDevice::Xpu : text == "cpu" ? nova::RenderDevice::Cpu : _device;
    }
    s.gpuTextureBudget = uint32_t(std::max(0, GetRenderSetting<int>(HdNovaSettingsTokens->gpuTextureBudget, 0)));
    s.clampIndirect = std::max(0.0f, GetRenderSetting<float>(HdNovaSettingsTokens->clampIndirect, s.clampIndirect));
    s.seed = uint32_t(GetRenderSetting<int>(HdNovaSettingsTokens->seed, int(s.seed)));

    // USD render settings deliver the filter name as a token or a string.
    // The names and sizes follow Karma's karma:global:pixelfilter.
    const VtValue filter = GetRenderSetting(HdNovaSettingsTokens->pixelFilter);
    const TfToken name = filter.IsHolding<TfToken>()       ? filter.UncheckedGet<TfToken>()
                         : filter.IsHolding<std::string>() ? TfToken(filter.UncheckedGet<std::string>())
                                                           : HdNovaSettingsTokens->gauss;
    if (name == HdNovaSettingsTokens->gauss || name == HdNovaSettingsTokens->gaussian)
        s.pixelFilter = nova::PixelFilterType::Gaussian;
    else if (name == HdNovaSettingsTokens->box)
        s.pixelFilter = nova::PixelFilterType::Box;
    else if (name == HdNovaSettingsTokens->blackman)
        s.pixelFilter = nova::PixelFilterType::Blackman;
    else if (name == HdNovaSettingsTokens->blackmanharris)
        s.pixelFilter = nova::PixelFilterType::BlackmanHarris;
    else if (name == HdNovaSettingsTokens->triangle)
        s.pixelFilter = nova::PixelFilterType::Triangle;
    else if (name == HdNovaSettingsTokens->point)
        s.pixelFilter = nova::PixelFilterType::Point;
    else if (name != _unknownPixelFilter) { // warn once per value, settings are read every frame
        HD_NOVA_WARN("unknown pixel filter '%s', using gauss", name.GetText());
        _unknownPixelFilter = name;
    }
    const float scale = GetRenderSetting<float>(HdNovaSettingsTokens->pixelFilterScale, s.pixelFilterScale);
    s.pixelFilterScale = std::isfinite(scale) ? std::clamp(scale, 0.0f, 16.0f) : s.pixelFilterScale;

    // Debugging aid: sample direct light with one strategy only.
    const VtValue strategy = GetRenderSetting(HdNovaSettingsTokens->lightStrategy);
    const TfToken strategyName = strategy.IsHolding<TfToken>()       ? strategy.UncheckedGet<TfToken>()
                                 : strategy.IsHolding<std::string>() ? TfToken(strategy.UncheckedGet<std::string>())
                                                                     : HdNovaSettingsTokens->mis;
    s.lightStrategy = strategyName == HdNovaSettingsTokens->light  ? nova::LightStrategy::LightOnly
                      : strategyName == HdNovaSettingsTokens->bsdf ? nova::LightStrategy::BsdfOnly
                                                                   : nova::LightStrategy::MIS;

    _FrameAndFps(s.frame, s.fps);
    s.textureFilterWidth = GetTextureOptions().filterWidth;

    // Adaptive sampling (docs/design/M7.md D4).
    const VtValue oracle = GetRenderSetting(HdNovaSettingsTokens->pixelOracle);
    const std::string oracleName = oracle.IsHolding<TfToken>()       ? oracle.UncheckedGet<TfToken>().GetString()
                                   : oracle.IsHolding<std::string>() ? oracle.UncheckedGet<std::string>()
                                                                     : std::string();
    s.adaptive = oracleName == HdNovaSettingsTokens->variance.GetString();
    const int minSamples = GetRenderSetting<int>(HdNovaSettingsTokens->oracleMinSamples, -1);
    s.oracleMinSamples = minSamples > 0 ? uint32_t(minSamples) : 0u;
    s.oracleThreshold = std::max(0.0f, GetRenderSetting<float>(HdNovaSettingsTokens->oracleThreshold, s.oracleThreshold));
    return s;
}

void HdNovaRenderDelegate::_FrameAndFps(float &frame, float &fps) const {
    // The frame and the frame rate (husk, nova_cli and Houdini set them).
    auto number = [this](char const *key, double fallback) {
        const VtValue v = GetRenderSetting(TfToken(key));
        if (v.IsHolding<double>())
            return v.UncheckedGet<double>();
        if (v.IsHolding<float>())
            return double(v.UncheckedGet<float>());
        if (v.IsHolding<int>())
            return double(v.UncheckedGet<int>());
        return fallback;
    };
    frame = float(number("houdini:frame", 1.0));
    const float rate = float(number("houdini:fps", 24.0));
    fps = rate > 0.0f ? rate : 24.0f;
}

void HdNovaRenderDelegate::SetRenderSetting(TfToken const &key, VtValue const &value) {
    HdRenderDelegate::SetRenderSetting(key, value);
    // Displacement evaluates the Time and Frame globals at sync, which runs
    // before the render pass reads the settings: meshes rebuilt in the next
    // sync (animated points) displace at the new frame. The render pass
    // re-syncs the other displaced meshes that read the time.
    static const TfToken frameKey("houdini:frame"), fpsKey("houdini:fps");
    if (_renderParam && (key == frameKey || key == fpsKey)) {
        float frame = 1.0f, fps = 24.0f;
        _FrameAndFps(frame, fps);
        _renderParam->SetFrame(frame, fps);
    }
}

unsigned HdNovaRenderDelegate::GetMaxMotionSteps() const {
    const int steps = GetRenderSetting<int>(HdNovaSettingsTokens->maxMotionSteps, 8);
    return unsigned(std::clamp(steps, 2, int(nova::kMaxMotionSteps)));
}

HdNovaVelocityDefaults HdNovaRenderDelegate::GetVelocityDefaults() const {
    static const TfToken vblur("karma:object:vblur"), instanceVblur("karma:object:instance_vblur"),
        geoSamples("karma:object:geosamples"), instanceSamples("karma:object:instance_samples");
    HdNovaVelocityDefaults d;
    const int mode = HdNovaVelocityBlurMode(GetRenderSetting(vblur));
    const int instanceMode = HdNovaVelocityBlurMode(GetRenderSetting(instanceVblur));
    d.velocityBlur = mode >= 0 ? mode : d.velocityBlur;
    d.instanceVelocityBlur = instanceMode >= 0 ? instanceMode : d.instanceVelocityBlur;
    auto count = [this](TfToken const &key, int fallback) {
        const VtValue v = VtValue::Cast<double>(GetRenderSetting(key));
        return v.IsHolding<double>() && std::isfinite(v.UncheckedGet<double>())
                   ? int(std::clamp(std::lround(v.UncheckedGet<double>()), 1l, 64l))
                   : fallback;
    };
    d.geoSamples = count(geoSamples, d.geoSamples);
    d.instanceSamples = count(instanceSamples, d.instanceSamples);
    float frame = 1.0f, fps = 24.0f;
    _FrameAndFps(frame, fps);
    d.timeCodesPerSecond = fps;
    return d;
}

int HdNovaRenderDelegate::GetSubdivLevel() const {
    return std::clamp(GetRenderSetting<int>(HdNovaSettingsTokens->subdivLevel, -1), -1, 6);
}

uint64_t HdNovaRenderDelegate::GetSubdivMaxTriangles() const {
    return uint64_t(std::max(1, GetRenderSetting<int>(HdNovaSettingsTokens->subdivMaxTriangles, 8000000)));
}

float HdNovaRenderDelegate::GetDicingQualityScale() const {
    const float q = GetRenderSetting<float>(HdNovaSettingsTokens->dicingQualityScale, 1.0f);
    return std::isfinite(q) ? std::clamp(q, 1e-3f, 64.0f) : 1.0f;
}

float HdNovaRenderDelegate::GetOffscreenQuality() const {
    const float q = GetRenderSetting<float>(HdNovaSettingsTokens->offscreenQuality, 0.1f);
    return std::isfinite(q) ? std::clamp(q, 1e-3f, 1.0f) : 0.1f;
}

HdNovaTextureOptions HdNovaRenderDelegate::GetTextureOptions() const {
    HdNovaTextureOptions o;
    o.autoTx = VtValue::Cast<bool>(GetRenderSetting(HdNovaSettingsTokens->textureAutoTx)).GetWithDefault(true);
    const VtValue dir = GetRenderSetting(HdNovaSettingsTokens->textureCacheDir);
    o.cacheDir = dir.IsHolding<std::string>() ? dir.UncheckedGet<std::string>()
                 : dir.IsHolding<TfToken>()   ? dir.UncheckedGet<TfToken>().GetString()
                                              : std::string();
    const float blur = GetRenderSetting<float>(HdNovaSettingsTokens->textureBlur, 0.0f);
    o.blur = std::isfinite(blur) ? std::clamp(blur, 0.0f, 1.0f) : 0.0f;
    const float width = GetRenderSetting<float>(HdNovaSettingsTokens->textureFilterWidth, 0.25f);
    o.filterWidth = std::isfinite(width) ? std::clamp(width, 0.0f, 16.0f) : 0.25f;
    o.abortOnMissing =
        VtValue::Cast<bool>(GetRenderSetting(HdNovaSettingsTokens->abortOnMissingTexture)).GetWithDefault(false);
    o.stats = std::clamp(GetRenderSetting<int>(HdNovaSettingsTokens->textureStats, 0), 0, 5);
    return o;
}

HdRenderSettingDescriptorList HdNovaRenderDelegate::GetRenderSettingDescriptors() const {
    return _settingDescriptors;
}

VtDictionary HdNovaRenderDelegate::GetRenderStats() const {
    const uint32_t samples = _completedSamples;
    const uint32_t target = std::max(1u, _targetSamples.load());
    const double fraction = std::min(1.0, double(samples) / double(target));
    const int64_t start = _renderStartNs, end = _renderEndNs;
    const double seconds = start ? double((end ? end : nowNs()) - start) * 1e-9 : 0.0;

    VtDictionary stats;
    stats["rendererName"] = VtValue(std::string("Nova"));
    stats["percentDone"] = VtValue(100.0 * fraction);
    stats["fractionDone"] = VtValue(fraction);
    stats["samples"] = VtValue(int(samples));
    stats["passes"] = VtValue(int(_completedPasses.load())); // proxy passes included
    stats[HdPerfTokens->numCompletedSamples.GetString()] = VtValue(int(samples));
    stats["totalClockTime"] = VtValue(seconds);
    stats["rendererStage"] = VtValue(std::string(_aborted     ? "Aborted"
                                                 : _converged ? "Converged"
                                                              : (_paused ? "Paused" : "Rendering")));
    stats["renderProgressAnnotation"] = VtValue(std::to_string(samples) + "/" + std::to_string(target) + " spp");
    // Throughput of the current render (the film and the counter restart together).
    const double rays = double(_renderer.raysTraced());
    stats["rays"] = VtValue(rays);
    stats["raysPerSecond"] = VtValue(seconds > 0.0 ? rays / seconds : 0.0);
    return stats;
}

// ---------------------------------------------------------------------------
// Render control
// ---------------------------------------------------------------------------

bool HdNovaRenderDelegate::Pause() {
    _paused = true;
    _renderThread.PauseRender();
    return true;
}

bool HdNovaRenderDelegate::Resume() {
    _paused = false;
    _renderThread.ResumeRender();
    return true;
}

bool HdNovaRenderDelegate::Stop(bool /*blocking*/) {
    std::lock_guard<std::mutex> lock(_controlMutex);
    _stoppedByHost = true;
    _renderThread.StopRender(); // blocks until the render callback returns
    return true;
}

bool HdNovaRenderDelegate::Restart() {
    std::lock_guard<std::mutex> lock(_controlMutex);
    _stoppedByHost = false;
    _needsReset = true; // the render pass restarts on its next Execute
    return true;
}

// ---------------------------------------------------------------------------
// Prim factories
// ---------------------------------------------------------------------------

const TfTokenVector &HdNovaRenderDelegate::GetSupportedRprimTypes() const { return SUPPORTED_RPRIM_TYPES; }
const TfTokenVector &HdNovaRenderDelegate::GetSupportedSprimTypes() const { return SUPPORTED_SPRIM_TYPES; }
const TfTokenVector &HdNovaRenderDelegate::GetSupportedBprimTypes() const { return SUPPORTED_BPRIM_TYPES; }

HdRenderParam *HdNovaRenderDelegate::GetRenderParam() const { return _renderParam.get(); }
HdResourceRegistrySharedPtr HdNovaRenderDelegate::GetResourceRegistry() const { return _resourceRegistry; }

HdRenderPassSharedPtr HdNovaRenderDelegate::CreateRenderPass(HdRenderIndex *index,
                                                             HdRprimCollection const &collection) {
    return std::make_shared<HdNovaRenderPass>(index, collection, this);
}

HdInstancer *HdNovaRenderDelegate::CreateInstancer(HdSceneDelegate *delegate, SdfPath const &id) {
    return new HdNovaInstancer(delegate, id);
}

void HdNovaRenderDelegate::DestroyInstancer(HdInstancer *instancer) { delete instancer; }

HdRprim *HdNovaRenderDelegate::CreateRprim(TfToken const &typeId, SdfPath const &rprimId) {
    if (typeId == HdPrimTypeTokens->sphere)
        return new HdNovaSphere(rprimId);
    if (typeId == HdPrimTypeTokens->mesh)
        return new HdNovaMesh(rprimId);
    if (typeId == HdPrimTypeTokens->basisCurves)
        return new HdNovaBasisCurves(rprimId);
    if (typeId == HdPrimTypeTokens->points)
        return new HdNovaPoints(rprimId);
    if (typeId == HdPrimTypeTokens->volume)
        return new HdNovaVolume(rprimId);
    TF_CODING_ERROR("Nova: unknown Rprim type %s", typeId.GetText());
    return nullptr;
}

void HdNovaRenderDelegate::DestroyRprim(HdRprim *rPrim) { delete rPrim; }

HdSprim *HdNovaRenderDelegate::CreateSprim(TfToken const &typeId, SdfPath const &sprimId) {
    if (typeId == HdPrimTypeTokens->camera)
        return new HdNovaCamera(sprimId);
    if (typeId == HdPrimTypeTokens->material)
        return new HdNovaMaterial(sprimId);
    if (typeId == HdPrimTypeTokens->distantLight || typeId == HdPrimTypeTokens->domeLight ||
        typeId == HdPrimTypeTokens->sphereLight || typeId == HdPrimTypeTokens->rectLight ||
        typeId == HdPrimTypeTokens->diskLight || typeId == HdPrimTypeTokens->cylinderLight ||
        typeId == HdPrimTypeTokens->light)
        return new HdNovaLight(sprimId, typeId);
    if (typeId == HdPrimTypeTokens->lightFilter)
        return new HdNovaLightFilter(sprimId);
    TF_CODING_ERROR("Nova: unknown Sprim type %s", typeId.GetText());
    return nullptr;
}

HdSprim *HdNovaRenderDelegate::CreateFallbackSprim(TfToken const &typeId) {
    return CreateSprim(typeId, SdfPath::EmptyPath());
}

void HdNovaRenderDelegate::DestroySprim(HdSprim *sprim) { delete sprim; }

HdBprim *HdNovaRenderDelegate::CreateBprim(TfToken const &typeId, SdfPath const &bprimId) {
    if (typeId == HdPrimTypeTokens->renderBuffer)
        return new HdNovaRenderBuffer(bprimId, _renderParam.get());
    if (typeId == HdNovaField::OpenVdbAssetToken() || typeId == HdNovaField::HoudiniFieldAssetToken() ||
        typeId == TfToken("field3dAsset"))
        return new HdNovaField(bprimId, typeId);
    TF_CODING_ERROR("Nova: unknown Bprim type %s", typeId.GetText());
    return nullptr;
}

HdBprim *HdNovaRenderDelegate::CreateFallbackBprim(TfToken const &typeId) {
    return CreateBprim(typeId, SdfPath::EmptyPath());
}

void HdNovaRenderDelegate::DestroyBprim(HdBprim *bprim) { delete bprim; }

void HdNovaRenderDelegate::CommitResources(HdChangeTracker *) {}

HdAovDescriptor HdNovaRenderDelegate::GetDefaultAovDescriptor(TfToken const &name) const {
    if (name == HdAovTokens->color)
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
    // Linear camera depth with 0 for the background, as Karma ("linear" depth
    // style in UsdRenderers.json, so Houdini composites its guides correctly).
    if (name == HdAovTokens->depth || name == HdAovTokens->cameraDepth)
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
    if (name == HdAovTokens->primId || name == HdAovTokens->instanceId)
        return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    if (name == HdAovTokens->normal || name == HdNovaRenderBuffer::AlbedoToken() || name == HdAovTokens->Neye ||
        name == HdAovTokens->Peye)
        return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
    if (name == HdAovTokens->elementId)
        return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    // Any other name is a render var (Karma's vocabulary, M6): its source
    // arrives with the binding's settings. Solaris asks for the render
    // settings' vars by name and drops those without a valid descriptor, so
    // with a RenderSettings prim the viewport would show no beauty at all.
    return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
}

TfToken HdNovaRenderDelegate::GetMaterialBindingPurpose() const { return HdTokens->full; }

// Karma's priority (kma > mtlx > universal), "nova" first; the universal
// network is the fallback Hydra applies itself.
TfTokenVector HdNovaRenderDelegate::GetMaterialRenderContexts() const { return HdNovaMaterialRenderContexts(); }

PXR_NAMESPACE_CLOSE_SCOPE
