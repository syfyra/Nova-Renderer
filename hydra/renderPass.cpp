#include "renderPass.h"

#include "camera.h"
#include "diagnostics.h"
#include "lightFilter.h"
#include "renderBuffer.h"
#include "renderDelegate.h"
#include "resyncSceneIndex.h"

#include "core/integrator/volume_media.h"
#include "core/integrator/wavefront.h"

#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/legacyDisplayStyleSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/xformSchema.h>

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (resolution)
    (pixelAspectRatio)
    (aspectRatioConformPolicy)
    (expandAperture)
    (cropAperture)
    (adjustApertureWidth)
    (adjustApertureHeight)
    (adjustPixelAspectRatio)
    ((viewport, "houdini:viewport"))
    (disableDepthOfField)
    (disableMotionBlur)
    (instantaneousShutter)
);

namespace {

nova::Mat4 toNova(GfMatrix4d const &m) {
    nova::Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = m[i][j];
    return r;
}

// UsdRenderSettings aspectRatioConformPolicy -> camera window policy.
CameraUtilConformWindowPolicy conformPolicy(VtValue const &value, CameraUtilConformWindowPolicy fallback) {
    TfToken policy;
    if (value.IsHolding<TfToken>())
        policy = value.UncheckedGet<TfToken>();
    else if (value.IsHolding<std::string>())
        policy = TfToken(value.UncheckedGet<std::string>());
    if (policy == _tokens->expandAperture)
        return CameraUtilFit;
    if (policy == _tokens->cropAperture)
        return CameraUtilCrop;
    if (policy == _tokens->adjustApertureWidth)
        return CameraUtilMatchVertically;
    if (policy == _tokens->adjustApertureHeight)
        return CameraUtilMatchHorizontally;
    if (policy == _tokens->adjustPixelAspectRatio)
        return CameraUtilDontConform;
    return fallback;
}

// Conforms the camera window to `aspect` as Karma does for final renders: a
// window offset (film back shift) scales with the window, as if it were a
// fraction of the aperture. CameraUtilConformedWindow keeps the offset in
// aperture units instead; in the projection matrix Karma's rule keeps the
// offset terms (perspective [2][0..1], orthographic [3][0..1]) unchanged.
// The dicing view of a camera view (M4.6): pixels per unit from the
// vertical projection scale and the image height.
nova::DicingView dicingViewOf(GfMatrix4d const &worldToView, GfMatrix4d const &projection, float imageHeight) {
    nova::DicingView d;
    d.valid = true;
    const GfMatrix4d viewToWorld = worldToView.GetInverse();
    d.eye = {float(viewToWorld[3][0]), float(viewToWorld[3][1]), float(viewToWorld[3][2])};
    d.forward = nova::normalize(nova::Vec3f{-float(viewToWorld[2][0]), -float(viewToWorld[2][1]), -float(viewToWorld[2][2])});
    d.perspective = std::fabs(projection[2][3]) > 1e-9;
    d.pixelsPerUnit = float(std::fabs(projection[1][1])) * imageHeight * 0.5f;
    // An edge nearer than the near clipping plane is measured at that plane
    // (it is not seen): the camera's own distance, not a number of units, so
    // that a scene modelled ten thousand times smaller is diced the same.
    if (d.perspective) {
        const double nearClip = projection[3][2] / (projection[2][2] - 1.0);
        if (std::isfinite(nearClip) && nearClip > 0.0)
            d.nearDistance = float(nearClip);
    }
    const GfMatrix4d clip = worldToView * projection;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            d.worldToClip.m[i][j] = clip[i][j];
    return d;
}

GfMatrix4d karmaConformedWindow(GfMatrix4d const &projection, CameraUtilConformWindowPolicy policy,
                                double aspect) {
    GfMatrix4d conformed = CameraUtilConformedWindow(projection, policy, aspect);
    for (int row : {2, 3})
        for (int col : {0, 1})
            conformed[row][col] = projection[row][col];
    return conformed;
}

// A pixel aspect ratio a camera window can be conformed to; anything else
// (zero, negative, NaN) counts as square pixels.
double validPixelAspect(double aspect) { return std::isfinite(aspect) && aspect > 0.0 ? aspect : 1.0; }

// Finite as the floats the core traces with.
bool isFinite(GfMatrix4d const &m) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!std::isfinite(float(m[i][j])))
                return false;
    return true;
}

// A matrix rays can be unprojected through.
bool isInvertible(GfMatrix4d const &m) {
    if (!isFinite(m))
        return false;
    double det = 0.0;
    const GfMatrix4d inverse = m.GetInverse(&det);
    return det != 0.0 && std::isfinite(det) && isFinite(inverse);
}

// VtValue equality in which a NaN equals itself: values that hold one (a
// clear value, an AOV setting) are unequal to themselves and are compared
// by their hash, which is taken from their bits.
bool sameValue(VtValue const &a, VtValue const &b) {
    if (a == b)
        return true;
    return !(a == a) && !(b == b) && a.GetTypeid() == b.GetTypeid() && a.GetHash() == b.GetHash();
}

// HdRenderPassAovBinding's equality with sameValue(): bindings that hold a
// NaN would otherwise differ from themselves and restart the render on
// every Execute.
bool sameBindings(HdRenderPassAovBindingVector const &a, HdRenderPassAovBindingVector const &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].aovName != b[i].aovName || a[i].renderBuffer != b[i].renderBuffer ||
            a[i].renderBufferId != b[i].renderBufferId || !sameValue(a[i].clearValue, b[i].clearValue) ||
            a[i].aovSettings.size() != b[i].aovSettings.size())
            return false;
        for (auto const &[key, value] : a[i].aovSettings) {
            const auto other = b[i].aovSettings.find(key);
            if (other == b[i].aovSettings.end() || !sameValue(value, other->second))
                return false;
        }
    }
    return true;
}

} // namespace

HdNovaRenderPass::HdNovaRenderPass(HdRenderIndex *index, HdRprimCollection const &collection,
                                   HdNovaRenderDelegate *delegate)
    : HdRenderPass(index, collection), _delegate(delegate) {
    // The render settings' velocity blur before the first sync, so the
    // prims resolve their motion once (docs/design/M8.md D19); likewise the
    // other per-object defaults (docs/design/M11-P1.md).
    if (HdNovaVelocityModeSceneIndexRefPtr velocity = HdNovaVelocityModeSceneIndex::Find(index))
        velocity->SetDefaults(_delegate->GetVelocityDefaults());
    // True when prims already used other defaults (this pass replaces
    // another in a session): they are re-synced at the first execute, which
    // SetObjectDefaults() alone would no longer tell.
    _defaultsChanged = _delegate->GetNovaRenderParam().SetObjectDefaults(_delegate->GetObjectDefaults());
}

HdNovaRenderPass::~HdNovaRenderPass() {
    // The render thread must no longer write into the buffers bound here.
    _delegate->GetRenderThread().StopRender();
}

bool HdNovaRenderPass::IsConverged() const {
    // A render stopped by the host will not change any more.
    if (_delegate->IsStoppedByHost() || _missingCamera || _invalidView)
        return true;
    if (_bindings.empty())
        return false;
    for (const HdRenderPassAovBinding &b : _bindings)
        if (b.renderBuffer && !b.renderBuffer->IsConverged())
            return false;
    return true;
}

HdNovaRenderPass::View HdNovaRenderPass::_ComputeView(HdRenderPassStateSharedPtr const &state, int width,
                                                      int height) const {
    View view;
    view.worldToView = state->GetWorldToViewMatrix();
    const GfRect2i bufferRect(GfVec2i(0), width, height);

    // Thin lens from the camera (HdCamera reports lengths in world units).
    // focusOn is not set by USD, so it is not consulted (as in hdPrman).
    if (const HdCamera *cam = state->GetCamera()) {
        // USD's disableDepthOfField and Karma's karma:global:enable_dof.
        const bool dof = _delegate->IsDepthOfFieldEnabled() && cam->GetFStop() > 0.0f &&
                         cam->GetFocusDistance() > 0.0f && cam->GetFocalLength() > 0.0f;
        view.lensRadius = dof ? cam->GetFocalLength() / (2.0f * cam->GetFStop()) : 0.0f;
        view.focusDistance = dof ? cam->GetFocusDistance() : 0.0f;
        // A lens that is not finite (an infinite focus distance or focal
        // length) has no focus plane to aim the rays at: a pinhole.
        if (!std::isfinite(view.lensRadius) || !std::isfinite(view.focusDistance))
            view.lensRadius = view.focusDistance = 0.0f;
    }

    const CameraUtilFraming &framing = state->GetFraming();
    if (framing.IsValid()) {
        // Camera framing (film pixel = image pixel). The rays are those of the
        // whole display window, conformed with the window policy, and the data
        // window only selects the pixels: a crop then traces exactly the rays
        // of the full frame (state->GetProjectionMatrix() maps NDC to the data
        // window instead, which rounds differently). Framing windows are
        // y-down, the film is bottom-up.
        const GfRect2i dw = framing.dataWindow.GetIntersection(bufferRect);
        if (!dw.IsEmpty())
            view.region = {dw.GetMinX(), height - 1 - dw.GetMaxY(), dw.GetMaxX() + 1, height - dw.GetMinY()};
        const GfRange2f &display = framing.displayWindow;
        const GfVec2f size = display.GetSize();
        const HdCamera *cam = state->GetCamera();
        if (cam && size[0] > 0.0f && size[1] > 0.0f) {
            const double aspect =
                validPixelAspect(double(framing.pixelAspectRatio)) * double(size[0]) / double(size[1]);
            // The viewport conforms like Houdini's own drawing (CameraUtil),
            // final renders (nova_cli) like Karma.
            const bool viewport = _delegate->GetRenderSetting<bool>(_tokens->viewport, false);
            view.projection = viewport ? CameraUtilConformedWindow(cam->ComputeProjectionMatrix(),
                                                                   state->GetWindowPolicy(), aspect)
                                       : karmaConformedWindow(cam->ComputeProjectionMatrix(),
                                                              state->GetWindowPolicy(), aspect);
            // Raster coordinates (and sampler seeds) are image pixels relative
            // to the display window, so overscan or a shifted buffer renders
            // the same samples as the plain full frame.
            const float minX = display.GetMin()[0], minY = float(height) - display.GetMax()[1]; // film, y-up
            view.imageOriginX = -int(std::floor(minX));
            view.imageOriginY = -int(std::floor(minY));
            view.ndcScale = GfVec2f(2.0f / size[0], 2.0f / size[1]);
            view.ndcOffset = GfVec2f(-1.0f - view.ndcScale[0] * (minX - std::floor(minX)),
                                     -1.0f - view.ndcScale[1] * (minY - std::floor(minY)));
        } else if (!dw.IsEmpty()) { // no display window: NDC spans the data window
            view.projection = state->GetProjectionMatrix();
            view.ndcScale = GfVec2f(2.0f / float(view.region.width()), 2.0f / float(view.region.height()));
            view.ndcOffset = GfVec2f(-1.0f - view.ndcScale[0] * float(view.region.x0),
                                     -1.0f - view.ndcScale[1] * float(view.region.y0));
        }
        return view;
    }

    // Viewport API, as used by husk: the buffers hold only the data window and
    // the viewport origin places it in the full image (y-up). The full image
    // size is the "resolution" render setting, and the camera is conformed to
    // the full image, not to the data window (crop windows, overscan).
    const GfVec4f vp = state->GetViewport();
    GfVec2i resolution = _delegate->GetRenderSetting<GfVec2i>(_tokens->resolution, GfVec2i(0));
    const bool interactive = _delegate->GetRenderSetting<bool>(_tokens->viewport, false);
    if (interactive || resolution[0] <= 0 || resolution[1] <= 0 || !state->GetCamera()) {
        resolution = GfVec2i(width, height); // the buffer is the whole image
        view.projection = state->GetProjectionMatrix();
    } else {
        const double pixelAspect =
            validPixelAspect(_delegate->GetRenderSetting<float>(_tokens->pixelAspectRatio, 1.0f));
        const double aspect = pixelAspect * double(resolution[0]) / double(resolution[1]);
        // husk passes the conform policy as a render setting, not as an override.
        const CameraUtilConformWindowPolicy policy =
            state->GetOverrideWindowPolicy()
                ? *state->GetOverrideWindowPolicy()
                : conformPolicy(_delegate->GetRenderSetting(_tokens->aspectRatioConformPolicy), state->GetWindowPolicy());
        view.projection = karmaConformedWindow(state->GetCamera()->ComputeProjectionMatrix(), policy, aspect);
        view.imageOriginX = int(vp[0]);
        view.imageOriginY = int(vp[1]);
    }
    // Raster coordinates are image pixels (film pixel + image origin).
    view.region = {0, 0, width, height};
    view.ndcScale = GfVec2f(2.0f / float(resolution[0]), 2.0f / float(resolution[1]));
    view.ndcOffset = GfVec2f(-1.0f, -1.0f);
    return view;
}

void HdNovaRenderPass::_Execute(HdRenderPassStateSharedPtr const &state, TfTokenVector const &) {
    std::lock_guard<std::mutex> control(_delegate->GetControlMutex());

    HdRenderThread &thread = _delegate->GetRenderThread();
    HdNovaRenderParam &param = _delegate->GetNovaRenderParam();
    nova::Renderer &renderer = _delegate->GetRenderer();
    nova::Scene &scene = _delegate->GetScene();

    // A binding may name its buffer by path only.
    HdRenderPassAovBindingVector bindings = state->GetAovBindings();
    for (HdRenderPassAovBinding &b : bindings)
        if (!b.renderBuffer && !b.renderBufferId.IsEmpty())
            b.renderBuffer = static_cast<HdRenderBuffer *>(
                GetRenderIndex()->GetBprim(HdPrimTypeTokens->renderBuffer, b.renderBufferId));

    if (_delegate->IsStoppedByHost()) {
        // Not rendering, but keep only live buffers: the old ones may have
        // been destroyed while stopped. The render thread is idle, and
        // Restart() forces a full restart that rebinds them.
        _bindings = bindings;
        return;
    }

    // All AOVs share the size of the first buffer.
    HdRenderBuffer *first = bindings.empty() ? nullptr : bindings.front().renderBuffer;
    const int width = first ? int(first->GetWidth()) : 0;
    const int height = first ? int(first->GetHeight()) : 0;
    if (!state->GetCamera() || width <= 0 || height <= 0) {
        // Nothing to render into; drop the old bindings, they may be gone.
        if (!_bindings.empty()) {
            thread.StopRender();
            _bindings.clear();
            _delegate->SetAovBindings({});
        }
        // A batch render with AOVs but no camera never gets one.
        const bool interactive = _delegate->GetRenderSetting<bool>(_tokens->viewport, false);
        if (!interactive && !state->GetCamera() && width > 0 && !_missingCamera) {
            HD_NOVA_WARN("the render camera is not in the stage; nothing to render");
            _missingCamera = true;
        }
        return;
    }
    _missingCamera = false;

    // Motion is sampled with the shutter of the render camera during sync. If
    // the effective shutter changed (another camera, an edited shutter, motion
    // blur toggled), resample first: dirty the moving prims and render after
    // the next sync.
    const HdCamera *hdCamera = state->GetCamera();
    const bool disableMotionBlur = _delegate->GetRenderSetting<bool>(_tokens->disableMotionBlur, false);
    const bool instantaneous = _delegate->GetRenderSetting<bool>(_tokens->instantaneousShutter, false);
    const HdNovaShutter shutter = HdNovaResolveShutter(hdCamera->GetShutterOpen(), hdCamera->GetShutterClose(),
                                                       disableMotionBlur, instantaneous);
    const unsigned maxMotionSteps = _delegate->GetMaxMotionSteps();
    param.SetMotionFlags(disableMotionBlur, instantaneous);
    const bool shutterChanged = !(shutter == param.GetShutter()) || maxMotionSteps != param.GetMaxMotionSteps();
    // Subdivision settings: meshes re-resolve their level and tessellation
    // rate (a mesh only rebuilds if the rate changes).
    const int subdivLevel = _delegate->GetSubdivLevel();
    const uint64_t subdivMaxTriangles = _delegate->GetSubdivMaxTriangles();
    const bool subdivChanged =
        subdivLevel != param.GetSubdivLevel() || subdivMaxTriangles != param.GetSubdivMaxTriangles();
    if (shutterChanged || subdivChanged) {
        if (shutterChanged)
            TF_DEBUG(HDNOVA_MOTION).Msg("Nova: camera %s shutter [%g, %g] (disableMotionBlur %d) -> [%g, %g]\n",
                                        hdCamera->GetId().GetText(), hdCamera->GetShutterOpen(),
                                        hdCamera->GetShutterClose(), int(disableMotionBlur), shutter.open,
                                        shutter.close);
        // Both at once, so a mesh is rebuilt once with all new settings.
        thread.StopRender();
        param.SetShutter(shutter);
        param.SetMaxMotionSteps(maxMotionSteps);
        param.SetSubdivSettings(subdivLevel, subdivMaxTriangles);
        if (HdRenderIndex *index = GetRenderIndex()) {
            // A new shutter resamples the motion of every moving prim type
            // (meshes, spheres, curves, points, volumes, instancers and
            // cameras; each rprim rebuilds when its sampled shutter differs);
            // new subdivision settings re-resolve the meshes.
            TfTokenVector types{HdPrimTypeTokens->mesh};
            HdDataSourceLocatorSet locators;
            HdDirtyBits legacyBits = 0;
            if (shutterChanged) {
                types.push_back(HdPrimTypeTokens->sphere);
                types.push_back(HdPrimTypeTokens->basisCurves);
                types.push_back(HdPrimTypeTokens->points);
                types.push_back(HdPrimTypeTokens->volume);
                types.push_back(HdPrimTypeTokens->instancer);
                types.push_back(HdPrimTypeTokens->camera);
                locators.insert(HdXformSchema::GetDefaultLocator());
                locators.insert(HdPrimvarsSchema::GetDefaultLocator());
                legacyBits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyNormals |
                              HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyInstancer;
            }
            if (subdivChanged) {
                locators.insert(HdLegacyDisplayStyleSchema::GetDefaultLocator());
                legacyBits |= HdChangeTracker::DirtyDisplayStyle;
            }
            if (!HdNovaResyncSceneIndex::DirtyPrims(index->GetInstanceName(), types, locators)) {
                // Legacy scene delegates (no scene index emulation).
                HdChangeTracker &tracker = index->GetChangeTracker();
                tracker.MarkAllRprimsDirty(legacyBits);
                if (shutterChanged && !hdCamera->GetId().IsEmpty())
                    tracker.MarkSprimDirty(hdCamera->GetId(), HdCamera::DirtyTransform);
            }
            _bindings.clear(); // restart once the updated scene arrives
            return;
        }
    }

    // Velocity blur defaults of the render settings (Karma's karma:object:
    // vblur and friends, docs/design/M8.md D19), this render index's own: a
    // change re-resolves the motion of the prims it affects; render after
    // the sync.
    if (HdNovaVelocityModeSceneIndexRefPtr velocity = HdNovaVelocityModeSceneIndex::Find(GetRenderIndex()))
        if (velocity->SetDefaults(_delegate->GetVelocityDefaults())) {
            thread.StopRender();
            _bindings.clear();
            return;
        }
    // The other per-object defaults of the render settings (Karma's curve
    // and point styles, width scale, emission as light, dicing; docs/design/
    // M11-P1.md): prims that used the old ones re-sync, render after it.
    const bool defaultsChanged = param.SetObjectDefaults(_delegate->GetObjectDefaults()) || _defaultsChanged;
    _defaultsChanged = false;
    if (defaultsChanged)
        if (HdRenderIndex *index = GetRenderIndex()) {
            thread.StopRender();
            const TfTokenVector types{HdPrimTypeTokens->mesh, HdPrimTypeTokens->basisCurves, HdPrimTypeTokens->points,
                                      HdPrimTypeTokens->sphere, HdPrimTypeTokens->volume};
            HdDataSourceLocatorSet locators;
            locators.insert(HdPrimvarsSchema::GetDefaultLocator());
            if (!HdNovaResyncSceneIndex::DirtyPrims(index->GetInstanceName(), types, locators))
                index->GetChangeTracker().MarkAllRprimsDirty(HdChangeTracker::DirtyPrimvar);
            _bindings.clear();
            return;
        }

    View view = _ComputeView(state, width, height);
    view.frameTime = shutter.FrameTime();
    if (const auto *novaCamera = dynamic_cast<const HdNovaCamera *>(hdCamera))
        if (novaCamera->GetMotionSteps().size() >= 2 && novaCamera->GetSampledShutter() == shutter)
            view.cameraMotion = novaCamera->GetMotionSteps();

    // A camera that gives no rays: a NaN or infinite parameter, a zero
    // aperture or scale, equal clipping planes. Nothing is rendered; the
    // buffers hold their clear values and count as converged
    // (husk and nova_cli would otherwise wait forever: a NaN never equals the
    // view of the last Execute, so every Execute would restart the render).
    if (!isInvertible(hdCamera->GetTransform()) || !isInvertible(view.worldToView) ||
        !isInvertible(view.projection) || !std::all_of(view.cameraMotion.begin(), view.cameraMotion.end(), isFinite)) {
        if (!_invalidView || !sameBindings(bindings, _bindings)) {
            thread.StopRender();
            // The bindings first: they switch int buffers of float images to
            // float32, so the clear value is written as a float.
            _delegate->SetAovBindings(bindings);
            for (const HdRenderPassAovBinding &b : bindings)
                if (auto *rb = static_cast<HdNovaRenderBuffer *>(b.renderBuffer)) {
                    rb->Clear(b.clearValue);
                    rb->SetConverged(true);
                }
            _bindings = bindings;
        }
        if (!_invalidView) {
            HD_NOVA_WARN("the camera %s gives no rays (a non-finite or singular transform or projection: a NaN "
                         "parameter, a zero aperture or scale, equal clipping planes); nothing to render",
                         hdCamera->GetId().GetText());
            _invalidView = true;
        }
        return;
    }
    const bool viewRestored = _invalidView; // the buffers hold clear values: render again
    _invalidView = false;
    // Compare setting values rather than the delegate's settings version:
    // Houdini sends many unrelated keys (houdini:frame, houdini:interactive...).
    nova::RenderSettings settings = _delegate->GetNovaSettings();
    settings.lpe = _delegate->GetLpeProgram(bindings);
    settings.utility = _delegate->GetUtilityRequests(bindings);
    settings.crypto = _delegate->GetCryptoRequests(bindings);
    settings.deep = _delegate->GetDeepRequest();
    // A Karma Texture Baker's settings (M11 P8): the film is a texture tile.
    settings.bake = _delegate->GetBakeSettings();

    // Displacement dicing (M4.6): the view sets where meshes dice from. The
    // viewport keeps its first one (camera moves do not re-dice); a final
    // render (husk, nova_cli) dices every frame from that frame's own camera,
    // so a frame of a sequence is the frame rendered alone. Displaced meshes
    // waiting for the view, meshes diced from another one, meshes of new
    // dicing settings, of materials whose displacement changed and, on a new
    // frame, those whose displacement reads the time are re-synced before
    // rendering.
    {
        param.SetFrame(settings.frame, settings.fps);
        SdfPathVector resync;
        if (param.TakeFrameChanged())
            resync = param.TimeDisplacedMeshes();
        const float qualityScale = _delegate->GetDicingQualityScale(), offscreen = _delegate->GetOffscreenQuality();
        if (qualityScale != param.GetDicingQualityScale() || offscreen != param.GetOffscreenQuality()) {
            param.SetDicingSettings(qualityScale, offscreen);
            resync = param.DisplacedMeshes(); // includes the time-dependent ones
        }
        if (view.ndcScale[1] > 0.0f) {
            const nova::DicingView dicing = dicingViewOf(view.worldToView, view.projection, 2.0f / view.ndcScale[1]);
            const bool known = param.GetDicingView().valid;
            const bool viewport = _delegate->GetRenderSetting<bool>(_tokens->viewport, false);
            if (!known || (!viewport && HdNovaRenderParam::DicingViewKey(dicing) != param.GetDicingViewKey())) {
                param.SetDicingView(dicing);
                // Meshes of a fixed rate keep their geometry (their sync
                // finds nothing changed).
                if (known)
                    for (SdfPath const &p : param.DisplacedMeshes())
                        resync.push_back(p);
            }
            for (SdfPath const &p : param.TakeWaitingMeshes())
                resync.push_back(p);
        }
        for (SdfPath const &p : param.GetMaterials().TakePendingMeshes())
            resync.push_back(p);
        std::sort(resync.begin(), resync.end());
        resync.erase(std::unique(resync.begin(), resync.end()), resync.end());
        if (!resync.empty())
            if (HdRenderIndex *index = GetRenderIndex()) {
                thread.StopRender();
                HdDataSourceLocatorSet locators;
                locators.insert(HdPrimvarsSchema::GetDefaultLocator());
                if (!HdNovaResyncSceneIndex::DirtyPaths(index->GetInstanceName(), resync, locators))
                    for (SdfPath const &p : resync)
                        index->GetChangeTracker().MarkRprimDirty(p, HdChangeTracker::DirtyPrimvar);
                _bindings.clear(); // render after the next sync
                return;
            }
    }

    // Light filters whose space is another prim follow that prim, which may
    // have moved without the filter being synced (docs/design/M11-P4.md):
    // an edit restarts the render below.
    for (HdNovaLightFilter *filter : param.SpaceFilters())
        filter->RefreshSpace(param);

    bool restart = _delegate->ConsumeNeedsReset() || viewRestored;
    const HdNovaDenoiseConfig denoise = _delegate->GetDenoiseConfig();
    restart |= !(denoise == _denoise);
    // Texture settings: new conversions apply to textures prepared from now
    // on; blur and the filter width to every lookup.
    const HdNovaTextureOptions textureOptions = _delegate->GetTextureOptions();
    if (!(textureOptions == param.GetSurfaceTextures().GetOptions())) {
        thread.StopRender();
        param.GetSurfaceTextures().SetOptions(textureOptions);
        restart = true;
    }
    // The material of geometry without one (nova:defaultMaterial); the
    // scene's own is the matte one.
    if (const bool plastic = param.GetMaterials().PlasticDefault(); int(plastic) != _defaultMaterial) {
        thread.StopRender();
        scene.updateMaterial(nova::kDefaultMaterialId, param.GetMaterials().DefaultMaterial());
        if (const nova::MaterialId withOpacity = param.GetMaterials().OpacityDefault(); withOpacity != nova::kInvalidId)
            scene.updateMaterial(withOpacity, param.GetMaterials().DefaultMaterial(true));
        _defaultMaterial = int(plastic);
    }
    const int threads = _delegate->GetThreadLimit(int(WorkGetConcurrencyLimit()));
    restart |= threads != _threads;
    restart |= !(view == _view);
    restart |= width != _width || height != _height;
    restart |= param.GetEditVersion() != _editVersion || scene.version() != _sceneVersion;
    // Settings that hold a NaN differ from themselves: compared plainly they
    // would look new on every Execute and restart the render each time. They
    // count as changed once, when the NaN arrives (the delegate reads its
    // settings NaN-free; this keeps a setting that slips through from
    // hanging husk).
    restart |= settings != _settings && (settings == settings || _settings == _settings);
    const bool bindingsChanged = !sameBindings(bindings, _bindings);
    restart |= bindingsChanged;
    if (!restart)
        return;
    // New buffers or a new size start empty; otherwise the buffers keep the
    // last image until the first pass of the restart replaces it (a moving
    // camera restarts every frame).
    const bool newBuffers = bindingsChanged || width != _width || height != _height;

    thread.StopRender();
    // Interrupting the old render flags a reset; this restart covers it.
    _delegate->ConsumeNeedsReset();

    // The shading-side object defaults (fake caustics and their style,
    // internal reflection, the roughness clamp of true caustics).
    scene.setObjectDefaults(settings.limits);
    // Ray visibility and holdouts of objects without their own (M11 P2).
    scene.setDefaultVisibility(settings.objectVisibility);
    scene.setBackgroundOccludesShadows(settings.backgroundOccludesShadows);
    // Karma's outlines of objects without their own, and the look-dev light
    // overrides (the headlight looks along the camera; M11 P9a).
    scene.setDefaultOutline(settings.objectOutline);
    scene.setLightOverride(settings.look.lights, settings.look.autoHeadlight, toNova(view.worldToView.GetInverse()));
    // Volume majorants of Time/Frame-driven shaders bound the rendered frame.
    scene.setShadeFrame(settings.frame, settings.fps);
    scene.commit();
    // Volume shaders whose extinction intervals give no bound render with a
    // sampled majorant (docs/design/M8.md D7): unbiased, but slower and noisier.
    for (nova::VolumeId v : scene.activeVolumes()) {
        const nova::VolumeRecord &rec = scene.volumeRecord(v);
        if (rec.estimatedCells > 0 && _warnedVolumes.insert(rec.desc.name).second)
            HD_NOVA_WARN("volume %s: its shader has no extinction bound in %u of %zu cells (an operation without "
                         "an interval rule, e.g. a noise or a texture); it renders slower and noisier",
                         rec.desc.name.c_str(), rec.estimatedCells, rec.layout.cellCount());
    }
    if (!scene.activeVolumes().empty())
        TF_DEBUG(HDNOVA_VOLUME).Msg("[Nova] majorant violations so far: %llu\n",
                                    static_cast<unsigned long long>(nova::majorantViolations()));
    if (TfDebug::IsEnabled(HDNOVA_SSS)) {
        uint64_t walks = 0, capped = 0;
        nova::subsurfaceWalkStats(walks, capped);
        TF_DEBUG(HDNOVA_SSS).Msg("[Nova] subsurface walks so far: %llu, killed at the segment cap: %llu\n",
                                 static_cast<unsigned long long>(walks), static_cast<unsigned long long>(capped));
    }
    _editVersion = param.GetEditVersion();
    _sceneVersion = scene.version();
    _settings = settings;
    _view = view;
    _width = width;
    _height = height;
    _bindings = bindings;

    nova::Camera camera;
    camera.ndcToView = toNova(view.projection.GetInverse());
    camera.viewToWorld = toNova(view.worldToView.GetInverse());
    camera.worldToView = toNova(view.worldToView);
    camera.lensRadius = view.lensRadius;
    camera.focusDistance = view.focusDistance;
    camera.frameTime = view.frameTime;
    std::vector<nova::Mat4> cameraSteps;
    for (GfMatrix4d const &m : view.cameraMotion)
        cameraSteps.push_back(toNova(m));
    camera.setMotion(cameraSteps.data(), uint32_t(cameraSteps.size()));
    camera.rasterToNdcScale = {view.ndcScale[0], view.ndcScale[1]};
    camera.rasterToNdcOffset = {view.ndcOffset[0], view.ndcOffset[1]};
    camera.imageOriginX = view.imageOriginX;
    camera.imageOriginY = view.imageOriginY;

    renderer.setThreadLimit(threads);
    _threads = threads;
    renderer.setCamera(camera);
    renderer.setSettings(settings);
    renderer.resize(width, height);
    renderer.setRegion(view.region);
    // The viewport (not husk): proxy passes for a fast first image and BVH
    // refits for deformations (docs/design/M7.md D6, D7).
    const bool viewport = _delegate->GetRenderSetting<bool>(_tokens->viewport, false);
    renderer.setProxyDepth(viewport ? _delegate->GetIprProxyDepth() : 0);
    scene.setInteractive(viewport);
    renderer.reset();

    // The bindings first: they switch int buffers of float images (Karma's
    // ids) to float32, so the clear value below is written as a float.
    _delegate->SetAovBindings(bindings);
    // Mark the buffers unconverged before the thread starts, so the host never
    // sees the previous frame's convergence.
    for (const HdRenderPassAovBinding &b : bindings) {
        if (auto *rb = static_cast<HdNovaRenderBuffer *>(b.renderBuffer)) {
            if (newBuffers)
                rb->Clear(b.clearValue);
            rb->SetConverged(false);
        }
    }

    _denoise = denoise;
    _delegate->SetDenoiseConfig(denoise);
    _delegate->OnRenderStarted();
    thread.StartRender();
}

PXR_NAMESPACE_CLOSE_SCOPE
