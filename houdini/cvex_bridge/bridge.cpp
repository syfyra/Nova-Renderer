// The CVEX bridge (docs/design/M5.md D4-D8, 3.2): compiles VEX surface and
// displacement code through transform.h with VCC, and runs it per batch on
// per-thread CVEX contexts owned by the shader. Built as nova_cvex.dll
// against the HDK; hdNova loads it on demand (core/shading/external.h).

#include "transform.h"

#include "core/shading/external.h"

#include <CVEX/CVEX_Context.h>
#include <CVEX/CVEX_Function.h>
#include <CVEX/CVEX_Value.h>
#include <UT/UT_ErrorManager.h>
#include <UT/UT_StringArray.h>
#include <UT/UT_WorkBuffer.h>
#include <VCC/VCC_Utils.h>
#include <VEX/VEX_VexResolver.h>

#include <tbb/enumerable_thread_specific.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

#include <windows.h> // WIN32_LEAN_AND_MEAN comes with the HDK's defines

namespace {

using Context = CVEX_ContextT<VEX_32>;
using RunData = CVEX_RunDataT<VEX_32>;
using Value = CVEX_ValueT<VEX_32>;

constexpr int kLanes = nova::ExternalShader::maxLanes();
constexpr float kTwoPi = 6.28318530717958647692f;

// VCC compiles and CVEX loads are not documented as thread-safe.
std::mutex &loadMutex() {
    static std::mutex m;
    return m;
}

// $HFS: the environment, else the directory above libCVEX.dll.
std::string houdiniRoot() {
    if (const char *hfs = std::getenv("HFS"); hfs && *hfs)
        return hfs;
    HMODULE module = GetModuleHandleW(L"libCVEX.dll");
    wchar_t path[MAX_PATH] = {};
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH))
        return {};
    return std::filesystem::path(path).parent_path().parent_path().string(); // $HFS/bin/libCVEX.dll
}

std::string readText(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// VEX's built-in BSDFs beyond diffuse (ggx(), ...) run CVEX shaders that
// they name as operator library paths, oplib:/Shop/ggx_sample?Shop/ggx_sample.
// Only a process with Houdini's operator director resolves those; outside one
// (nova_cli, tests) PBR finds no shader and crashes. Houdini ships the same
// shaders compiled in $HFS/houdini/vex/CVex, which this resolver serves.
class BuiltinBsdfResolver final : public VEX_VexResolver {
public:
    explicit BuiltinBsdfResolver(std::string dir) : _dir(std::move(dir)) {}

    // Present while Houdini's operators are, which resolve the paths themselves.
    static bool houdiniResolves() {
        HMODULE op = GetModuleHandleW(L"libOP.dll");
        using GetDirector = void *(*)();
        auto get = op ? reinterpret_cast<GetDirector>(GetProcAddress(op, "?OPgetDirector@@YAPEAVOP_Director@@XZ"))
                      : nullptr;
        return get && get();
    }

protected:
    bool canResolveCode(const char *shader) const override { return !file(shader).empty(); }
    bool supportedByMantra(const char *) const override { return true; }
    bool resolveVflCode(const char *, std::ostream &, VEX_CodeGenFlags, int) const override { return false; }
    bool resolveVexCode(const char *shader, std::ostream &os, VEX_CodeGenFlags, int, bool &pure,
                        VEX_ErrorLog &) const override {
        std::string path = file(shader);
        if (path.empty())
            return false;
        os << readText(path);
        pure = true;
        return true;
    }
    bool resolveCacheVexCode(const char *, int, VEX_ErrorLog &) const override { return false; }
    bool resolveDependencies(const char *, UT_StringArray &, int) const override { return false; }
    bool resolveAddDependency(const char *, DEP_MicroNode *) const override { return false; }
    time_t resolveCodeTimeStamp(const char *shader, int) const override { return file(shader).empty() ? -1 : 0; }
    bool resolveVexContext(const char *shader, UT_String &ctx, int) const override {
        if (file(shader).empty())
            return false;
        ctx.harden("cvex");
        return true;
    }
    bool resolveCanonicalPath(const char *, UT_String &) const override { return false; }
    bool resolveAlias(const char *, UT_String &) const override { return false; }

private:
    // "oplib:/Shop/<name>?Shop/<name>" -> <dir>/<name>.vex when it exists.
    std::string file(const char *shader) const {
        constexpr std::string_view prefix = "oplib:/Shop/";
        std::string_view s = shader ? shader : "";
        if (!s.starts_with(prefix))
            return {};
        std::string_view name = s.substr(prefix.size(), s.find('?') - prefix.size());
        if (name.empty() || s.substr(prefix.size() + name.size()) != "?Shop/" + std::string(name))
            return {};
        std::string path = _dir + "/" + std::string(name) + ".vex";
        return std::filesystem::is_regular_file(path) ? path : std::string();
    }

    std::string _dir;
};

int widthOf(CVEX_Type t) {
    switch (t) {
    case CVEX_TYPE_INTEGER:
    case CVEX_TYPE_FLOAT:
        return 1;
    case CVEX_TYPE_VECTOR2:
        return 2;
    case CVEX_TYPE_VECTOR3:
        return 3;
    case CVEX_TYPE_VECTOR4:
    case CVEX_TYPE_MATRIX2:
        return 4;
    case CVEX_TYPE_MATRIX3:
        return 9;
    case CVEX_TYPE_MATRIX4:
        return 16;
    default:
        return 0;
    }
}

// Binds `n` floats per lane of `data` to `value` with its own type.
void bind(Value *value, float *data, int lanes) {
    switch (value->getType()) {
    case CVEX_TYPE_FLOAT:
        value->setTypedData(data, lanes);
        break;
    case CVEX_TYPE_VECTOR2:
        value->setTypedData(reinterpret_cast<VEXvec2<VEX_32> *>(data), lanes);
        break;
    case CVEX_TYPE_VECTOR3:
        value->setTypedData(reinterpret_cast<VEXvec3<VEX_32> *>(data), lanes);
        break;
    case CVEX_TYPE_VECTOR4:
        value->setTypedData(reinterpret_cast<VEXvec4<VEX_32> *>(data), lanes);
        break;
    case CVEX_TYPE_MATRIX2:
        value->setTypedData(reinterpret_cast<VEXmat2<VEX_32> *>(data), lanes);
        break;
    case CVEX_TYPE_MATRIX3:
        value->setTypedData(reinterpret_cast<VEXmat3<VEX_32> *>(data), lanes);
        break;
    case CVEX_TYPE_MATRIX4:
        value->setTypedData(reinterpret_cast<VEXmat4<VEX_32> *>(data), lanes);
        break;
    default:
        break;
    }
}

struct VaryingInput {
    const char *name;
    CVEX_Type type;
};
// The globals and Nova's inputs, in the order of the lane buffers.
constexpr VaryingInput kLaneInputs[] = {
    {"P", CVEX_TYPE_VECTOR3},    {"I", CVEX_TYPE_VECTOR3},    {"N", CVEX_TYPE_VECTOR3},
    {"Ng", CVEX_TYPE_VECTOR3},   {"dPds", CVEX_TYPE_VECTOR3}, {"dPdt", CVEX_TYPE_VECTOR3},
    {"s", CVEX_TYPE_FLOAT},      {"t", CVEX_TYPE_FLOAT},      {"Time", CVEX_TYPE_FLOAT},
    {nova::cvex::kLightDir, CVEX_TYPE_VECTOR3}, {nova::cvex::kSampleX, CVEX_TYPE_FLOAT},
    {nova::cvex::kSampleY, CVEX_TYPE_FLOAT},
};
constexpr int kLaneInputCount = int(std::size(kLaneInputs));

struct Output {
    const char *name;
    CVEX_Type type;
};
constexpr Output kSurfaceOutputs[] = {
    {"nova_light", CVEX_TYPE_VECTOR3}, {"nova_light_pdf", CVEX_TYPE_FLOAT}, {"nova_dir", CVEX_TYPE_VECTOR3},
    {"nova_weight", CVEX_TYPE_VECTOR3}, {"nova_pdf", CVEX_TYPE_FLOAT},       {"nova_bounce", CVEX_TYPE_INTEGER},
    {"nova_albedo", CVEX_TYPE_VECTOR3}, {"nova_opacity", CVEX_TYPE_VECTOR3}, {"nova_emission", CVEX_TYPE_VECTOR3},
};
enum SurfaceOutput { oLight, oLightPdf, oDir, oWeight, oPdf, oBounce, oAlbedo, oOpacity, oEmission };
constexpr Output kDisplaceOutputs[] = {{"P", CVEX_TYPE_VECTOR3}};

// CVEX's run under a structured-exception guard: an access violation inside
// Houdini's VEX/PBR code fails the run (-1) instead of the host process.
int guardedRun(Context &ctx, int count, RunData *rd) {
    __try {
        return ctx.run(count, false, rd) ? 1 : 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
}

// Set once a run faulted inside VEX's sample_bsdf (Shader::run): surfaces
// compile with the sampling-free tail from then on.
std::atomic<bool> g_safeSampling{false};

// A compiled function with the parameters Nova binds.
struct Compiled {
    CVEX_Function fn;
    std::vector<nova::ExternalParam> params;
    std::vector<CVEX_Type> types;
    bool writesOpacity = true;
};

// A compiled function run on a context per thread: the lanes' globals and
// Nova's inputs, the parameters, and the given outputs.
class Runner {
public:
    Runner(Compiled c, std::vector<Output> outputs)
        : _fn(std::move(c.fn)), _params(std::move(c.params)), _types(std::move(c.types)),
          _outputs(std::move(outputs)) {}

    // The contexts go with the shader (a render thread outlives every shader
    // it ran: each VEX code edit in an IPR would leave one per thread). No
    // run is in flight here: the last reference to the shader is gone.
    ~Runner() {
        std::lock_guard lock(loadMutex()); // (as their loads)
        _bound.clear();
    }

    const std::vector<nova::ExternalParam> &params() const { return _params; }

    struct Bound;

    // Runs 1..kLanes lanes: the context with its outputs, or null with the
    // error (reported once). `faulted`: the run raised an access violation
    // (not reported; its context is dropped).
    const Bound *run(int count, const nova::ExternalLane *lanes, const nova::ExternalInput *inputs,
                     const std::string *strings, std::string *error, bool *faulted = nullptr) const {
        if (count > kLanes) {
            if (error)
                *error = "too many lanes";
            return nullptr;
        }
        Bound &b = bound();
        if (!b.ok) {
            fail(b.error, error);
            return nullptr;
        }
        // Globals and Nova's inputs.
        for (int l = 0; l < count; ++l) {
            const nova::ExternalLane &x = lanes[l];
            const nova::Vec3f *vecs[] = {&x.P, &x.I, &x.N, &x.Ng, &x.dPds, &x.dPdt};
            for (int k = 0; k < 6; ++k) {
                float *d = b.lane[size_t(k)].data() + size_t(l) * 3;
                d[0] = vecs[k]->x;
                d[1] = vecs[k]->y;
                d[2] = vecs[k]->z;
            }
            b.lane[6][size_t(l)] = x.s;
            b.lane[7][size_t(l)] = x.t;
            b.lane[8][size_t(l)] = x.time;
            float *wi = b.lane[9].data() + size_t(l) * 3;
            wi[0] = x.wi.x;
            wi[1] = x.wi.y;
            wi[2] = x.wi.z;
            b.lane[10][size_t(l)] = x.sx;
            b.lane[11][size_t(l)] = x.sy;
        }
        // Parameters: every float parameter is varying (a uniform value fills
        // every lane), so one load serves all materials of this code.
        for (size_t p = 0; p < _params.size(); ++p) {
            const nova::ExternalParam &param = _params[p];
            if (param.width == 0) {
                // An empty string is a value (an authored "" clears a map
                // path, as in Karma); only null strings mean the defaults.
                if (b.strings[p])
                    (*b.strings[p])[0] = UT_StringHolder(strings ? strings[p] : param.stringDefault);
                continue;
            }
            std::vector<float> &buf = b.params[p];
            const int w = param.width;
            const float *src = inputs && inputs[p].data ? inputs[p].data : param.defaults.data();
            const bool varying = inputs && inputs[p].data && inputs[p].varying;
            if (param.isInt) {
                auto *ints = reinterpret_cast<int32_t *>(buf.data());
                for (int l = 0; l < count; ++l)
                    ints[l] = int32_t(std::lround(src[varying ? l : 0]));
            } else {
                for (int l = 0; l < count; ++l)
                    for (int k = 0; k < w; ++k)
                        buf[size_t(l * w + k)] = src[(varying ? l * w : 0) + k];
            }
        }
        const int ran = guardedRun(b.ctx, count, &b.rd);
        if (ran < 0) {
            {
                std::lock_guard lock(loadMutex());
                _bound.local().reset(); // in an unknown state (`b` dangles from here)
            }
            if (faulted)
                *faulted = true;
            else
                fail("an access violation inside Houdini's VEX code; the shader renders black", error);
            if (error)
                *error = "an access violation inside Houdini's VEX code";
            return nullptr;
        }
        if (ran == 0) {
            fail(std::string("CVEX run failed: ") + (b.ctx.getLastError() ? b.ctx.getLastError() : "") +
                     (b.ctx.getVexErrors().c_str() ? b.ctx.getVexErrors().c_str() : ""),
                 error);
            return nullptr;
        }
        return &b;
    }

    // Reports `message` once per shader (see fail()).
    void report(const std::string &message) const { fail(message, nullptr); }

    // A loaded context with its bound buffers, one per thread and shader.
    struct Bound {
        Context ctx;
        RunData rd;
        std::vector<float> lane[kLaneInputCount];
        std::vector<std::vector<float>> params;
        std::vector<std::unique_ptr<CVEX_StringArray>> strings;
        std::vector<std::vector<float>> out; // per output
        bool ok = false;
        std::string error;
    };

private:
    // This thread's context, loaded on its first run.
    Bound &bound() const {
        std::unique_ptr<Bound> &slot = _bound.local();
        if (!slot)
            slot = load();
        return *slot;
    }

    std::unique_ptr<Bound> load() const {
        auto b = std::make_unique<Bound>();
        std::lock_guard lock(loadMutex());
        for (int k = 0; k < kLaneInputCount; ++k) {
            b->lane[k].assign(size_t(kLanes) * size_t(widthOf(kLaneInputs[k].type)), 0.0f);
            b->ctx.addInput(kLaneInputs[k].name, kLaneInputs[k].type, true);
        }
        b->params.resize(_params.size());
        b->strings.resize(_params.size());
        for (size_t p = 0; p < _params.size(); ++p) {
            if (_params[p].width == 0) {
                b->strings[p] = std::make_unique<CVEX_StringArray>();
                b->strings[p]->append(UT_StringHolder(_params[p].stringDefault));
                b->ctx.addInput(_params[p].name, *b->strings[p]);
            } else {
                b->params[p].assign(size_t(kLanes) * size_t(_params[p].width), 0.0f);
                b->ctx.addInput(_params[p].name, _types[p], true);
            }
        }
        b->out.resize(_outputs.size());
        for (size_t o = 0; o < _outputs.size(); ++o) {
            b->out[o].assign(size_t(kLanes) * size_t(widthOf(_outputs[o].type)), 0.0f);
            b->ctx.addRequiredOutput(_outputs[o].name, _outputs[o].type);
        }
        const char *argv[] = {"nova"};
        if (!b->ctx.loadFunction(_fn, 1, argv)) {
            b->error = std::string("CVEX load failed: ") + (b->ctx.getLastError() ? b->ctx.getLastError() : "");
            return b;
        }
        for (int k = 0; k < kLaneInputCount; ++k)
            if (Value *v = b->ctx.findInput(kLaneInputs[k].name, kLaneInputs[k].type))
                bind(v, b->lane[k].data(), kLanes);
        for (size_t p = 0; p < _params.size(); ++p) {
            if (_params[p].width == 0)
                continue;
            if (Value *v = b->ctx.findInput(_params[p].name, _types[p])) {
                if (_params[p].isInt)
                    v->setTypedData(reinterpret_cast<VEXint<VEX_32> *>(b->params[p].data()), kLanes);
                else
                    bind(v, b->params[p].data(), kLanes);
            }
        }
        for (size_t o = 0; o < _outputs.size(); ++o) {
            Value *v = b->ctx.findOutput(_outputs[o].name, _outputs[o].type);
            if (!v) {
                b->error = std::string("the CVEX function has no output ") + _outputs[o].name;
                return b;
            }
            if (_outputs[o].type == CVEX_TYPE_INTEGER)
                v->setTypedData(reinterpret_cast<VEXint<VEX_32> *>(b->out[o].data()), kLanes);
            else
                bind(v, b->out[o].data(), kLanes);
        }
        b->ok = true;
        return b;
    }

    // Run errors: to the caller, and once per shader to stderr (the render
    // loop has no channel for them; compile errors reach the host earlier).
    void fail(const std::string &message, std::string *error) const {
        if (error)
            *error = message;
        std::call_once(_reported, [&] { std::fprintf(stderr, "[Nova] Warning: VEX shader: %s\n", message.c_str()); });
    }

    CVEX_Function _fn;
    mutable std::once_flag _reported;
    std::vector<nova::ExternalParam> _params;
    std::vector<CVEX_Type> _types;
    std::vector<Output> _outputs;
    mutable tbb::enumerable_thread_specific<std::unique_ptr<Bound>> _bound; // per thread
};

class Shader final : public nova::ExternalShader {
public:
    using SafeCompiler = std::function<std::optional<Compiled>()>;

    // `safe`: compiled with the sampling-free tail; `compileSafe` compiles
    // that variant on demand.
    Shader(Compiled c, bool safe, SafeCompiler compileSafe)
        : _writesOpacity(c.writesOpacity), _runner(std::move(c), {std::begin(kSurfaceOutputs), std::end(kSurfaceOutputs)}),
          _safe(safe), _compileSafe(std::move(compileSafe)) {}

    const std::vector<nova::ExternalParam> &params() const override { return _runner.params(); }

    bool writesOpacity() const override { return _writesOpacity; }

    bool run(int count, const nova::ExternalLane *lanes, const nova::ExternalInput *inputs,
             const std::string *strings, nova::ExternalResult *out, std::string *error) const override {
        if (count <= 0)
            return true;
        const Runner *runner = active();
        bool faulted = false;
        const Runner::Bound *b = runner->run(count, lanes, inputs, strings, error, runner == &_runner ? &faulted : nullptr);
        if (!b && faulted && !_safe) {
            // VEX's sample_bsdf faults on Houdini's native GGX lobes in some
            // Houdini processes (docs/design/M5.md, notes on M5.4): from now
            // on the sampling-free variant, for this batch too.
            g_safeSampling = true;
            if (const Runner *safe = safeRunner()) {
                runner = safe;
                b = safe->run(count, lanes, inputs, strings, error);
            }
        }
        // The sampling-free variant itself faulted: nothing else reports it.
        if (!b && faulted && _safe)
            _runner.report("an access violation inside Houdini's VEX code; the shader renders black");
        if (!b)
            return false;
        // Results (docs/design/M5.md D6): VEX's eval is 2 pi f cos; its pdf
        // is 2 pi times the albedo-weighted sum of the components' pdfs.
        auto vec = [&](int o, int l) {
            const float *d = b->out[size_t(o)].data() + size_t(l) * 3;
            return nova::Vec3f{d[0], d[1], d[2]};
        };
        auto spec = [&](int o, int l) {
            const nova::Vec3f v = vec(o, l);
            return nova::Spectrum{v.x, v.y, v.z};
        };
        for (int l = 0; l < count; ++l) {
            nova::ExternalResult &r = out[l];
            r.albedo = spec(oAlbedo, l);
            // VEX weighs the components by luminance() of their albedo (NTSC).
            const float a = std::fmax(0.299f * r.albedo.x + 0.587f * r.albedo.y + 0.114f * r.albedo.z, 0.0f);
            const float pdfScale = a > 0.0f ? 1.0f / (kTwoPi * a) : 0.0f;
            r.lightF = spec(oLight, l) * (1.0f / kTwoPi);
            r.lightPdf = b->out[oLightPdf][size_t(l)] * pdfScale;
            r.bounce = uint32_t(reinterpret_cast<const int32_t *>(b->out[oBounce].data())[l]);
            r.dir = r.bounce ? vec(oDir, l) : nova::Vec3f{0.0f, 0.0f, 0.0f};
            r.weight = r.bounce ? spec(oWeight, l) : nova::Spectrum{0.0f, 0.0f, 0.0f};
            r.pdf = b->out[oPdf][size_t(l)] * pdfScale;
            r.opacity = spec(oOpacity, l);
            r.emission = spec(oEmission, l);
        }
        return true;
    }

private:
    const Runner *active() const {
        if (!_safe && g_safeSampling)
            if (const Runner *safe = safeRunner())
                return safe;
        return &_runner;
    }

    // The sampling-free variant, compiled once (null if it does not compile).
    const Runner *safeRunner() const {
        std::call_once(_safeOnce, [this] {
            std::optional<Compiled> c = _compileSafe ? _compileSafe() : std::nullopt;
            if (c) {
                _safeVariant = std::make_unique<Runner>(std::move(*c), std::vector<Output>(std::begin(kSurfaceOutputs),
                                                                                          std::end(kSurfaceOutputs)));
                // Printed here (once per shader by _safeOnce), not through the
                // variant's report(): that one stays free for its own failures.
                std::fprintf(stderr, "[Nova] Warning: VEX shader: Houdini's VEX faulted in sample_bsdf (native GGX "
                                     "lobes); VEX materials sample a cosine/mirror mixture from now on: glossy lobes "
                                     "are noisier, perfect mirrors and refraction are not sampled\n");
            } else {
                _runner.report("Houdini's VEX faulted in sample_bsdf and the fallback does not compile; the shader "
                               "renders black");
            }
        });
        return _safeVariant.get();
    }

    bool _writesOpacity;
    Runner _runner;
    bool _safe;
    SafeCompiler _compileSafe;
    mutable std::once_flag _safeOnce;
    mutable std::unique_ptr<Runner> _safeVariant;
};

class Displacer final : public nova::ExternalDisplacer {
public:
    explicit Displacer(Compiled c) : _runner(std::move(c), {std::begin(kDisplaceOutputs), std::end(kDisplaceOutputs)}) {}

    const std::vector<nova::ExternalParam> &params() const override { return _runner.params(); }

    bool run(int count, const nova::ExternalLane *lanes, const nova::ExternalInput *inputs,
             const std::string *strings, nova::Vec3f *P, std::string *error) const override {
        if (count <= 0)
            return true;
        const Runner::Bound *b = _runner.run(count, lanes, inputs, strings, error);
        if (!b)
            return false; // (a fault is reported once)
        for (int l = 0; l < count; ++l) {
            const float *d = b->out[0].data() + size_t(l) * 3;
            P[l] = nova::Vec3f{d[0], d[1], d[2]};
        }
        return true;
    }

private:
    Runner _runner;
};

class Bridge final : public nova::ExternalBridge {
public:
    std::shared_ptr<nova::ExternalShader> compileSurface(const std::string &source, std::string *error) override {
        std::lock_guard lock(_cacheMutex);
        if (auto it = _surfaces.find(source); it != _surfaces.end())
            if (std::shared_ptr<Shader> s = it->second.lock())
                return s;
        // Released shaders (keyed by their whole source) go from the cache.
        std::erase_if(_surfaces, [](const auto &e) { return e.second.expired(); });
        const bool safe = g_safeSampling;
        std::optional<Compiled> c = compile(source, surfaceTransform(safe), error);
        if (!c)
            return nullptr;
        Shader::SafeCompiler compileSafe;
        if (!safe)
            compileSafe = [this, source] {
                std::lock_guard relock(_cacheMutex);
                return compile(source, surfaceTransform(true), nullptr);
            };
        auto s = std::make_shared<Shader>(std::move(*c), safe, std::move(compileSafe));
        _surfaces[source] = s;
        return s;
    }

    std::shared_ptr<nova::ExternalDisplacer> compileDisplacement(const std::string &source,
                                                                 std::string *error) override {
        std::lock_guard lock(_cacheMutex);
        if (auto it = _displacers.find(source); it != _displacers.end())
            if (std::shared_ptr<Displacer> d = it->second.lock())
                return d;
        std::erase_if(_displacers, [](const auto &e) { return e.second.expired(); });
        std::optional<Compiled> c = compile(source, nova::cvex::transformDisplacement, error);
        if (!c)
            return nullptr;
        auto d = std::make_shared<Displacer>(std::move(*c));
        _displacers[source] = d;
        return d;
    }

    nova::ExternalBounces bounces() const override { return _bounces; }

    Bridge() {
        // NOVA_CVEX_SAFE_SAMPLING=1 starts with the sampling-free tail (tests,
        // and a way out should sample_bsdf fault in another way).
        if (const char *safe = std::getenv("NOVA_CVEX_SAFE_SAMPLING"); safe && *safe == '1')
            g_safeSampling = true;
        if (!BuiltinBsdfResolver::houdiniResolves()) {
            static BuiltinBsdfResolver *resolver = new BuiltinBsdfResolver(houdiniRoot() + "/houdini/vex/CVex");
            (void)resolver; // registered by its constructor, kept for the process
        }
        _bounces = queryBounces();
    }

private:
    using Transform = std::function<nova::cvex::Transformed(const std::string &, const std::string &,
                                                            const std::vector<std::string> &)>;

    static Transform surfaceTransform(bool safe) {
        return [safe](const std::string &source, const std::string &layer, const std::vector<std::string> &standIns) {
            return nova::cvex::transformSurface(source, layer, standIns, safe);
        };
    }

    std::optional<Compiled> compile(const std::string &source, Transform transform, std::string *error) {
        if (_shaderLayer.empty() && !_shaderLayerRead) {
            _shaderLayerRead = true;
            const std::string root = houdiniRoot();
            if (!root.empty())
                _shaderLayer = readText(root + "/houdini/vex/include/shaderlayer.h");
        }
        std::vector<std::string> standIns;
        std::string errors;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const nova::cvex::Transformed t = transform(source, _shaderLayer, standIns);
            if (!t.ok) {
                if (error)
                    *error = t.error;
                return std::nullopt;
            }
            std::lock_guard lock(loadMutex());
            Context proto;
            UT_WorkBuffer code;
            code.strcpy(t.source.c_str());
            // VCC reports compile errors through the thread's error manager.
            UT_ErrorManager::Scope scope;
            CVEX_Function fn = VCC_Utils::preloadLocalFunction<VEX_32>(proto, code);
            if (!fn.isValid()) {
                // (An empty UT_String has no buffer.)
                UT_String messages;
                scope.getErrorMessages(messages);
                const char *vexErrors = proto.getVexErrors().c_str();
                const char *last = proto.getLastError();
                errors = std::string(messages.c_str() ? messages.c_str() : "") + (vexErrors ? vexErrors : "") +
                         (last ? last : "");
                // Surface globals library code reads: defaults, then retry.
                bool added = false;
                const std::string marker = "Reference to undefined variable: ";
                for (size_t p = 0; (p = errors.find(marker, p)) != std::string::npos;) {
                    p += marker.size();
                    size_t e = p;
                    while (e < errors.size() && (std::isalnum(static_cast<unsigned char>(errors[e])) || errors[e] == '_'))
                        ++e;
                    const std::string name = errors.substr(p, e - p);
                    if (!nova::cvex::standInDefault(name).empty() &&
                        std::find(standIns.begin(), standIns.end(), name) == standIns.end()) {
                        standIns.push_back(name);
                        added = true;
                    }
                }
                if (added)
                    continue;
                break;
            }
            // The original shader's parameters CVEX can bind.
            UT_StringArray names;
            UT_Array<CVEX_Type> types;
            UT_IntArray exports;
            fn.getParameters(names, types, exports);
            const char *argv[] = {"nova"};
            proto.loadFunction(fn, 1, argv);
            std::vector<nova::ExternalParam> params;
            std::vector<CVEX_Type> ptypes;
            for (const nova::cvex::ParamDecl &decl : t.params) {
                if (decl.exported)
                    continue;
                exint k = names.find(UT_StringRef(decl.name.c_str()));
                if (k < 0)
                    continue;
                nova::ExternalParam p;
                p.name = decl.name;
                const CVEX_Type type = types(k);
                if (type == CVEX_TYPE_STRING) {
                    p.width = 0;
                    p.stringDefault = nova::cvex::unquote(decl.defaultText);
                } else {
                    p.width = widthOf(type);
                    if (p.width == 0)
                        continue; // arrays, dicts: unbound (the shader's default)
                    p.isInt = type == CVEX_TYPE_INTEGER;
                    UT_DoubleArray values;
                    proto.getParameterDefaults(UT_StringRef(decl.name.c_str()), type, values);
                    p.defaults.assign(size_t(p.width), 0.0f);
                    for (exint i = 0; i < values.size() && i < exint(p.width); ++i)
                        p.defaults[size_t(i)] = float(values(i));
                }
                params.push_back(std::move(p));
                ptypes.push_back(type);
            }
            return Compiled{fn, std::move(params), std::move(ptypes), t.writesOpacity};
        }
        if (error) {
            // The first few error lines.
            std::istringstream lines(errors);
            std::string line, text;
            for (int n = 0; n < 4 && std::getline(lines, line);)
                if (line.find("Error") != std::string::npos) {
                    text += (text.empty() ? "" : "; ") + line;
                    ++n;
                }
            *error = "VEX compile failed: " + (text.empty() ? errors.substr(0, 400) : text);
        }
        return std::nullopt;
    }

    // VEX's bounce bits, from bouncemask() of the pbr.h labels.
    static nova::ExternalBounces queryBounces() {
        nova::ExternalBounces b;
        static const char *kSource = R"vex(cvex nova_bounces(float dummy = 0;
    export int d = 0; export int r = 0; export int f = 0; export int v = 0; export int s = 0)
{
    d = bouncemask("diffuse") | bouncemask("alldiffuse");
    r = bouncemask("reflect") | bouncemask("allreflect") | bouncemask("coat");
    f = bouncemask("refract") | bouncemask("allrefract");
    v = bouncemask("volume") | bouncemask("allvolume");
    s = bouncemask("sss") | bouncemask("allsss");
}
)vex";
        std::lock_guard lock(loadMutex());
        Context ctx;
        UT_WorkBuffer code;
        code.strcpy(kSource);
        CVEX_Function fn = VCC_Utils::preloadLocalFunction<VEX_32>(ctx, code);
        if (!fn.isValid())
            return b;
        int32_t values[5] = {};
        const char *names[5] = {"d", "r", "f", "v", "s"};
        for (const char *n : names)
            ctx.addRequiredOutput(n, CVEX_TYPE_INTEGER);
        const char *argv[] = {"nova"};
        if (!ctx.loadFunction(fn, 1, argv))
            return b;
        for (int k = 0; k < 5; ++k)
            if (Value *v = ctx.findOutput(names[k], CVEX_TYPE_INTEGER))
                v->setTypedData(reinterpret_cast<VEXint<VEX_32> *>(&values[k]), 1);
        if (!ctx.run(1, false))
            return b;
        b.diffuse = uint32_t(values[0]);
        b.reflect = uint32_t(values[1]);
        b.refract = uint32_t(values[2]);
        b.volume = uint32_t(values[3]);
        b.sss = uint32_t(values[4]);
        return b;
    }

    std::recursive_mutex _cacheMutex; // (the fallback compiles while a render holds no lock)
    std::unordered_map<std::string, std::weak_ptr<Shader>> _surfaces;
    std::unordered_map<std::string, std::weak_ptr<Displacer>> _displacers;
    std::string _shaderLayer;
    bool _shaderLayerRead = false;
    nova::ExternalBounces _bounces;
};

} // namespace

extern "C" __declspec(dllexport) nova::ExternalBridge *novaCvexBridge() {
    static Bridge bridge;
    return &bridge;
}
