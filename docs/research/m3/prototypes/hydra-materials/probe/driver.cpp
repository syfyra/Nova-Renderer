// probe_driver: loads a stage through UsdImaging scene indices into a render
// index with the hdProbe delegate (scene index emulation), syncs, applies USD
// edits from a command file and syncs again, so the probe logs the dirty bits
// Hydra 2 delivers for each edit. Throwaway research tool.
//
//   probe_driver scene.usda [commands.txt]
// commands: sync <label> | set <attrPath> <values...> | connect <inputAttr> <sourceAttr>
//           | disconnect <inputAttr> | bind <primPath> <materialPath> [purpose] | remove <primPath>
#include <pxr/pxr.h>

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

class ProbeTask final : public HdTask {
public:
    ProbeTask(HdRenderPassSharedPtr pass, HdRenderPassStateSharedPtr state)
        : HdTask(SdfPath::EmptyPath()), _pass(std::move(pass)), _state(std::move(state)) {}
    void Sync(HdSceneDelegate *, HdTaskContext *, HdDirtyBits *dirtyBits) override {
        _pass->Sync();
        *dirtyBits = HdChangeTracker::Clean;
    }
    void Prepare(HdTaskContext *, HdRenderIndex *index) override { _state->Prepare(index->GetResourceRegistry()); }
    void Execute(HdTaskContext *) override { _pass->Execute(_state, _tags); }
    const TfTokenVector &GetRenderTags() const override { return _tags; }

private:
    HdRenderPassSharedPtr _pass;
    HdRenderPassStateSharedPtr _state;
    TfTokenVector _tags{HdRenderTagTokens->geometry, HdRenderTagTokens->render};
};

bool setAttr(UsdStageRefPtr const &stage, std::string const &path, std::vector<std::string> const &vals) {
    UsdAttribute a = stage->GetAttributeAtPath(SdfPath(path));
    if (!a) {
        std::fprintf(stderr, "no attribute %s\n", path.c_str());
        return false;
    }
    const SdfValueTypeName t = a.GetTypeName();
    auto f = [&](size_t i) { return float(std::atof(vals.at(i).c_str())); };
    if (t == SdfValueTypeNames->Float) return a.Set(f(0));
    if (t == SdfValueTypeNames->Int) return a.Set(std::atoi(vals.at(0).c_str()));
    if (t == SdfValueTypeNames->Color3f || t == SdfValueTypeNames->Float3 || t == SdfValueTypeNames->Vector3f)
        return a.Set(GfVec3f(f(0), f(1), f(2)));
    if (t == SdfValueTypeNames->String) return a.Set(vals.at(0));
    if (t == SdfValueTypeNames->Token) return a.Set(TfToken(vals.at(0)));
    if (t == SdfValueTypeNames->Asset) return a.Set(SdfAssetPath(vals.at(0)));
    std::fprintf(stderr, "unsupported type %s\n", t.GetAsToken().GetText());
    return false;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: probe_driver scene.usda [commands.txt]\n");
        return 2;
    }
    PlugRegistry::GetInstance(); // PXR_PLUGINPATH_NAME must contain the hdProbe plugInfo
    UsdStageRefPtr stage = UsdStage::Open(argv[1]);
    if (!stage)
        return 2;

    HdPluginRenderDelegateUniqueHandle delegate =
        HdRendererPluginRegistry::GetInstance().CreateRenderDelegate(TfToken("HdProbeRendererPlugin"));
    if (!delegate) {
        std::fprintf(stderr, "cannot create HdProbeRendererPlugin\n");
        return 2;
    }
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(delegate.Get(), HdDriverVector(), "probe"));
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    info.addDrawModeSceneIndex = false;
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    sis.stageSceneIndex->SetTime(UsdTimeCode::Default());
    index->InsertSceneIndex(sis.finalSceneIndex, SdfPath::AbsoluteRootPath(), false);

    const HdRprimCollection collection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined));
    HdRenderPassSharedPtr pass = delegate->CreateRenderPass(index.get(), collection);
    auto state = std::make_shared<HdRenderPassState>();
    HdTaskSharedPtrVector tasks{std::make_shared<ProbeTask>(pass, state)};
    HdEngine engine;

    auto sync = [&](std::string const &label) {
        std::fprintf(stderr, "\n######## SYNC: %s ########\n", label.c_str());
        sis.stageSceneIndex->ApplyPendingUpdates();
        engine.Execute(index.get(), &tasks);
    };
    sync("initial");

    if (argc > 2) {
        std::ifstream in(argv[2]);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            std::vector<std::string> w = TfStringTokenize(line);
            const std::string &cmd = w[0];
            std::fprintf(stderr, "\n>>> %s\n", line.c_str());
            if (cmd == "sync") {
                sync(w.size() > 1 ? w[1] : "");
            } else if (cmd == "set") {
                setAttr(stage, w[1], std::vector<std::string>(w.begin() + 2, w.end()));
            } else if (cmd == "connect") {
                UsdAttribute dst = stage->GetAttributeAtPath(SdfPath(w[1]));
                if (!dst) {
                    UsdPrim p = stage->GetPrimAtPath(SdfPath(w[1]).GetPrimPath());
                    UsdAttribute srcA = stage->GetAttributeAtPath(SdfPath(w[2]));
                    dst = p.CreateAttribute(SdfPath(w[1]).GetNameToken(), srcA.GetTypeName());
                }
                UsdShadeConnectableAPI::ConnectToSource(UsdShadeInput(dst), SdfPath(w[2]));
            } else if (cmd == "disconnect") {
                UsdShadeConnectableAPI::DisconnectSource(stage->GetAttributeAtPath(SdfPath(w[1])));
            } else if (cmd == "bind") {
                UsdPrim p = stage->GetPrimAtPath(SdfPath(w[1]));
                UsdShadeMaterial m(stage->GetPrimAtPath(SdfPath(w[2])));
                UsdShadeMaterialBindingAPI::Apply(p).Bind(
                    m, UsdShadeTokens->fallbackStrength,
                    w.size() > 3 ? TfToken(w[3]) : UsdShadeTokens->allPurpose);
            } else if (cmd == "remove") {
                stage->RemovePrim(SdfPath(w[1]));
            } else {
                std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
            }
        }
    }
    return 0;
}
