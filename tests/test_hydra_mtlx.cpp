// MaterialX inside hdNova (M3.0 spike S1): the delegate finds and loads
// Houdini's MaterialX libraries without any environment set up by Houdini (as
// in nova_cli), and the MaterialX calls the translator relies on behave as
// verified in docs/research/m3/mtlx-nodes.md.

#include "hydra/material/nodeDefDatabase.h"

#include <MaterialXCore/Document.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace mx = MaterialX;
PXR_NAMESPACE_USING_DIRECTIVE

TEST_CASE("MaterialX libraries load next to MaterialXCore.dll", "[hydra][mtlx]") {
    HdNovaNodeDefDatabase const &db = HdNovaNodeDefDatabase::Get();
    INFO(db.Describe());
    REQUIRE(!db.GetStdlibRoot().empty());
    mx::ConstDocumentPtr lib = db.GetLibrary();
    REQUIRE(lib);
    CHECK(db.GetFileCount() >= 50);
    CHECK(lib->getNodeDefs().size() >= 890);
    CHECK(lib->getNodeGraphs().size() >= 250);
    // Loading is a one-off cost per process (measured 41 ms); a generous
    // bound only catches pathological cases such as loading every folder.
    CHECK(db.GetLoadMilliseconds() < 1000.0);
    CHECK(&HdNovaNodeDefDatabase::Get() == &db);
}

TEST_CASE("Standard Surface node definitions carry the 1.0.1 defaults", "[hydra][mtlx]") {
    mx::ConstDocumentPtr lib = HdNovaNodeDefDatabase::Get().GetLibrary();
    mx::ConstNodeDefPtr ss = lib->getNodeDef("ND_standard_surface_surfaceshader");
    REQUIRE(ss);
    mx::ConstInputPtr base = ss->getActiveInput("base"), baseColor = ss->getActiveInput("base_color");
    REQUIRE(base);
    REQUIRE(baseColor);
    CHECK(base->getValueString() == "1.0");
    CHECK(baseColor->getValueString() == "0.8, 0.8, 0.8");
    CHECK(ss->getActiveInput("normal")->getDefaultGeomPropString() == "Nworld");
    // Intrinsic models must exist so the translator can keep them unexpanded.
    CHECK(lib->getNodeDef("ND_open_pbr_surface_surfaceshader"));
    CHECK(lib->getNodeDef("ND_UsdPreviewSurface_surfaceshader"));
    CHECK(lib->getNodeDef("ND_surface_unlit"));
}

TEST_CASE("Flattening a nodegraph node against the shared library inserts its geometry inputs", "[hydra][mtlx]") {
    // A per-material document that references the shared library instead of
    // copying it (setDataLibrary, ~0.2 ms instead of 18 ms per material).
    mx::DocumentPtr doc = mx::createDocument();
    doc->setDataLibrary(std::const_pointer_cast<mx::Document>(HdNovaNodeDefDatabase::Get().GetLibrary()));
    mx::NodeGraphPtr g = doc->addNodeGraph("NG_test");
    mx::NodePtr tiled = g->addNode("tiledimage", "tiled1", "color3");
    tiled->setInputValue("file", std::string("C:/tex/a.exr"), "filename");
    tiled->setInputValue("uvtiling", mx::Vector2(4.0f, 4.0f));
    g->addOutput("out", "color3")->setConnectedNode(tiled);
    REQUIRE(tiled->getNodeDef());
    CHECK(tiled->getNodeDef()->getName() == "ND_tiledimage_color3");

    g->flattenSubgraphs();
    bool hasImage = false, hasTexcoord = false, hasTiled = false;
    for (mx::NodePtr node : g->getNodes()) {
        hasImage |= node->getCategory() == "image";
        hasTexcoord |= node->getCategory() == "texcoord"; // the unconnected texcoord's defaultgeomprop UV0
        hasTiled |= node->getCategory() == "tiledimage";
    }
    CHECK(hasImage);
    CHECK(hasTexcoord);
    CHECK(!hasTiled);
}
