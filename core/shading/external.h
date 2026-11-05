#pragma once

// External shaders: VEX code Nova runs through the CVEX bridge
// (docs/design/M5.md D4-D7, houdini/cvex_bridge). The bridge compiles a VEX
// surface function into a CVEX function that also evaluates and samples the
// shader's BSDF inside VEX; Nova hands it batches of shading points and gets
// per-lane results. No Houdini types appear here: the bridge is a separate
// DLL that hdNova loads when a material needs it.

#include "core/math/spectrum.h"
#include "core/math/vec.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nova {

// A parameter of an external shader. Float-valued parameters (width > 0)
// are bound per lane; strings are uniform.
struct ExternalParam {
    std::string name;
    int width = 0;                    // floats per value: 1 float/int, 2, 3, 4, 9, 16; 0: string
    bool isInt = false;               // an int parameter (passed as rounded floats)
    std::vector<float> defaults;      // the shader's default (width floats)
    std::string stringDefault;
};

// One shading point, world space. I points from the eye to the surface;
// N and Ng keep their authored orientation (VEX's surface globals).
struct ExternalLane {
    Vec3f P, I, N, Ng, dPds, dPdt;
    float s = 0.0f, t = 0.0f, time = 0.0f;
    Vec3f wi;                         // the light direction NEE evaluates (zero: none)
    float sx = 0.5f, sy = 0.5f;       // the BSDF sample numbers
};

// Per lane (docs/design/M5.md D6): f cos and the MIS pdf at wi; the
// continuation (sample_bsdf's direction and throughput weight, the MIS pdf
// there); the sample's VEX bounce bits; albedo, opacity and emission.
struct ExternalResult {
    Spectrum lightF{0.0f, 0.0f, 0.0f};
    float lightPdf = 0.0f;
    Vec3f dir;                        // zero: no continuation
    Spectrum weight{0.0f, 0.0f, 0.0f};
    float pdf = 0.0f;
    uint32_t bounce = 0;
    Spectrum albedo{0.0f, 0.0f, 0.0f}, opacity{1.0f, 1.0f, 1.0f}, emission{0.0f, 0.0f, 0.0f};
};

// VEX's bounce bits (pbr.h bouncemask: diffuse, reflect, refract, volume, sss).
struct ExternalBounces {
    uint32_t diffuse = 0, reflect = 0, refract = 0, volume = 0, sss = 0;
};

// Values of one parameter for a run: `count` lanes of `width` floats
// (varying) or one value (uniform); null: the shader's default.
struct ExternalInput {
    const float *data = nullptr;
    bool varying = false;
};

class ExternalShader {
public:
    virtual ~ExternalShader() = default;
    virtual const std::vector<ExternalParam> &params() const = 0;
    // Runs `count` lanes (at most maxLanes()); `inputs` and `strings` follow
    // params() (strings only for string parameters; null `strings`: the
    // defaults, an empty string is passed as is). Thread-safe: each thread
    // uses its own CVEX context.
    virtual bool run(int count, const ExternalLane *lanes, const ExternalInput *inputs,
                     const std::string *strings, ExternalResult *out, std::string *error) const = 0;
    // False when the shader never sets its opacity (Of): opaque at no cost
    // (docs/design/M5.md D9).
    virtual bool writesOpacity() const = 0;
    static constexpr int maxLanes() { return 256; }
};

// A VEX displacement function (docs/design/M5.md D8). The lanes' P, N, I,
// Ng, dPds, dPdt, s, t and time are its globals (world space; wi, sx, sy
// unused); a run writes the displaced P of every lane.
class ExternalDisplacer {
public:
    virtual ~ExternalDisplacer() = default;
    virtual const std::vector<ExternalParam> &params() const = 0;
    // As ExternalShader::run.
    virtual bool run(int count, const ExternalLane *lanes, const ExternalInput *inputs,
                     const std::string *strings, Vec3f *P, std::string *error) const = 0;
    static constexpr int maxLanes() { return 256; }
};

class ExternalBridge {
public:
    virtual ~ExternalBridge() = default;
    // Compiles a VEX surface function's source (a VEX Material Builder's
    // sourceCode). Null with `error` when it cannot run in CVEX.
    virtual std::shared_ptr<ExternalShader> compileSurface(const std::string &source, std::string *error) = 0;
    virtual ExternalBounces bounces() const = 0;
    // As compileSurface, for a VEX displace function's source.
    virtual std::shared_ptr<ExternalDisplacer> compileDisplacement(const std::string &source, std::string *error) = 0;
};

// The bridge DLL's entry point (extern "C", resolved by name).
using ExternalBridgeFactory = ExternalBridge *(*)();
constexpr const char *kExternalBridgeEntry = "novaCvexBridge";

} // namespace nova
