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
#include "renderBuffer.h"
#include "renderPass.h"
#include "volume.h"

#include "core/film/lpe_compile.h"

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/js/json.h>
#include <pxr/base/tf/staticTokens.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/tokens.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
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
    // What the host handed to the constructor is authored; the descriptors'
    // defaults filled in below are not (docs/design/M11-P1.md).
    {
        std::lock_guard lock(_authoredMutex);
        for (auto const &[key, value] : _settingsMap)
            if (!value.IsEmpty())
                _authored.insert(key);
    }
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
        {"Internal Reflection", HdNovaSettingsTokens->internalReflection, VtValue(true)},
        {"Caustics Roughness Clamp", HdNovaSettingsTokens->causticsRoughnessClamp, VtValue(0.0f)},
        {"Constrain Roughness", HdNovaSettingsTokens->constrainRoughness, VtValue(false)},
        {"Clamp Indirect", HdNovaSettingsTokens->clampIndirect, VtValue(defaults.clampIndirect)},
        {"Clamp Direct", HdNovaSettingsTokens->clampDirect, VtValue(defaults.clampDirect)},
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
        const uint8_t flags = compiled->outputFlags[i];
        if (!compiled->errors[i].empty())
            HD_NOVA_WARN("AOV %s: %s; it keeps its clear value", names[i].c_str(), compiled->errors[i].c_str());
        else if (flags & nova::lpe::OutputHoldout)
            HD_NOVA_WARN("AOV %s: Nova has no holdouts yet; it renders black", names[i].c_str());
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
}

HdNovaDenoiseConfig HdNovaRenderDelegate::GetDenoiseConfig() const {
    HdNovaDenoiseConfig config;
    const auto text = [&](TfToken const &key) {
        const VtValue v = GetRenderSetting(key);
        return v.IsHolding<std::string>() ? v.UncheckedGet<std::string>()
               : v.IsHolding<TfToken>()   ? v.UncheckedGet<TfToken>().GetString()
                                          : std::string();
    };
    // Nova's switch when authored, else Karma's denoiser (docs/design/M11-P1.md).
    if (IsSettingAuthored(HdNovaSettingsTokens->denoise)) {
        config.final.enabled = HdNovaFlag(GetRenderSetting(HdNovaSettingsTokens->denoise)).value_or(false);
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
    // Karma's viewport toggle stands in for Nova's.
    static const TfToken karmaViewportDenoise("karma:hydra:denoise");
    config.viewportDenoise =
        HdNovaFlag(IsSettingAuthored(HdNovaSettingsTokens->viewportDenoise) ? GetRenderSetting(HdNovaSettingsTokens->viewportDenoise)
                                                                            : GetAuthoredSetting(karmaViewportDenoise))
            .value_or(false);
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

bool HdNovaRenderDelegate::IsSettingAuthored(TfToken const &key) const {
    std::lock_guard lock(_authoredMutex);
    return _authored.count(key) != 0;
}

VtValue HdNovaRenderDelegate::GetAuthoredSetting(TfToken const &key) const {
    return IsSettingAuthored(key) ? GetRenderSetting(key) : VtValue();
}

bool HdNovaRenderDelegate::HasKarmaSettings() const {
    std::lock_guard lock(_authoredMutex);
    for (TfToken const &key : _authored)
        if (TfStringStartsWith(key.GetString(), "karma:global:"))
            return true;
    return false;
}

namespace {

// Karma's names (docs/research/m11/karma-properties.md).
TF_DEFINE_PRIVATE_TOKENS(_karma,
    ((convergenceMode, "karma:global:convergence_mode"))
    ((samplesPerPixel, "karma:global:samplesperpixel"))
    ((pathTracedSamples, "karma:global:pathtracedsamples"))
    ((percentOfSamples, "karma:global:percentofsamples"))
    ((pixelOracle, "karma:global:pixeloracle"))
    ((varianceMin, "karma:object:varianceaa_minsamples"))
    ((varianceMax, "karma:object:varianceaa_maxsamples"))
    ((varianceThreshold, "karma:object:varianceaa_thresh"))
    ((randomSeed, "karma:global:randomseed"))
    ((pixelFilter, "karma:global:pixelfilter"))
    ((pixelFilterScale, "karma:global:pixelfilterscale"))
    ((lightSamplingMode, "karma:global:light_sampling_mode"))
    ((colorLimit, "karma:global:colorlimit"))
    ((colorLimitIndirect, "karma:global:colorlimitindirect"))
    ((colorLimitLinked, "karma:global:colorlimitindlinked"))
    ((constrainMaxRough, "karma:global:constrainmaxrough"))
    ((thinWall, "karma:object:thinwallatrefractlimit"))
    ((causticsEnable, "karma:object:causticsenable"))
    ((causticsClamp, "karma:object:causticsroughnessclamp"))
    ((fakeBsdf, "karma:object:fakecausticsbsdfenable"))
    ((fakeColor, "karma:object:fakecausticscolor"))
    ((fakeOpacity, "karma:object:fakecausticsopacity"))
    ((fakeDarken, "karma:object:fakecausticsfresneldarken"))
    ((internalReflect, "karma:object:internalreflectenable"))
    ((imageWidth, "karma:global:mtlx_imagewidth"))
    ((imageBlur, "karma:global:mtlx_imageblur"))
    ((abortMissing, "karma:global:abortmissingtexture"))
    ((dicingScale, "karma:global:dicingqualityscale"))
    ((offscreen, "karma:global:offscreenquality"))
    ((proxyDepth, "karma:global:ipr_proxydepth"))
    ((enableDof, "karma:global:enable_dof"))
    ((curveStyle, "karma:object:curve_style"))
    ((pointStyle, "karma:object:point_style"))
    ((widthScale, "karma:object:width_scale"))
    ((lightSource, "karma:object:treat_as_lightsource"))
    ((lightDiffuse, "karma:object:lightsource_diffusescale"))
    ((lightSpecular, "karma:object:lightsource_specularscale"))
    ((lightQuality, "karma:object:lightsource_samplingquality"))
    ((dicingQuality, "karma:object:dicingquality"))
    ((dicingDepthMin, "karma:object:dicingdepthmin"))
    ((dicingDepthMax, "karma:object:dicingdepthmax"))
    ((trueDisplace, "karma:object:truedisplace"))
    ((motionBlur, "karma:object:mblur"))
);

// A colour from a setting or primvar value (float3/color3f, double3, one
// number for all three).
std::optional<std::array<float, 3>> color3(VtValue const &v) {
    if (v.IsHolding<GfVec3f>()) {
        const GfVec3f c = v.UncheckedGet<GfVec3f>();
        return std::array<float, 3>{c[0], c[1], c[2]};
    }
    if (v.IsHolding<GfVec3d>()) {
        const GfVec3d c = v.UncheckedGet<GfVec3d>();
        return std::array<float, 3>{float(c[0]), float(c[1]), float(c[2])};
    }
    if (v.IsHolding<VtArray<GfVec3f>>() && v.UncheckedGet<VtArray<GfVec3f>>().size() == 1) {
        const GfVec3f c = v.UncheckedGet<VtArray<GfVec3f>>()[0];
        return std::array<float, 3>{c[0], c[1], c[2]};
    }
    if (const std::optional<double> n = HdNovaNumber(v))
        return std::array<float, 3>{float(*n), float(*n), float(*n)};
    return std::nullopt;
}

// Karma's colour limit as a Nova clamp: Karma clamps to the value itself (0
// renders black), Nova reads 0 as off.
float karmaLimit(double v) { return v > 0.0 ? float(std::min(v, 1e30)) : 1e-30f; }

} // namespace

nova::RenderSettings HdNovaRenderDelegate::GetNovaSettings() const {
    nova::RenderSettings s;
    const bool karmaScene = HasKarmaSettings();
    // Nova's value when authored, else Karma's when authored, else the
    // fallback (Nova's default; for properties without a Nova default in
    // Karma scenes Karma's) - docs/design/M11-P1.md.
    const auto own = [this](TfToken const &nova) { return GetAuthoredSetting(nova); };
    const auto karma = [this](TfToken const &key) { return GetAuthoredSetting(key); };
    const auto number = [&](TfToken const &nova, TfToken const &k, double fallback) {
        if (const auto v = HdNovaNumber(own(nova)))
            return *v;
        if (const auto v = HdNovaNumber(karma(k)))
            return *v;
        return fallback;
    };
    const auto flag = [&](TfToken const &nova, TfToken const &k, bool fallback) {
        if (const auto v = HdNovaFlag(own(nova)))
            return *v;
        if (const auto v = HdNovaFlag(karma(k)))
            return *v;
        return fallback;
    };

    // Samples (docs/design/M11-P1.md 2): Nova's, else Karma's by its
    // convergence mode: Path Traced renders pathtracedsamples paths; in
    // Variance and Distributed modes every primary sample sends up to
    // varianceaa_maxsamples secondary rays, which Nova's one-path-per-sample
    // wavefront takes as primary x secondary samples, with the pixel oracle
    // ending pixels whose noise is low (Variance: always adaptive).
    const int convergence = HdNovaMenuIndex(karma(_karma->convergenceMode), {"Path Traced", "Distributed", "Variance"});
    const bool karmaSampling = !IsSettingAuthored(HdNovaSettingsTokens->samples) &&
                               (karmaScene || convergence >= 0 || IsSettingAuthored(_karma->samplesPerPixel) ||
                                IsSettingAuthored(_karma->pathTracedSamples));
    const int mode = convergence >= 0 ? convergence : 2; // Karma's default: Variance
    const HdNovaKarmaOracle oracle = HdNovaParseKarmaPixelOracle(HdNovaText(karma(_karma->pixelOracle)));
    const double primary = HdNovaNumber(karma(_karma->samplesPerPixel)).value_or(9.0);
    const double secondaryMin = std::max(1.0, HdNovaNumber(karma(_karma->varianceMin)).value_or(1.0));
    const double secondaryMax = std::max(1.0, HdNovaNumber(karma(_karma->varianceMax)).value_or(9.0));
    if (karmaSampling) {
        double samples = mode == 0 ? HdNovaNumber(karma(_karma->pathTracedSamples)).value_or(128.0)
                                   : std::max(1.0, primary) * secondaryMax;
        samples *= std::clamp(HdNovaNumber(karma(_karma->percentOfSamples)).value_or(100.0), 1.0, 100.0) / 100.0;
        s.maxSamples = uint32_t(std::clamp(std::lround(samples), 1l, 1l << 20));
    } else {
        s.maxSamples = uint32_t(std::max(1, GetRenderSetting<int>(HdNovaSettingsTokens->samples, int(s.maxSamples))));
    }
    s.maxDepth = uint32_t(std::max(1, GetRenderSetting<int>(HdNovaSettingsTokens->maxDepth, 64)));
    {
        const TfToken limits[nova::kLimitClasses] = {HdNovaSettingsTokens->diffuseLimit, HdNovaSettingsTokens->reflectLimit,
                                                     HdNovaSettingsTokens->refractLimit, HdNovaSettingsTokens->sssLimit,
                                                     HdNovaSettingsTokens->volumeLimit};
        // Nova's defaults are Karma's (docs/design/M4.md D13). Karma's limits
        // stand in for Nova's unless those are authored (pyro, groom and
        // Karma setups author them on the render settings).
        static const TfToken karmaLimits[nova::kLimitClasses] = {
            TfToken("karma:object:diffuselimit"), TfToken("karma:object:reflectlimit"),
            TfToken("karma:object:refractlimit"), TfToken("karma:object:ssslimit"), TfToken("karma:object:volumelimit")};
        const float defaults[nova::kLimitClasses] = {1.0f, 4.0f, 4.0f, 1.0f, 0.0f};
        for (int k = 0; k < nova::kLimitClasses; ++k) {
            const double v = number(limits[k], karmaLimits[k], defaults[k]);
            s.limits.limit[k] = std::isfinite(v) && v >= 0.0 ? float(v) : defaults[k];
        }
        s.limits.thinWallAtRefractLimit = flag(HdNovaSettingsTokens->thinWallAtRefractLimit, _karma->thinWall, true);
        // Fake caustics: Nova's switch (Nova's physical factor), else
        // Karma's causticsenable inverted (Karma's factor; off by default in
        // Karma scenes, which then fake their caustics).
        if (const auto v = HdNovaFlag(own(HdNovaSettingsTokens->fakeCaustics))) {
            s.limits.fakeCaustics = *v;
        } else if (const auto k = HdNovaFlag(karma(_karma->causticsEnable))) {
            s.limits.fakeCaustics = !*k;
            s.limits.fakeStyle.karma = true;
        } else if (karmaScene) {
            s.limits.fakeCaustics = true;
            s.limits.fakeStyle.karma = true;
        }
        if (const auto v = HdNovaFlag(karma(_karma->fakeBsdf)))
            s.limits.fakeStyle.bsdf = *v;
        if (const auto c = color3(karma(_karma->fakeColor)))
            for (int i = 0; i < 3; ++i)
                s.limits.fakeStyle.color[i] = std::max((*c)[i], 0.0f);
        if (const auto c = color3(karma(_karma->fakeOpacity)))
            for (int i = 0; i < 3; ++i)
                s.limits.fakeStyle.opacity[i] = std::clamp((*c)[i], 0.0f, 1.0f);
        if (const auto v = HdNovaNumber(karma(_karma->fakeDarken)))
            s.limits.fakeStyle.fresnelDarken = float(std::clamp(*v, 0.0, 1.0));
        // Karma's defaults differ from Nova's: no internal reflection, a 0.2
        // roughness clamp on true caustics, constrained roughness.
        s.limits.internalReflection = flag(HdNovaSettingsTokens->internalReflection, _karma->internalReflect, !karmaScene);
        s.limits.causticsRoughnessClamp = float(
            std::clamp(number(HdNovaSettingsTokens->causticsRoughnessClamp, _karma->causticsClamp, karmaScene ? 0.2 : 0.0),
                       0.0, 1.0));
    }
    s.constrainRoughness = flag(HdNovaSettingsTokens->constrainRoughness, _karma->constrainMaxRough, karmaScene);
    // Light selection (docs/design/M9.md D11, D18): the light tree, unless
    // the light sampling mode asks for Uniform ("uniform" or 0): Nova's when
    // authored, else Karma's light_sampling_mode.
    {
        VtValue mode = own(HdNovaSettingsTokens->lightSamplingMode);
        if (mode.IsEmpty())
            mode = karma(_karma->lightSamplingMode);
        const std::string text = HdNovaText(mode);
        if (!text.empty())
            s.lightTree = TfStringToLower(text).find("uniform") == std::string::npos;
        else
            s.lightTree = HdNovaNumber(mode).value_or(1.0) != 0.0;
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
    // Colour limits (docs/design/M11-P1.md 3): Nova's clamps when either is
    // authored; else Karma's colorlimit for what the camera sees and the
    // first hit gathers, colorlimitindirect (colorlimit when linked) for the
    // rest, emission found by the first bounce counting as indirect
    // (measured). Karma's registry defaults (20) in Karma scenes.
    if (IsSettingAuthored(HdNovaSettingsTokens->clampIndirect) || IsSettingAuthored(HdNovaSettingsTokens->clampDirect) ||
        !(karmaScene || IsSettingAuthored(_karma->colorLimit) || IsSettingAuthored(_karma->colorLimitIndirect))) {
        s.clampIndirect = std::max(0.0f, GetRenderSetting<float>(HdNovaSettingsTokens->clampIndirect, s.clampIndirect));
        s.clampDirect = std::max(0.0f, GetRenderSetting<float>(HdNovaSettingsTokens->clampDirect, s.clampDirect));
    } else {
        const double direct = HdNovaNumber(karma(_karma->colorLimit)).value_or(20.0);
        const bool linked = HdNovaFlag(karma(_karma->colorLimitLinked)).value_or(true);
        const double indirect = linked ? direct : HdNovaNumber(karma(_karma->colorLimitIndirect)).value_or(20.0);
        s.clampDirect = karmaLimit(direct);
        s.clampIndirect = karmaLimit(indirect);
        s.clampEmissionIndirect = true;
    }
    s.seed = uint32_t(std::lround(number(HdNovaSettingsTokens->seed, _karma->randomSeed, double(s.seed))));

    // USD render settings deliver the filter name as a token or a string.
    // The names and sizes follow Karma's karma:global:pixelfilter, which
    // stands in for Nova's when that is not authored.
    VtValue filter = own(HdNovaSettingsTokens->pixelFilter);
    if (filter.IsEmpty()) {
        const std::string karmaFilter = HdNovaKarmaFilterName(HdNovaText(karma(_karma->pixelFilter)));
        if (!karmaFilter.empty())
            filter = VtValue(TfToken(karmaFilter));
    }
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
    const float scale =
        float(number(HdNovaSettingsTokens->pixelFilterScale, _karma->pixelFilterScale, s.pixelFilterScale));
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
    // Nova's settings when authored, else with Karma's sample counts Karma's
    // pixel oracle (Variance mode always adapts: its secondary samples
    // stop at varianceaa_thresh), docs/design/M11-P1.md 2.
    const std::string oracleName = HdNovaText(GetRenderSetting(HdNovaSettingsTokens->pixelOracle));
    s.adaptive = oracleName == HdNovaSettingsTokens->variance.GetString();
    int minSamples = GetRenderSetting<int>(HdNovaSettingsTokens->oracleMinSamples, -1);
    s.oracleThreshold = std::max(0.0f, GetRenderSetting<float>(HdNovaSettingsTokens->oracleThreshold, s.oracleThreshold));
    if (karmaSampling && !IsSettingAuthored(HdNovaSettingsTokens->pixelOracle)) {
        const bool variance = mode == 2 || (oracle.valid ? oracle.variance : true);
        s.adaptive = variance;
        if (!IsSettingAuthored(HdNovaSettingsTokens->oracleThreshold)) {
            const double secondary = HdNovaNumber(karma(_karma->varianceThreshold)).value_or(0.01);
            s.oracleThreshold =
                float(std::max(0.0, (oracle.valid && oracle.variance) || mode != 2 ? double(oracle.threshold) : secondary));
        }
        if (!IsSettingAuthored(HdNovaSettingsTokens->oracleMinSamples)) {
            if (mode == 2 && !(oracle.valid && oracle.variance))
                minSamples = int(std::lround(std::max(1.0, primary) * secondaryMin)); // uniform primaries
            else if (oracle.minRays > 0)
                minSamples = int(std::lround(oracle.minRays * (mode == 0 ? 1.0 : secondaryMin)));
            else
                minSamples = -1;
        }
    }
    s.oracleMinSamples = minSamples > 0 ? uint32_t(minSamples) : 0u;
    _WarnUnsupportedKarmaSettings();
    return s;
}

HdNovaObjectDefaults HdNovaRenderDelegate::GetObjectDefaults() const {
    HdNovaObjectDefaults d;
    const auto karma = [this](TfToken const &key) { return GetAuthoredSetting(key); };
    const auto number = [&](TfToken const &key, float fallback) {
        const auto v = HdNovaNumber(karma(key));
        return v ? float(*v) : fallback;
    };
    d.curveStyle =
        HdNovaMenuIndex(karma(_karma->curveStyle), {"Flat Ribbons", "Rounded Curves", "Oriented Ribbons", "USD Default Behaviour"});
    d.pointStyle = HdNovaMenuIndex(karma(_karma->pointStyle), {"Discs", "Spheres", "Oriented Discs"});
    d.widthScale = std::max(number(_karma->widthScale, -1.0f), -1.0f);
    d.lightSource = HdNovaMenuIndex(karma(_karma->lightSource), {"No", "Yes", "Auto"});
    d.lightDiffuse = number(_karma->lightDiffuse, -1.0f);
    d.lightSpecular = number(_karma->lightSpecular, -1.0f);
    d.lightQuality = number(_karma->lightQuality, -1.0f);
    const float quality = number(_karma->dicingQuality, -1.0f);
    d.dicingQuality = quality > 0.0f ? quality : -1.0f;
    // A fixed dicing depth when the minimum and maximum agree (Nova's
    // nova:dicingDepth); different ones warn (_WarnUnsupportedKarmaSettings).
    const auto dmin = HdNovaNumber(karma(_karma->dicingDepthMin)), dmax = HdNovaNumber(karma(_karma->dicingDepthMax));
    if (dmin && dmax && std::lround(*dmin) == std::lround(*dmax))
        d.dicingDepth = int(std::clamp(std::lround(*dmin), -1l, 10l));
    const int trueDisplace = HdNovaMenuIndex(karma(_karma->trueDisplace),
                                             {"Displacement as Bump", "True Displacement", "Disable Displacement Shader",
                                              "Bump Added to Displacement"});
    d.displacement = trueDisplace < 0 ? -1 : trueDisplace == 2 ? 0 : 1;
    if (const auto v = HdNovaFlag(karma(_karma->motionBlur)))
        d.motionBlur = *v ? 1 : 0;
    return d;
}

bool HdNovaRenderDelegate::IsDepthOfFieldEnabled() const {
    static const TfToken disable("disableDepthOfField");
    return !HdNovaFlag(GetRenderSetting(disable)).value_or(false) &&
           HdNovaFlag(GetAuthoredSetting(_karma->enableDof)).value_or(true);
}

void HdNovaRenderDelegate::_WarnUnsupportedKarmaSettings() const {
    // Karma Render Settings properties that change the image and that Nova
    // does not honour yet: each warns once per delegate (one stage render)
    // when its value differs from the one Nova's image matches.
    // Performance-only and noise-only properties stay silent; holdouts and
    // ray visibility belong to another package (docs/design/M11.md P2).
    struct Check {
        const char *key;
        const char *what;
        bool (*differs)(VtValue const &);
    };
    static const auto on = [](VtValue const &v) { return HdNovaFlag(v).value_or(false); };
    static const auto off = [](VtValue const &v) { return !HdNovaFlag(v).value_or(true); };
    static const auto text = [](VtValue const &v) { return !TfStringTrim(HdNovaText(v)).empty(); };
    static const Check checks[] = {
        {"karma:global:outline_enable", "outlines", on},
        {"karma:global:guiding_enable", "path guiding", on},
        {"karma:global:disable_lighting", "the headlight look-dev mode (disable lighting)", on},
        {"karma:global:override_lighting", "the lighting override",
         [](VtValue const &v) {
             return HdNovaMenuIndex(v, {"Off", "Emissive Objects", "Headlight", "Dome Light"}) > 0;
         }},
        {"karma:global:image_blur", "image blur off (camera rays at shutter open)", off},
        {"karma:global:samplefilter", "sample filters", text},
        {"karma:global:globalaovmaterial", "the global AOV material", text},
        {"karma:global:dicingcamera", "the dicing camera", text},
        {"karma:global:overrideobject", "object properties overriding the prims'", text},
        {"karma:global:raybias", "the ray bias",
         [](VtValue const &v) { return std::fabs(HdNovaNumber(v).value_or(0.001) - 0.001) > 1e-6; }},
        {"karma:global:diffusecomponents", "custom diffuse component labels",
         [](VtValue const &v) { return TfStringTrim(HdNovaText(v)) != "diffuse"; }},
        {"karma:global:refractcomponents", "custom refraction component labels",
         [](VtValue const &v) { return TfStringTrim(HdNovaText(v)) != "refract"; }},
        {"karma:global:ssscomponents", "custom SSS component labels",
         [](VtValue const &v) { return TfStringTrim(HdNovaText(v)) != "sss"; }},
        {"karma:global:volumecomponents", "custom volume component labels",
         [](VtValue const &v) { return TfStringTrim(HdNovaText(v)) != "volume"; }},
        {"karma:object:blurstyle", "linear motion blur",
         [](VtValue const &v) { return HdNovaMenuIndex(v, {"Linear Blur", "Rotation Blur"}) == 0; }},
        {"karma:object:samplesfromstage", "motion samples not taken from the stage (xformsamples, geosamples)", off},
        {"karma:object:cull_backface", "backface culling",
         [](VtValue const &v) { return HdNovaMenuIndex(v, {"Off", "Cull Backface", "Cull Frontface"}) > 0; }},
        {"karma:object:curve_basis", "a forced curve basis",
         [](VtValue const &v) {
             return HdNovaMenuIndex(v, {"Geometry Default", "Force Linear", "Force Bezier", "Force B-Spline",
                                        "Force Catmull-Rom"}) > 0;
         }},
        {"karma:object:truedisplace", "displacement as bump",
         [](VtValue const &v) { return TfStringToLower(HdNovaText(v)).find("bump") != std::string::npos ||
                                       HdNovaNumber(v).value_or(1.0) == 0.0 || HdNovaNumber(v).value_or(1.0) == 3.0; }},
        {"karma:object:dicingpredisplace", "dicing with a trial displacement", on},
        {"karma:object:dicingzimportance", "the dicing z importance",
         [](VtValue const &v) { return std::fabs(HdNovaNumber(v).value_or(1.0) - 1.0) > 1e-6; }},
        {"karma:object:limitcolor", "the ray limit colour",
         [](VtValue const &v) {
             const auto c = color3(v);
             return c && ((*c)[0] != 0.0f || (*c)[1] != 0.0f || (*c)[2] != 0.0f);
         }},
        {"karma:object:dielectricpriority", "nested dielectrics",
         [](VtValue const &v) { return HdNovaNumber(v).value_or(0.0) != 0.0; }},
        {"karma:object:isportal", "light portals", on},
        {"karma:object:bumpshadowterminator", "the bump shadow terminator",
         [](VtValue const &v) { return HdNovaNumber(v).value_or(0.0) != 0.0; }},
        {"karma:object:smoothp", "smoothed shadow positions off", off},
        {"karma:object:cuspangle", "the cusp angle of generated normals",
         [](VtValue const &v) { return std::fabs(HdNovaNumber(v).value_or(60.0) - 60.0) > 1e-6; }},
        {"karma:object:stencilmap", "stencil maps", text},
        // Not volumesteprate: Nova's tracking is unbiased by design (M8 D16);
        // finer rates converge to its image, the coarse default differs.
    };
    std::vector<std::string> messages;
    for (const Check &c : checks) {
        const VtValue v = GetAuthoredSetting(TfToken(c.key));
        if (v.IsEmpty() || !c.differs(v))
            continue;
        std::lock_guard lock(_warnMutex);
        if (_warnedKarma.insert(c.key).second)
            messages.push_back(TfStringPrintf("%s (%s)", c.what, c.key));
    }
    // Karma's image filters other than its denoiser (which Nova runs).
    {
        const std::string filters = HdNovaText(GetAuthoredSetting(HdNovaSettingsTokens->karmaImageFilter));
        JsParseError error;
        const JsValue v = filters.empty() ? JsValue() : JsParseString(filters, &error);
        if (v.IsArray())
            for (const JsValue &entry : v.GetJsArray()) {
                const std::string name = entry.IsArray() && !entry.GetJsArray().empty() && entry.GetJsArray()[0].IsString()
                                             ? entry.GetJsArray()[0].GetString()
                                             : entry.IsString() ? entry.GetString() : std::string();
                if (name.empty() || name == "denoise")
                    continue;
                std::lock_guard lock(_warnMutex);
                if (_warnedKarma.insert("imagefilter:" + name).second)
                    messages.push_back(TfStringPrintf("the image filter '%s' (karma:global:imagefilter)", name.c_str()));
            }
    }
    // Different minimum and maximum dicing depths (Nova dices at one depth).
    {
        const auto dmin = HdNovaNumber(GetAuthoredSetting(_karma->dicingDepthMin));
        const auto dmax = HdNovaNumber(GetAuthoredSetting(_karma->dicingDepthMax));
        if ((dmin || dmax) && !(dmin && dmax && std::lround(*dmin) == std::lround(*dmax))) {
            std::lock_guard lock(_warnMutex);
            if (_warnedKarma.insert("dicingdepth").second)
                messages.push_back("dicing depths with a range (karma:object:dicingdepthmin/max)");
        }
    }
    for (std::string const &m : messages)
        HD_NOVA_WARN("Karma render setting not supported, the image differs from Karma's: %s", m.c_str());
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
    TF_DEBUG(HDNOVA_SETTINGS).Msg("[Nova] setting %s = %s\n", key.GetText(), TfStringify(value).c_str());
    {
        std::lock_guard lock(_authoredMutex);
        if (value.IsEmpty())
            _authored.erase(key);
        else
            _authored.insert(key);
    }
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

// Nova's number when authored, else Karma's (times karmaScale), else the
// fallback (docs/design/M11-P1.md).
double HdNovaRenderDelegate::_NumberSetting(TfToken const &nova, TfToken const &karma, double fallback,
                                           double karmaScale) const {
    if (const auto v = HdNovaNumber(GetAuthoredSetting(nova)))
        return *v;
    if (const auto v = HdNovaNumber(GetAuthoredSetting(karma)))
        return *v * karmaScale;
    return fallback;
}

float HdNovaRenderDelegate::GetDicingQualityScale() const {
    const float q = float(_NumberSetting(HdNovaSettingsTokens->dicingQualityScale, _karma->dicingScale, 1.0));
    return std::isfinite(q) ? std::clamp(q, 1e-3f, 64.0f) : 1.0f;
}

float HdNovaRenderDelegate::GetOffscreenQuality() const {
    const float q = float(_NumberSetting(HdNovaSettingsTokens->offscreenQuality, _karma->offscreen, 0.1));
    return std::isfinite(q) ? std::clamp(q, 1e-3f, 1.0f) : 0.1f;
}

int HdNovaRenderDelegate::GetIprProxyDepth() const {
    return std::clamp(int(std::lround(_NumberSetting(HdNovaSettingsTokens->iprProxyDepth, _karma->proxyDepth, 2.0))), 0, 3);
}

HdNovaTextureOptions HdNovaRenderDelegate::GetTextureOptions() const {
    HdNovaTextureOptions o;
    o.autoTx = VtValue::Cast<bool>(GetRenderSetting(HdNovaSettingsTokens->textureAutoTx)).GetWithDefault(true);
    const VtValue dir = GetRenderSetting(HdNovaSettingsTokens->textureCacheDir);
    o.cacheDir = dir.IsHolding<std::string>() ? dir.UncheckedGet<std::string>()
                 : dir.IsHolding<TfToken>()   ? dir.UncheckedGet<TfToken>().GetString()
                                              : std::string();
    // Karma's mtlx_imageblur and mtlx_imagewidth stand in (Nova's width 0.25
    // is Karma's 1, docs/design/M3.md M3.9).
    const float blur = float(_NumberSetting(HdNovaSettingsTokens->textureBlur, _karma->imageBlur, 0.0));
    o.blur = std::isfinite(blur) ? std::clamp(blur, 0.0f, 1.0f) : 0.0f;
    const float width = float(_NumberSetting(HdNovaSettingsTokens->textureFilterWidth, _karma->imageWidth, 0.25, 0.25));
    o.filterWidth = std::isfinite(width) ? std::clamp(width, 0.0f, 16.0f) : 0.25f;
    o.abortOnMissing = HdNovaFlag(IsSettingAuthored(HdNovaSettingsTokens->abortOnMissingTexture)
                                      ? GetRenderSetting(HdNovaSettingsTokens->abortOnMissingTexture)
                                      : GetAuthoredSetting(_karma->abortMissing))
                           .value_or(false);
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
