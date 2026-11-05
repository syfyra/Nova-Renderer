#include "nodeDefDatabase.h"

#include "../diagnostics.h"

#include <MaterialXFormat/File.h>
#include <MaterialXFormat/Util.h>

#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/js/json.h>
#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/stringUtils.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mx = MaterialX;

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// Houdini's library folders below the standard library root. "targets" (code
// generation) and "mdl" are not needed for rendering.
const mx::FilePathVec kStdlibFolders = {"stdlib", "pbrlib", "bxdf", "cmlib", "nprlib", "lights", "houdini"};

std::vector<std::string> EnvPathList(const char *name) {
    std::vector<std::string> out;
    for (std::string const &p : TfStringSplit(TfGetenv(name), ARCH_PATH_LIST_SEP))
        if (!p.empty())
            out.push_back(p);
    return out;
}

bool IsStdlibRoot(std::string const &dir) {
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::path(dir) / "stdlib", ec);
}

// <dir of MaterialXCore.dll>/../houdini/materialx/libraries: where the
// libraries of the MaterialX build this process uses are. Pixar's
// ArchGetAddressInfo does not find the module on Windows, so the loader is
// asked directly.
std::string LibrariesNextToMaterialXDll() {
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(L"MaterialXCore.dll");
    if (!module)
        return {};
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
        if (n == 0)
            return {};
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    std::error_code ec;
    const std::filesystem::path libs = std::filesystem::weakly_canonical(
        std::filesystem::path(path).parent_path().parent_path() / "houdini" / "materialx" / "libraries", ec);
    return ec ? std::string() : libs.generic_string();
#else
    return {};
#endif
}

// A registry value as MaterialX value text: numbers and lists of them joined
// by ", ", booleans as true/false, strings as they are.
std::string ValueText(JsValue const &v) {
    if (v.IsArray()) {
        std::string s;
        for (JsValue const &e : v.GetJsArray())
            s += (s.empty() ? "" : ", ") + ValueText(e);
        return s;
    }
    if (v.IsBool())
        return v.GetBool() ? "true" : "false";
    if (v.IsString())
        return v.GetString();
    if (v.IsInt())
        return std::to_string(v.GetInt());
    if (v.IsReal())
        return TfStringPrintf("%.9g", v.GetReal());
    return {};
}

// MaterialX type of a registry type ("float[3]" -> vector3; variadic inputs,
// the ramp key lists, are arrays).
std::string KarmaType(std::string const &type, bool variadic) {
    static const std::map<std::string, std::string> types = {
        {"float", "float"},     {"int", "integer"},     {"bool", "boolean"},    {"string", "string"},
        {"float[2]", "vector2"}, {"float[3]", "vector3"}, {"float[4]", "vector4"}, {"float[9]", "matrix33"},
        {"float[16]", "matrix44"},
    };
    const auto it = types.find(type);
    if (it == types.end())
        return {};
    return variadic ? it->second + "array" : it->second;
}

// Adds a nodedef for every kma_* shader of Karma's registry that has none;
// returns how many.
size_t LoadKarmaNodes(mx::DocumentPtr const &doc, std::string const &file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return 0;
    std::stringstream text;
    text << stream.rdbuf();
    JsParseError error;
    const JsValue root = JsParseString(text.str(), &error);
    if (!root.IsArray()) {
        HD_NOVA_WARN("cannot read Karma's shader registry '%s' (line %u): %s", file.c_str(), error.line,
                     error.reason.c_str());
        return 0;
    }
    size_t count = 0;
    for (JsValue const &entry : root.GetJsArray()) {
        if (!entry.IsObject())
            continue;
        JsObject const &node = entry.GetJsObject();
        const auto name = node.find("name");
        if (name == node.end() || !name->second.IsString() || !TfStringStartsWith(name->second.GetString(), "kma_"))
            continue;
        const std::string id = name->second.GetString();
        if (doc->getNodeDef(id))
            continue;
        mx::NodeDefPtr nd = doc->addNodeDef(id, "", id);
        bool ok = true;
        for (char const *kind : {"inputs", "outputs"}) {
            const auto list = node.find(kind);
            if (list == node.end() || !list->second.IsArray())
                continue;
            for (JsValue const &port : list->second.GetJsArray()) {
                if (!port.IsObject())
                    continue;
                JsObject const &p = port.GetJsObject();
                const auto pn = p.find("name"), pt = p.find("type"), pd = p.find("default"), pv = p.find("variadic");
                if (pn == p.end() || pt == p.end() || !pn->second.IsString() || !pt->second.IsString())
                    continue;
                const bool variadic = pv != p.end() && pv->second.IsBool() && pv->second.GetBool();
                const std::string type = KarmaType(pt->second.GetString(), variadic);
                if (type.empty()) {
                    ok = false;
                    continue;
                }
                mx::ValueElementPtr e = kind[0] == 'i' ? mx::ValueElementPtr(nd->addInput(pn->second.GetString(), type))
                                                       : mx::ValueElementPtr(nd->addOutput(pn->second.GetString(), type));
                if (pd != p.end())
                    e->setValueString(ValueText(pd->second));
            }
        }
        if (nd->getOutputs().empty())
            nd->addOutput("out", "float");
        count += ok ? 1 : 0;
    }
    return count;
}

// Karma's graph library (karmaNodeGraphs.json): [signatures, graphs].
// A signature is [name, [[input, type, default, defaultgeomprop]], [[output,
// type]]]; a graph is [nodedef, nodegraph, nodes, outputs] with nodes
// [nodedef id, name, [[input index, source node (-1: the graph's input),
// source output index]], [[input index, value]]] and outputs [kind, node,
// output index] in the signature's output order. Input indices follow the
// node definitions' input order. Only the kma graphs are added (MaterialX's
// own graphs stay MaterialX's).
size_t LoadKarmaGraphs(mx::DocumentPtr const &doc, std::string const &file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return 0;
    std::stringstream text;
    text << stream.rdbuf();
    JsParseError error;
    const JsValue root = JsParseString(text.str(), &error);
    if (!root.IsArray() || root.GetJsArray().size() < 2 || !root.GetJsArray()[0].IsArray() ||
        !root.GetJsArray()[1].IsArray()) {
        HD_NOVA_WARN("cannot read Karma's node graphs '%s'", file.c_str());
        return 0;
    }
    auto str = [](JsValue const &v) { return v.IsString() ? v.GetString() : std::string(); };
    // Signatures: default geometry properties of the kma nodedefs.
    for (JsValue const &sig : root.GetJsArray()[0].GetJsArray()) {
        if (!sig.IsArray() || sig.GetJsArray().size() < 2)
            continue;
        JsArray const &s = sig.GetJsArray();
        mx::NodeDefPtr nd = doc->getNodeDef(str(s[0]));
        if (!nd || !TfStringStartsWith(str(s[0]), "kma_") || !s[1].IsArray())
            continue;
        for (JsValue const &in : s[1].GetJsArray()) {
            if (!in.IsArray() || in.GetJsArray().size() < 4)
                continue;
            JsArray const &i = in.GetJsArray();
            if (mx::InputPtr input = nd->getInput(str(i[0])); input && !str(i[3]).empty()) {
                input->setDefaultGeomPropString(str(i[3]));
                input->removeAttribute(mx::ValueElement::VALUE_ATTRIBUTE);
            }
        }
    }
    size_t count = 0;
    for (JsValue const &g : root.GetJsArray()[1].GetJsArray()) {
        if (!g.IsArray() || g.GetJsArray().size() < 4)
            continue;
        JsArray const &graph = g.GetJsArray();
        const std::string ndName = str(graph[0]), ngName = str(graph[1]);
        mx::NodeDefPtr nd = doc->getNodeDef(ndName);
        if (!nd || !TfStringStartsWith(ndName, "kma_") || doc->getNodeGraph(ngName) || !graph[2].IsArray() ||
            !graph[3].IsArray())
            continue;
        JsArray const &nodes = graph[2].GetJsArray();
        // Node elements first (connections may point forward).
        std::vector<mx::NodeDefPtr> defs;
        std::vector<std::string> names;
        bool ok = true;
        for (JsValue const &n : nodes) {
            JsArray const &node = n.GetJsArray();
            mx::NodeDefPtr def = node.size() >= 2 ? doc->getNodeDef(str(node[0])) : nullptr;
            ok &= def != nullptr;
            defs.push_back(def);
            names.push_back(node.size() >= 2 ? str(node[1]) : std::string());
        }
        if (!ok)
            continue;
        mx::NodeGraphPtr ng = doc->addNodeGraph(ngName);
        ng->setNodeDefString(ndName);
        auto outputName = [](mx::NodeDefPtr const &def, size_t index) {
            const std::vector<mx::OutputPtr> outs = def->getActiveOutputs();
            return index < outs.size() ? outs[index]->getName() : std::string("out");
        };
        for (size_t k = 0; k < nodes.size() && ok; ++k) {
            const std::vector<mx::OutputPtr> outs = defs[k]->getActiveOutputs();
            mx::NodePtr node = ng->addNode(defs[k]->getNodeString(), names[k],
                                           outs.size() == 1 ? outs[0]->getType() : std::string("multioutput"));
            node->setNodeDefString(defs[k]->getName());
            const std::vector<mx::InputPtr> inputs = defs[k]->getActiveInputs();
            JsArray const &node_ = nodes[k].GetJsArray();
            if (node_.size() > 2 && node_[2].IsArray())
                for (JsValue const &c : node_[2].GetJsArray()) {
                    JsArray const &conn = c.GetJsArray();
                    if (conn.size() < 3 || !conn[0].IsInt() || !conn[1].IsInt() || !conn[2].IsInt() ||
                        size_t(conn[0].GetInt()) >= inputs.size()) {
                        ok = false;
                        break;
                    }
                    mx::InputPtr const &decl = inputs[size_t(conn[0].GetInt())];
                    mx::InputPtr in = node->addInput(decl->getName(), decl->getType());
                    const int source = conn[1].GetInt(), output = conn[2].GetInt();
                    if (source < 0) {
                        const std::vector<mx::InputPtr> iface = nd->getActiveInputs();
                        if (size_t(output) >= iface.size()) {
                            ok = false;
                            break;
                        }
                        in->setInterfaceName(iface[size_t(output)]->getName());
                    } else if (size_t(source) < nodes.size()) {
                        in->setNodeName(names[size_t(source)]);
                        if (defs[size_t(source)]->getActiveOutputs().size() > 1)
                            in->setOutputString(outputName(defs[size_t(source)], size_t(output)));
                    } else {
                        ok = false;
                        break;
                    }
                }
            if (node_.size() > 3 && node_[3].IsArray())
                for (JsValue const &v : node_[3].GetJsArray()) {
                    JsArray const &value = v.GetJsArray();
                    if (value.size() < 2 || !value[0].IsInt() || size_t(value[0].GetInt()) >= inputs.size())
                        continue;
                    mx::InputPtr const &decl = inputs[size_t(value[0].GetInt())];
                    mx::InputPtr in = node->getInput(decl->getName());
                    if (!in)
                        in = node->addInput(decl->getName(), decl->getType());
                    in->setValueString(ValueText(value[1]));
                }
        }
        // Graph outputs, in the signature's order.
        const std::vector<mx::OutputPtr> ndOuts = nd->getActiveOutputs();
        JsArray const &outs = graph[3].GetJsArray();
        for (size_t k = 0; k < outs.size() && k < ndOuts.size() && ok; ++k) {
            JsArray const &o = outs[k].GetJsArray();
            if (o.size() < 3 || !o[1].IsInt() || size_t(o[1].GetInt()) >= nodes.size()) {
                ok = false;
                break;
            }
            mx::OutputPtr out = ng->addOutput(ndOuts[k]->getName(), ndOuts[k]->getType());
            const size_t source = size_t(o[1].GetInt());
            out->setNodeName(names[source]);
            if (defs[source]->getActiveOutputs().size() > 1)
                out->setOutputString(outputName(defs[source], size_t(o[2].GetInt())));
        }
        if (!ok) {
            doc->removeNodeGraph(ngName);
            continue;
        }
        nd->setAttribute("nova_graph", ngName);
        ++count;
    }
    return count;
}

} // namespace

HdNovaNodeDefDatabase const &HdNovaNodeDefDatabase::Get() {
    static std::once_flag once;
    static HdNovaNodeDefDatabase *database = nullptr; // intentionally leaked: used until process exit
    std::call_once(once, [] { database = new HdNovaNodeDefDatabase(); });
    return *database;
}

HdNovaNodeDefDatabase::HdNovaNodeDefDatabase() : _library(mx::createDocument()) {
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::string> candidates = EnvPathList("NOVA_MTLX_SEARCH_PATHS");
    const size_t novaCount = candidates.size();
    for (std::string const &p : EnvPathList("PXR_MTLX_STDLIB_SEARCH_PATHS"))
        candidates.push_back(p);
    if (std::string next = LibrariesNextToMaterialXDll(); !next.empty())
        candidates.push_back(next);
    for (std::string const &dir : candidates)
        if (IsStdlibRoot(dir)) {
            _stdlibRoot = dir;
            break;
        }
    for (size_t i = 0; i < novaCount; ++i)
        if (candidates[i] != _stdlibRoot && !IsStdlibRoot(candidates[i]))
            _userLibraries.push_back(candidates[i]);
    for (std::string const &p : EnvPathList("PXR_MTLX_PLUGIN_SEARCH_PATHS"))
        _userLibraries.push_back(p);

    if (_stdlibRoot.empty()) {
        HD_NOVA_WARN("no MaterialX standard library found (searched NOVA_MTLX_SEARCH_PATHS, "
                     "PXR_MTLX_STDLIB_SEARCH_PATHS and the MaterialX install); MaterialX materials will not render");
    }

    try {
        if (!_stdlibRoot.empty())
            _fileCount += mx::loadLibraries(kStdlibFolders, mx::FileSearchPath(_stdlibRoot), _library).size();
        for (std::string const &dir : _userLibraries)
            _fileCount += mx::loadLibraries({}, mx::FileSearchPath(dir), _library).size();
    } catch (std::exception const &e) {
        HD_NOVA_WARN("cannot load the MaterialX libraries: %s", e.what());
    }
    // <houdini>/karmaShaderNodes.json: the standard library root is
    // <houdini>/materialx/libraries.
    if (!_stdlibRoot.empty()) {
        const std::filesystem::path registry =
            std::filesystem::path(_stdlibRoot).parent_path().parent_path() / "karmaShaderNodes.json";
        try {
            _karmaNodes = LoadKarmaNodes(_library, registry.generic_string());
            _karmaGraphs = LoadKarmaGraphs(
                _library, (registry.parent_path() / "karmaNodeGraphs.json").generic_string());
        } catch (std::exception const &e) {
            HD_NOVA_WARN("cannot add Karma's shader nodes: %s", e.what());
        }
    }

    _loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    TF_DEBUG(HDNOVA_MATERIAL).Msg("Nova: %s\n", Describe().c_str());
}

std::string HdNovaNodeDefDatabase::Describe() const {
    return TfStringPrintf("MaterialX %s libraries from '%s'%s: %zu files, %zu nodedefs (%zu Karma nodes), "
                          "%zu nodegraphs (%.1f ms)",
                          mx::getVersionString().c_str(), _stdlibRoot.c_str(),
                          _userLibraries.empty()
                              ? ""
                              : (" + '" + TfStringJoin(_userLibraries, "', '") + "'").c_str(),
                          _fileCount, _library->getNodeDefs().size(), _karmaNodes, _library->getNodeGraphs().size(),
                          _loadMs);
}

PXR_NAMESPACE_CLOSE_SCOPE
