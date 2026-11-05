// Verifies the MaterialX C++ calls recommended for Nova's nodedef database
// (Houdini 22 / MaterialX 1.39.5). Throwaway.
#include <MaterialXCore/Document.h>
#include <MaterialXFormat/File.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace mx = MaterialX;

static void dumpNodeDef(mx::DocumentPtr const &lib, const char *name) {
    mx::NodeDefPtr nd = lib->getNodeDef(name);
    if (!nd) {
        std::printf("%s: <no nodedef>\n", name);
        return;
    }
    std::printf("%s: node=%s type=%s version=%s inherit=%s\n", name, nd->getNodeString().c_str(),
                nd->getType().c_str(), nd->getVersionString().c_str(), nd->getInheritString().c_str());
    int n = 0;
    for (mx::InputPtr in : nd->getActiveInputs()) {
        if (n++ < 6 || in->getName() == "base" || in->getName() == "base_color" || in->getName() == "normal")
            std::printf("    in %-20s %-10s value='%s' defaultgeomprop='%s' uniform=%d enum='%s'\n",
                        in->getName().c_str(), in->getType().c_str(), in->getValueString().c_str(),
                        in->getDefaultGeomPropString().c_str(), in->getIsUniform(),
                        in->getAttribute("enum").c_str());
    }
    std::printf("    (%zu active inputs, %zu outputs)\n", nd->getActiveInputs().size(), nd->getActiveOutputs().size());
    mx::InterfaceElementPtr impl = nd->getImplementation();
    if (!impl) {
        std::printf("    implementation: none\n");
    } else if (mx::NodeGraphPtr ng = impl->asA<mx::NodeGraph>()) {
        std::printf("    implementation: nodegraph %s with %zu nodes:", ng->getName().c_str(), ng->getNodes().size());
        int k = 0;
        for (mx::NodePtr node : ng->getNodes())
            if (k++ < 12)
                std::printf(" %s(%s)", node->getName().c_str(), node->getCategory().c_str());
        std::printf("\n");
    } else {
        std::printf("    implementation: %s (%s)\n", impl->getCategory().c_str(), impl->getName().c_str());
    }
}

int main(int argc, char **argv) {
    const char *hfs = std::getenv("HFS");
    std::string root = hfs ? std::string(hfs) + "/houdini/materialx/libraries"
                           : "C:/Program Files/Side Effects Software/Houdini 22.0.466/houdini/materialx/libraries";
    auto t0 = std::chrono::steady_clock::now();
    mx::DocumentPtr lib = mx::createDocument();
    mx::FileSearchPath search(root);
    // Folders relative to the search path; the default (empty) loads everything below it.
    mx::FilePathVec folders = {"stdlib", "pbrlib", "bxdf", "cmlib", "nprlib", "lights", "houdini"};
    mx::StringSet loaded = mx::loadLibraries(folders, search, lib);
    auto t1 = std::chrono::steady_clock::now();
    std::printf("MaterialX %s: loaded %zu files, %zu nodedefs, %zu nodegraphs in %.1f ms from %s\n",
                mx::getVersionString().c_str(), loaded.size(), lib->getNodeDefs().size(),
                lib->getNodeGraphs().size(), std::chrono::duration<double, std::milli>(t1 - t0).count(),
                root.c_str());
    const char *names[] = {"ND_standard_surface_surfaceshader", "ND_open_pbr_surface_surfaceshader",
                           "ND_UsdPreviewSurface_surfaceshader", "ND_image_color3", "ND_tiledimage_color3",
                           "ND_texcoord_vector2", "ND_geompropvalue_float", "ND_normalmap_float",
                           "ND_normalmap_vector3", "ND_ramp4_color3", "ND_hmtlxpbrtextureset", "kma_rayimport",
                           "UsdPreviewSurface"};
    for (const char *n : names)
        dumpNodeDef(lib, n);

    // Nodedef by category + output type (how a translator finds e.g. a
    // replacement when only "image"/"color3" is known).
    std::vector<mx::NodeDefPtr> matches = lib->getMatchingNodeDefs("image");
    std::printf("getMatchingNodeDefs(\"image\"): %zu\n", matches.size());

    // Flatten a nodegraph-implemented node into primitive nodes.
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(lib);
    mx::NodeGraphPtr g = doc->addNodeGraph("NG_test");
    mx::NodePtr tiled = g->addNode("tiledimage", "tiled1", "color3");
    tiled->setInputValue("file", std::string("C:/tex/a.exr"), "filename");
    tiled->setInputValue("uvtiling", mx::Vector2(4.0f, 4.0f));
    mx::OutputPtr out = g->addOutput("out", "color3");
    out->setConnectedNode(tiled);
    auto t2 = std::chrono::steady_clock::now();
    g->flattenSubgraphs();
    auto t3 = std::chrono::steady_clock::now();
    std::printf("flattened tiledimage in %.3f ms -> nodes:", std::chrono::duration<double, std::milli>(t3 - t2).count());
    for (mx::NodePtr node : g->getNodes()) {
        std::printf(" %s(%s nd=%s)", node->getName().c_str(), node->getCategory().c_str(),
                    node->getNodeDef() ? node->getNodeDef()->getName().c_str() : "?");
    }
    std::printf("\n");
    mx::XmlWriteOptions wo;
    wo.elementPredicate = [](mx::ConstElementPtr e) { return !e->hasSourceUri(); };
    std::printf("%s\n", mx::writeToXmlString(doc, &wo).c_str());
    return 0;
}
