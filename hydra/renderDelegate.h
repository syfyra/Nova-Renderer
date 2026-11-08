#pragma once

#include "aov.h"
#include "bake.h"
#include "denoise.h"
#include "renderParam.h"
#include "velocitySceneIndex.h"

#include "core/integrator/renderer.h"
#include "core/scene/scene.h"

#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderThread.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/pxr.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

#define HDNOVA_SETTINGS_TOKENS                     \
    ((samples, "nova:samples"))                   \
    ((maxDepth, "nova:maxDepth"))                 \
    ((clampIndirect, "nova:clampIndirect"))       \
    ((seed, "nova:seed"))                         \
    ((pixelFilter, "nova:pixelFilter"))           \
    ((pixelFilterScale, "nova:pixelFilterScale")) \
    ((lightStrategy, "nova:lightStrategy"))       \
    ((lightSamplingMode, "nova:lightSamplingMode")) \
    (lighttree)                                   \
    (cpu)                                         \
    (xpu)                                         \
    ((device, "nova:device"))                     \
    ((gpuTextureBudget, "nova:gpuTextureBudget")) \
    ((maxMotionSteps, "nova:maxMotionSteps"))     \
    ((iprProxyDepth, "nova:iprProxyDepth"))       \
    ((pixelOracle, "nova:pixelOracle"))           \
    ((oracleMinSamples, "nova:oracleMinSamples")) \
    ((oracleThreshold, "nova:oracleThreshold"))   \
    ((denoise, "nova:denoise"))                   \
    ((denoiseAovs, "nova:denoiseAovs"))           \
    ((viewportDenoise, "nova:viewportDenoise"))   \
    ((denoiseStartPasses, "nova:denoiseStartPasses")) \
    ((karmaImageFilter, "karma:global:imagefilter")) \
    ((houdiniViewport, "houdini:viewport"))       \
    (uniform)                                     \
    (variance)                                    \
    ((subdivLevel, "nova:subdivLevel"))           \
    ((subdivMaxTriangles, "nova:subdivMaxTriangles")) \
    ((dicingQualityScale, "nova:dicingQualityScale")) \
    ((offscreenQuality, "nova:offscreenQuality")) \
    ((textureAutoTx, "nova:textureAutoTx"))       \
    ((textureCacheDir, "nova:textureCacheDir"))   \
    ((textureBlur, "nova:textureBlur"))           \
    ((textureFilterWidth, "nova:textureFilterWidth")) \
    ((abortOnMissingTexture, "nova:abortOnMissingTexture")) \
    ((textureStats, "nova:textureStats"))         \
    ((diffuseLimit, "nova:object:diffuseLimit"))  \
    ((reflectLimit, "nova:object:reflectLimit"))  \
    ((refractLimit, "nova:object:refractLimit"))  \
    ((sssLimit, "nova:object:sssLimit"))          \
    ((volumeLimit, "nova:object:volumeLimit"))    \
    ((thinWallAtRefractLimit, "nova:object:thinWallAtRefractLimit")) \
    ((fakeCaustics, "nova:object:fakeCaustics"))  \
    (mis)                                         \
    (light)                                       \
    (bsdf)                                        \
    (point)                                       \
    (box)                                         \
    (gauss)                                       \
    (gaussian)                                    \
    (blackman)                                    \
    (blackmanharris)                              \
    (triangle)

TF_DECLARE_PUBLIC_TOKENS(HdNovaSettingsTokens, HDNOVA_SETTINGS_TOKENS);

class HdNovaRenderDelegate final : public HdRenderDelegate {
public:
    // The device renders on unless nova:device says otherwise: the CPU for
    // "Nova", the GPU for "Nova XPU" (docs/design/M10.md D1).
    explicit HdNovaRenderDelegate(nova::RenderDevice device = nova::RenderDevice::Cpu);
    HdNovaRenderDelegate(HdRenderSettingsMap const &settingsMap, nova::RenderDevice device = nova::RenderDevice::Cpu);
    ~HdNovaRenderDelegate() override;

    const TfTokenVector &GetSupportedRprimTypes() const override;
    const TfTokenVector &GetSupportedSprimTypes() const override;
    const TfTokenVector &GetSupportedBprimTypes() const override;

    HdRenderParam *GetRenderParam() const override;
    HdResourceRegistrySharedPtr GetResourceRegistry() const override;

    HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;
    // houdini:frame and houdini:fps also become the frame meshes displace
    // at, before the next sync (the render pass would see them only after).
    void SetRenderSetting(TfToken const &key, VtValue const &value) override;
    VtDictionary GetRenderStats() const override;

    bool IsPauseSupported() const override { return true; }
    bool IsPaused() const override { return _paused; }
    bool Pause() override;
    bool Resume() override;
    bool IsStopSupported() const override { return true; }
    // True while no render is running in the background.
    bool IsStopped() const override { return _stoppedByHost || !_renderThread.IsRendering(); }
    bool Stop(bool blocking = true) override;
    bool Restart() override;

    HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex *index,
                                           HdRprimCollection const &collection) override;

    HdInstancer *CreateInstancer(HdSceneDelegate *delegate, SdfPath const &id) override;
    void DestroyInstancer(HdInstancer *instancer) override;

    HdRprim *CreateRprim(TfToken const &typeId, SdfPath const &rprimId) override;
    void DestroyRprim(HdRprim *rPrim) override;

    HdSprim *CreateSprim(TfToken const &typeId, SdfPath const &sprimId) override;
    HdSprim *CreateFallbackSprim(TfToken const &typeId) override;
    void DestroySprim(HdSprim *sprim) override;

    HdBprim *CreateBprim(TfToken const &typeId, SdfPath const &bprimId) override;
    HdBprim *CreateFallbackBprim(TfToken const &typeId) override;
    void DestroyBprim(HdBprim *bprim) override;

    void CommitResources(HdChangeTracker *tracker) override;

    HdAovDescriptor GetDefaultAovDescriptor(TfToken const &name) const override;
    TfToken GetMaterialBindingPurpose() const override;
    TfTokenVector GetMaterialRenderContexts() const override;

    // --- Internal API used by the render pass ------------------------------
    nova::Scene &GetScene() { return _scene; }
    nova::Renderer &GetRenderer() { return _renderer; }
    HdRenderThread &GetRenderThread() { return _renderThread; }
    HdNovaRenderParam &GetNovaRenderParam() { return *_renderParam; }
    // Serializes host Stop()/Restart() against the render pass restarting.
    std::mutex &GetControlMutex() { return _controlMutex; }
    bool IsStoppedByHost() const { return _stoppedByHost; }

    // Called by the render pass (Hydra's sync thread).
    nova::RenderSettings GetNovaSettings() const;
    // Upper bound for motion steps per prim (nova:maxMotionSteps).
    unsigned GetMaxMotionSteps() const;
    // Velocity blur of objects that set nothing themselves (docs/design/M8.md
    // D19): Karma's karma:object:vblur, instance_vblur, geosamples and
    // instance_samples on the render settings; the frame rate from houdini:fps.
    HdNovaVelocityDefaults GetVelocityDefaults() const;
    // nova:subdivLevel (-1: from the display style's refine level, else 0..6)
    // and nova:subdivMaxTriangles (per mesh).
    int GetSubdivLevel() const;
    uint64_t GetSubdivMaxTriangles() const;
    // Displacement dicing (M4.6): nova:dicingQualityScale, nova:offscreenQuality.
    float GetDicingQualityScale() const;
    float GetOffscreenQuality() const;
    // Surface texture settings (nova:texture*, nova:abortOnMissingTexture).
    HdNovaTextureOptions GetTextureOptions() const;
    // The light path expression program of the bindings' LPE AOVs (null:
    // none); the same program while the expressions stay the same.
    std::shared_ptr<const nova::lpe::Compiled> GetLpeProgram(HdRenderPassAovBindingVector const &bindings);
    // The utility planes the bindings ask for (null: none); the same list
    // object while they stay the same.
    std::shared_ptr<const std::vector<nova::UtilityRequest>> GetUtilityRequests(
        HdRenderPassAovBindingVector const &bindings);
    // The cryptomattes the bindings ask for (null: none), likewise.
    std::shared_ptr<const std::vector<nova::CryptoRequest>> GetCryptoRequests(
        HdRenderPassAovBindingVector const &bindings);
    // Texture baking (docs/design/M11-P8.md): whether the settings are a
    // Karma Texture Baker's, and its settings with the meshes resolved (null:
    // no bake); the same object while they stay the same.
    bool IsBaking() const;
    std::shared_ptr<const nova::BakeSettings> GetBakeSettings();
    // Only called while the render thread is stopped, after GetLpeProgram()
    // and GetUtilityRequests() for the same bindings.
    void SetAovBindings(HdRenderPassAovBindingVector const &bindings);
    // Denoising (docs/design/M7.md D2, D3): what the settings ask for (main
    // thread), and the configuration the render thread uses (set while it is
    // stopped).
    HdNovaDenoiseConfig GetDenoiseConfig() const;
    void SetDenoiseConfig(HdNovaDenoiseConfig const &config) { _denoise = config; }
    void OnRenderStarted();
    bool ConsumeNeedsReset() { return _needsReset.exchange(false); }
    // The render stopped because textures are missing and
    // nova:abortOnMissingTexture is on (its buffers report convergence).
    bool IsAborted() const { return _aborted; }

private:
    void _Initialize();
    void _RenderLoop();
    // houdini:frame and houdini:fps (1 and 24 when not set).
    void _FrameAndFps(float &frame, float &fps) const;

    static const TfTokenVector SUPPORTED_RPRIM_TYPES;
    static const TfTokenVector SUPPORTED_SPRIM_TYPES;
    static const TfTokenVector SUPPORTED_BPRIM_TYPES;

    static std::mutex _mutexResourceRegistry;
    static std::atomic_int _counterResourceRegistry;
    static HdResourceRegistrySharedPtr _resourceRegistry;

    nova::RenderDevice _device; // the plugin's (nova:device's default)
    nova::Scene _scene;
    nova::Renderer _renderer;
    mutable HdRenderThread _renderThread; // IsRendering() is non-const
    std::unique_ptr<HdNovaRenderParam> _renderParam;
    HdRenderSettingDescriptorList _settingDescriptors;
    mutable TfToken _unknownPixelFilter; // last warned-about value
    HdRenderPassAovBindingVector _aovBindings;
    std::vector<HdNovaAovSource> _aovSources; // resolved per binding
    std::vector<std::string> _lpeExpressions;  // the program's outputs, in order
    std::shared_ptr<const nova::lpe::Compiled> _lpeProgram;
    std::shared_ptr<const std::vector<nova::UtilityRequest>> _utility;
    std::shared_ptr<const std::vector<nova::CryptoRequest>> _crypto;
    std::shared_ptr<const nova::BakeSettings> _bake;
    std::vector<HdNovaBakeFilter> _bakeFilters; // set with the bindings
    std::mutex _controlMutex;
    std::atomic<bool> _needsReset{false};
    std::atomic<bool> _stoppedByHost{false};
    std::atomic<bool> _paused{false};

    // Render statistics, written by the render thread.
    std::atomic<uint32_t> _completedSamples{0};
    std::atomic<uint32_t> _completedPasses{0};
    HdNovaDenoiseConfig _denoise;
    int64_t _lastDenoiseNs = 0;
    std::atomic<uint32_t> _targetSamples{0};
    std::atomic<bool> _converged{false};
    std::atomic<bool> _abortReported{false}; // missing textures with nova:abortOnMissingTexture
    std::atomic<bool> _aborted{false};       // the current render was aborted for them
    std::atomic<int64_t> _renderStartNs{0};
    std::atomic<int64_t> _renderEndNs{0};
};

PXR_NAMESPACE_CLOSE_SCOPE
