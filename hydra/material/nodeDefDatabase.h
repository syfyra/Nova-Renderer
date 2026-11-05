#pragma once

// The MaterialX data libraries (node definitions and the nodegraphs that
// implement them), loaded once per process from Houdini's MaterialX install.
// Material translation looks up node definitions here and flattens nodegraph
// implementations against it (docs/design/M3.md, K4).
//
// Library location, first match wins:
//   1. NOVA_MTLX_SEARCH_PATHS       (Nova override, path list)
//   2. PXR_MTLX_STDLIB_SEARCH_PATHS (set by Houdini and husk at run time)
//   3. <dir of MaterialXCore.dll>/../houdini/materialx/libraries (nova_cli and
//      tests, where nothing sets the variables)
// A location counts if it contains a "stdlib" folder; only Houdini's library
// folders below it are loaded. Every directory of PXR_MTLX_PLUGIN_SEARCH_PATHS
// (user libraries, as in usdMtlx) and every NOVA_MTLX_SEARCH_PATHS entry
// without a "stdlib" folder is loaded completely afterwards. For definitions
// with the same name the first one loaded wins.
//
// Karma's own shading nodes (kma_*, docs/research/m4/karma-nodes.md) have no
// MaterialX definitions: Karma describes them in <houdini>/karmaShaderNodes.json
// (inputs, types, defaults), next to the libraries. Nova adds a nodedef for
// each (named and categorized by the shader id, e.g. kma_rayswitch_float), so
// the translator lowers them like any node: natively where Nova implements
// them, else their default outputs with one warning. The kma nodes Karma
// builds from other nodes (<houdini>/karmaNodeGraphs.json: triplanar hex
// tiling, tangentrotate, melanin, deriv_2d) become nodegraph implementations
// of those nodedefs, with the graphs' default geometry properties.

#include <MaterialXCore/Document.h>

#include <pxr/pxr.h>

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaNodeDefDatabase {
public:
    // Loads the libraries on first use; thread-safe. Problems are reported
    // once as warnings, and the database is then empty (no node definitions).
    static HdNovaNodeDefDatabase const &Get();

    // The loaded definitions. Never null; immutable after loading.
    MaterialX::ConstDocumentPtr GetLibrary() const { return _library; }

    // The standard library location used ("" if none was found), the user
    // library directories, and the number of files loaded.
    std::string const &GetStdlibRoot() const { return _stdlibRoot; }
    std::vector<std::string> const &GetUserLibraries() const { return _userLibraries; }
    size_t GetFileCount() const { return _fileCount; }
    size_t GetKarmaNodeCount() const { return _karmaNodes; }
    size_t GetKarmaGraphCount() const { return _karmaGraphs; }
    double GetLoadMilliseconds() const { return _loadMs; }

    // One line for logs: version, location, file and definition counts.
    std::string Describe() const;

private:
    HdNovaNodeDefDatabase();

    MaterialX::DocumentPtr _library;
    std::string _stdlibRoot;
    std::vector<std::string> _userLibraries;
    size_t _fileCount = 0;
    size_t _karmaNodes = 0;
    size_t _karmaGraphs = 0;
    double _loadMs = 0.0;
};

PXR_NAMESPACE_CLOSE_SCOPE
