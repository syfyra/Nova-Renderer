#pragma once

// VEX surface code -> a CVEX function Nova can run (docs/design/M5.md D4,
// D5; docs/research/m5/vex-materials.md 0.4). Text processing only, no
// Houdini dependency:
//   * the `surface` function becomes a `cvex` function; the surface globals
//     (P, I, N, Ng, s, t, dPds, dPdt, Time, Eye, Cf, Of, Af, Ce) become its
//     parameters unless the shader already has one of that name, and the
//     bsdf F a local;
//   * shading-only calls get stand-ins: getraylevel(), getglobalraylevel(),
//     isshadowray() and rayimport() return 0, a two-argument frontface()
//     flips n against i, pbrlighting() statements (AOV exports) go;
//   * library code before the shader reads dPds/dPdt (VOP tangent helpers),
//     isshadingRHS() and objectstate() through stand-ins; shaderlayer.h's
//     init_layer reads P and N, so a copy of the header with those two reads
//     replaced is inlined first (its include guard keeps the original out);
//   * the end of the function evaluates F with eval_bsdf/albedo/sample_bsdf
//     into exports named nova_* (see kOutputs).
// Displacement functions get the same treatment without the BSDF parts.

#include <string>
#include <vector>

namespace nova::cvex {

// A parameter of the original shader: its name and the text of its
// default ("{0, 0, 0}", "\"label\"", empty when it has none).
struct ParamDecl {
    std::string name, type, defaultText;
    bool exported = false;
};

struct Transformed {
    bool ok = false;
    std::string source;              // the CVEX function's VFL
    std::string function;            // the shader's name
    std::string cvexFunction;        // the CVEX function's: the name and a hash of the code
    std::vector<ParamDecl> params;   // the original shader's parameters
    bool writesOpacity = false;      // the body names Of (surfaces)
    std::string error;
};

// Inputs Nova adds (varying): the light direction and the BSDF sample numbers.
inline constexpr const char *kLightDir = "nova_wi";
inline constexpr const char *kSampleX = "nova_sx";
inline constexpr const char *kSampleY = "nova_sy";

// `source`: the VEX code of a surface function (a VEX Material Builder's
// sourceCode); `shaderLayerHeader`: the text of Houdini's
// vex/include/shaderlayer.h (empty: not inlined).
// `standIns`: more surface globals library functions read ("Ng", "s", ...),
// defined as their defaults before the shader (the bridge adds the names
// vcc reports undefined there).
// `safeSampling`: the continuation without sample_bsdf (see kSafeTail).
Transformed transformSurface(const std::string &source, const std::string &shaderLayerHeader,
                             const std::vector<std::string> &standIns = {}, bool safeSampling = false);

// The same for a `displace` function (docs/design/M5.md D8): its globals
// become parameters, P and N exported; nothing is appended.
Transformed transformDisplacement(const std::string &source, const std::string &shaderLayerHeader,
                                  const std::vector<std::string> &standIns = {});

// The default a stand-in gets (empty: not a surface global).
std::string standInDefault(const std::string &name);

// The parameters of a VEX parameter list ("vector a = 0; export float b, c
// = 1; float d[]"), at the top level.
std::vector<ParamDecl> parameters(const std::string &list);

// A VEX string literal's value ("\"a\\\"b\"" -> a"b); the text itself if it is none.
std::string unquote(const std::string &literal);

} // namespace nova::cvex
