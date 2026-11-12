#pragma once

// The active render pass: husk --pass, the pass chosen in the Solaris viewport.
//
// A UsdRenderPass names its render settings (renderSource, which the host
// reads) and four collections. The host puts the pass's path into the scene
// globals (activeRenderPassPrim); what the collections do to the scene is the
// renderer's to apply, as Karma's own scene index does for Karma
// (BRAY_HdRenderPassSceneIndex) and HdPrman's for RenderMan. Without it every
// pass rendered the whole scene.
//
//   prune             what is in it is not in the scene;
//   renderVisibility  geometry and lights outside it are invisible;
//   cameraVisibility  geometry outside it is invisible to camera rays:
//                     karma:object:rendervisibility with "-primary";
//   matte             geometry in it is a matte: karma:object:holdoutmode.
//
// The last two are the constant primvars Nova reads for Karma's Render
// Geometry Settings (hydra/primvarUtil.h), so the prims downstream are what
// they would be with those settings authored. A collection without a
// membership expression says nothing. Like pxr's and HdPrman's indices this
// one matches the paths of the scene index: geometry inside a native instance
// has its prototype's path there, which no collection names.
//
// pxr's HdsiRenderPassPruneSceneIndex is not used for the pruning: it reads
// the pass when its input's first prims arrive, and never for an input that
// is populated before the index exists (a render index made for a stage that
// is up).

#include <pxr/imaging/hd/collectionExpressionEvaluator.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/pathExpression.h>

#include <optional>

PXR_NAMESPACE_OPEN_SCOPE

TF_DECLARE_WEAK_AND_REF_PTRS(HdNovaRenderPassSceneIndex);

class HdNovaRenderPassSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static HdNovaRenderPassSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input);
    ~HdNovaRenderPassSceneIndex() override;

    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const &primPath) const override;

protected:
    void _PrimsAdded(HdSceneIndexBase const &sender, HdSceneIndexObserver::AddedPrimEntries const &entries) override;
    void _PrimsRemoved(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::RemovedPrimEntries const &entries) override;
    void _PrimsDirtied(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::DirtiedPrimEntries const &entries) override;

private:
    explicit HdNovaRenderPassSceneIndex(HdSceneIndexBaseRefPtr const &input);

    // What the pass does to a prim.
    struct Effect {
        bool hidden = false, noCamera = false, matte = false;
        bool operator==(Effect const &) const = default;
        bool any() const { return hidden || noCamera || matte; }
    };
    struct Pass {
        SdfPath path;
        SdfPathExpression prune, renderVisibility, cameraVisibility, matte;
        std::optional<HdCollectionExpressionEvaluator> pruned, visible, camera, mattes;

        bool any() const { return pruned || visible || camera || mattes; }
        bool prunes(SdfPath const &primPath) const { return pruned && pruned->Match(primPath); }
        Effect effectOn(SdfPath const &primPath, TfToken const &primType) const;
    };
    // What a change of the pass does to the prims downstream.
    struct Changes {
        HdSceneIndexObserver::RemovedPrimEntries removed;
        HdSceneIndexObserver::AddedPrimEntries added;
        HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    };
    // Reads the scene globals' pass and its collections; with `changes`, what
    // that does to the prims is put there.
    void _Update(Changes *changes);
    void _Send(Changes const &changes);

    Pass _pass;
    bool _populated = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
