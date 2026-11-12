#pragma once

#include "denoise.h"

#include "core/integrator/settings.h"
#include "core/integrator/wavefront.h"
#include "core/scene/camera.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/pxr.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaRenderDelegate;

class HdNovaRenderPass final : public HdRenderPass {
public:
    HdNovaRenderPass(HdRenderIndex *index, HdRprimCollection const &collection,
                     HdNovaRenderDelegate *delegate);
    ~HdNovaRenderPass() override;

    bool IsConverged() const override;

protected:
    void _Execute(HdRenderPassStateSharedPtr const &renderPassState,
                  TfTokenVector const &renderTags) override;

private:
    // How film pixels map to the camera: the projection and the rectangle of
    // film pixels to render, with the raster -> NDC mapping for that rectangle.
    struct View {
        GfMatrix4d worldToView{1.0};
        GfMatrix4d projection{1.0};
        nova::TileRect region;
        GfVec2f ndcScale{0.0f};
        GfVec2f ndcOffset{0.0f};
        // Film position in the full image (see nova::Camera::imageOriginX).
        int imageOriginX = 0;
        int imageOriginY = 0;
        // Thin lens (see nova::Camera::lensRadius).
        float lensRadius = 0.0f;
        float focusDistance = 0.0f;
        // Camera motion: camera-to-world at uniform shutter steps (empty if
        // static) and the shutter position of the frame.
        std::vector<GfMatrix4d> cameraMotion;
        float frameTime = 0.0f;

        bool operator==(View const &o) const {
            return worldToView == o.worldToView && projection == o.projection && region.x0 == o.region.x0 &&
                   region.y0 == o.region.y0 && region.x1 == o.region.x1 && region.y1 == o.region.y1 &&
                   ndcScale == o.ndcScale && ndcOffset == o.ndcOffset && imageOriginX == o.imageOriginX &&
                   imageOriginY == o.imageOriginY && lensRadius == o.lensRadius && focusDistance == o.focusDistance &&
                   cameraMotion == o.cameraMotion && frameTime == o.frameTime;
        }
    };

    View _ComputeView(HdRenderPassStateSharedPtr const &state, int width, int height) const;

    HdNovaRenderDelegate *_delegate;

    // State of the running render; any change restarts it.
    View _view;
    std::set<std::string> _warnedVolumes; // volumes warned about (estimated majorants)
    int _width = 0;
    int _height = 0;
    uint64_t _editVersion = ~0ull;
    uint64_t _sceneVersion = ~0ull;
    int _threads = 0;          // the renderer's thread limit (nova:threads)
    int _defaultMaterial = -1; // the scene's default material: 1 Karma's plastic, 0 the matte one, -1 not set yet
    nova::RenderSettings _settings;
    HdRenderPassAovBindingVector _bindings;
    HdNovaDenoiseConfig _denoise; // docs/design/M7.md D2, D3
    // A batch render (husk) whose camera is not in the stage: reported once
    // and converged at once, so the render ends instead of waiting forever.
    bool _missingCamera = false;
    // The camera gives no rays (a NaN parameter, a zero aperture or scale,
    // equal clipping planes): reported once; the buffers hold their clear
    // values and count as converged until the view is valid again. A NaN in
    // the view would otherwise never equal the view of the last Execute and
    // restart the render every time.
    bool _invalidView = false;
    // The render settings' per-object defaults changed when this pass was
    // created, after prims had used the old ones (a pass recreated in a
    // session): the first execute re-syncs them.
    bool _defaultsChanged = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
