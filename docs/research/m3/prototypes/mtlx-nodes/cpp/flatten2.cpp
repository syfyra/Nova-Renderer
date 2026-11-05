// Checks MaterialX::GraphElement::flattenSubgraphs on Houdini's MaterialX 1.39.5:
// what happens to unconnected interface inputs with defaultgeomprop.
#include <MaterialXCore/Document.h>
#include <MaterialXFormat/File.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>
#include <chrono>
#include <iostream>

namespace mx = MaterialX;

int main(int argc, char **argv) {
    mx::FileSearchPath sp("C:/Program Files/Side Effects Software/Houdini 22.0.466/houdini/materialx");
    mx::DocumentPtr lib = mx::createDocument();
    auto t0 = std::chrono::steady_clock::now();
    mx::StringSet files = mx::loadLibraries({"libraries"}, sp, lib);
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "loaded " << files.size() << " files, " << lib->getNodeDefs().size() << " nodedefs, "
              << lib->getNodeGraphs().size() << " nodegraphs in "
              << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms\n";

    auto ti0 = std::chrono::steady_clock::now();
    { mx::DocumentPtr tmp = mx::createDocument(); tmp->importLibrary(lib); }
    auto ti1 = std::chrono::steady_clock::now();
    std::cout << "importLibrary copy: " << std::chrono::duration<double, std::milli>(ti1 - ti0).count() << " ms" << std::endl;
    mx::DocumentPtr doc = mx::createDocument();
    if (argc > 1) doc->importLibrary(lib); else doc->setDataLibrary(lib);
    mx::NodeGraphPtr g = doc->addNodeGraph("NG_test");
    mx::NodePtr tri = g->addNode("triplanarprojection", "tri", "color3");
    tri->setInputValue("file", std::string("C:/tex/a.png"), "filename");
    mx::NodePtr ti = g->addNode("tiledimage", "ti", "color3");
    ti->setInputValue("uvtiling", mx::Vector2(4, 4));
    mx::NodePtr ss = g->addNode("standard_surface", "ss", "surfaceshader");
    mx::NodePtr cb = g->addNode("checkerboard", "cb", "color3");
    mx::NodePtr mixn = g->addNode("mix", "m", "color3");
    mixn->setConnectedNode("fg", tri);
    mixn->setConnectedNode("bg", ti);
    mixn->setConnectedNode("mix", nullptr);
    ss->setConnectedNode("base_color", mixn);
    ss->setConnectedNode("coat_color", cb);
    mx::OutputPtr out = g->addOutput("out", "surfaceshader");
    out->setConnectedNode(ss);

    std::cout << "nodedef of tri: " << (tri->getNodeDef() ? tri->getNodeDef()->getName() : "none") << "\n";
    auto t2 = std::chrono::steady_clock::now();
    // Keep the uber shader native, flatten everything else.
    g->flattenSubgraphs(mx::EMPTY_STRING, [](mx::NodePtr n) { return n->getCategory() != "standard_surface"; });
    auto t3 = std::chrono::steady_clock::now();
    std::cout << "flatten took " << std::chrono::duration<double, std::milli>(t3 - t2).count() << " ms; nodes: "
              << g->getNodes().size() << "\n";
    mx::XmlWriteOptions wo;
    wo.elementPredicate = [&](mx::ConstElementPtr e) { return !e->hasSourceUri(); };
    std::cout << "tiledimage nodedef via datalib: " << (doc->getNodeDef("ND_tiledimage_color3") ? "found" : "missing") << std::endl;
    std::string xml = mx::writeToXmlString(doc, &wo);
    std::cout << xml << "\n";
    std::string msg;
    std::cout << "valid: " << doc->validate(&msg) << " " << msg << "\n";
    return 0;
}
