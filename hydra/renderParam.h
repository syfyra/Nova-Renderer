#pragma once

#include "crypto.h"
#include "motion.h"
#include "textureCache.h"
#include "textureSystem.h"
#include "material/registry.h"

#include "core/geometry/dicing.h"
#include "core/scene/scene.h"

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderThread.h>
#include <pxr/pxr.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// Shared state handed to every prim during Sync().
class HdNovaRenderParam final : public HdRenderParam {
public:
    HdNovaRenderParam(nova::Scene *scene, HdRenderThread *thread) : _scene(scene), _thread(thread) {
        _scene->setTextureSampler(&_surfaceTextures);
        _cryptoNames.Note("__materialname", "defaultshader"); // unbound geometry (Karma's name)
    }
    ~HdNovaRenderParam() override { _scene->setTextureSampler(nullptr); }

    // Prims call this before editing the scene. It stops the render thread so
    // the scene can be modified safely; the render pass sees the new edit
    // version and restarts progressive rendering.
    nova::Scene &AcquireSceneForEdit() {
        StopRenderForEdit();
        return *_scene;
    }

    // Same for edits outside the scene, e.g. reallocating a render buffer.
    void StopRenderForEdit() {
        _thread->StopRender();
        ++_editVersion;
    }

    uint64_t GetEditVersion() const { return _editVersion.load(); }
    nova::Scene &GetScene() { return *_scene; }
    HdRenderThread &GetRenderThread() { return *_thread; }
    HdNovaTextureCache &GetTextureCache() { return _textures; }
    HdNovaTextureSystem &GetSurfaceTextures() { return _surfaceTextures; }
    HdNovaMaterialRegistry &GetMaterials() { return _materials; }
    // The names behind the cryptomatte ids (manifests).
    HdNovaCryptoNames &GetCryptoNames() { return _cryptoNames; }

    // Motion sampling state. Written by the delegate and the render camera's
    // Sprim sync, which Hydra runs before the (parallel) Rprim sync that
    // reads it.
    HdNovaShutter GetShutter() const { return _shutter; }
    void SetShutter(HdNovaShutter shutter) { _shutter = shutter; }
    SdfPath const &GetRenderCameraPath() const { return _renderCameraPath; }
    void SetRenderCameraPath(SdfPath const &path) { _renderCameraPath = path; }
    unsigned GetMaxMotionSteps() const { return _maxMotionSteps; }
    void SetMaxMotionSteps(unsigned steps) { _maxMotionSteps = steps; }
    bool DisableMotionBlur() const { return _disableMotionBlur; }
    bool InstantaneousShutter() const { return _instantaneousShutter; }
    void SetMotionFlags(bool disableMotionBlur, bool instantaneousShutter) {
        _disableMotionBlur = disableMotionBlur;
        _instantaneousShutter = instantaneousShutter;
    }

    // Subdivision settings (nova:subdivLevel, -1 = from the display style's
    // refine level; nova:subdivMaxTriangles per mesh). Written by the render
    // pass between syncs, like the shutter.
    int GetSubdivLevel() const { return _subdivLevel; }
    uint64_t GetSubdivMaxTriangles() const { return _subdivMaxTriangles; }
    void SetSubdivSettings(int level, uint64_t maxTriangles) {
        _subdivLevel = level;
        _subdivMaxTriangles = maxTriangles;
    }

    // Displacement dicing (M4.6). The view meshes dice with is the render
    // camera's at the first frame (camera moves do not re-dice, as Karma's
    // default); until it is known, displaced meshes wait undisplaced and the
    // render pass re-syncs them. Meshes sync in parallel: locked.
    nova::DicingView GetDicingView() const {
        std::lock_guard lock(_dicingMutex);
        return _dicingView;
    }
    void SetDicingView(nova::DicingView const &view) {
        std::lock_guard lock(_dicingMutex);
        _dicingView = view;
    }
    float GetDicingQualityScale() const { return _dicingQualityScale; }
    float GetOffscreenQuality() const { return _offscreenQuality; }
    void SetDicingSettings(float qualityScale, float offscreenQuality) {
        _dicingQualityScale = qualityScale;
        _offscreenQuality = offscreenQuality;
    }
    // A displaced mesh (waiting: built without a view; readsTime: its
    // displacement reads the Time or Frame globals, so a new frame re-syncs it).
    void NoteDisplacedMesh(SdfPath const &mesh, bool waiting, bool readsTime = false) {
        std::lock_guard lock(_dicingMutex);
        _displacedMeshes.insert(mesh);
        if (waiting)
            _waitingMeshes.insert(mesh);
        else
            _waitingMeshes.erase(mesh);
        if (readsTime)
            _timeMeshes.insert(mesh);
        else
            _timeMeshes.erase(mesh);
    }
    void ForgetDisplacedMesh(SdfPath const &mesh) {
        std::lock_guard lock(_dicingMutex);
        _displacedMeshes.erase(mesh);
        _waitingMeshes.erase(mesh);
        _timeMeshes.erase(mesh);
    }
    std::vector<SdfPath> TakeWaitingMeshes() {
        std::lock_guard lock(_dicingMutex);
        std::vector<SdfPath> out(_waitingMeshes.begin(), _waitingMeshes.end());
        _waitingMeshes.clear();
        return out;
    }
    std::vector<SdfPath> DisplacedMeshes() const {
        std::lock_guard lock(_dicingMutex);
        return {_displacedMeshes.begin(), _displacedMeshes.end()};
    }
    // The displaced meshes whose displacement reads the time.
    std::vector<SdfPath> TimeDisplacedMeshes() const {
        std::lock_guard lock(_dicingMutex);
        return {_timeMeshes.begin(), _timeMeshes.end()};
    }
    // The frame (and fps) of the Time and Frame globals at tessellation. A
    // change is remembered until the render pass takes it to re-sync the
    // meshes that read the time (their displacement signature includes it).
    float GetFrame() const { return _frame; }
    float GetFps() const { return _fps; }
    void SetFrame(float frame, float fps) {
        if (frame == _frame && fps == _fps)
            return;
        _frame = frame;
        _fps = fps;
        _frameChanged = true;
    }
    bool TakeFrameChanged() { return _frameChanged.exchange(false); }

private:
    nova::Scene *_scene;
    HdRenderThread *_thread;
    std::atomic<uint64_t> _editVersion{0};
    HdNovaTextureCache _textures;
    HdNovaTextureSystem _surfaceTextures;
    HdNovaMaterialRegistry _materials;
    HdNovaCryptoNames _cryptoNames;
    HdNovaShutter _shutter;
    SdfPath _renderCameraPath;
    unsigned _maxMotionSteps = 8;
    bool _disableMotionBlur = false;
    bool _instantaneousShutter = false;
    int _subdivLevel = -1;
    uint64_t _subdivMaxTriangles = 8000000;
    mutable std::mutex _dicingMutex;
    nova::DicingView _dicingView;
    float _dicingQualityScale = 1.0f, _offscreenQuality = 0.1f;
    std::set<SdfPath> _displacedMeshes, _waitingMeshes, _timeMeshes;
    float _frame = 1.0f, _fps = 24.0f;
    std::atomic<bool> _frameChanged{false};
};

// Whether a displacement depends on the frame: its program reads the Time or
// Frame globals; a VEX displacement may read Time, which the bridge does not
// tell, so it is assumed to.
inline bool HdNovaDisplacementReadsTime(HdNovaDisplacement const &displacement) {
    if (displacement.shader.external)
        return true;
    if (!displacement.shader.program)
        return false;
    for (const nova::shading::GlobalBinding &g : displacement.shader.program->globals)
        if (g.global == nova::shading::Global::Time || g.global == nova::shading::Global::Frame)
            return true;
    return false;
}

PXR_NAMESPACE_CLOSE_SCOPE
