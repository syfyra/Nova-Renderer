#include "transform.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iterator>

namespace nova::cvex {

namespace {

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The index past the bracket that closes the one at `open`, skipping string
// literals; npos when unbalanced.
size_t matchClose(const std::string &s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            for (++i; i < s.size() && s[i] != '"'; ++i)
                if (s[i] == '\\')
                    ++i;
            continue;
        }
        if (c == '(' || c == '{' || c == '[')
            ++depth;
        else if ((c == ')' || c == '}' || c == ']') && --depth == 0)
            return i + 1;
    }
    return std::string::npos;
}

// Splits `s` at top-level `sep` (outside brackets and strings).
std::vector<std::string> splitTop(const std::string &s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    size_t begin = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            for (++i; i < s.size() && s[i] != '"'; ++i)
                if (s[i] == '\\')
                    ++i;
            continue;
        }
        if (c == '(' || c == '{' || c == '[')
            ++depth;
        else if (c == ')' || c == '}' || c == ']')
            --depth;
        else if (c == sep && depth == 0) {
            out.push_back(s.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    out.push_back(s.substr(begin));
    return out;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

// The identifier at the start of `s` (after spaces).
std::string leadingIdent(const std::string &s, size_t *end = nullptr) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    const size_t b = i;
    while (i < s.size() && isIdent(s[i]))
        ++i;
    if (end)
        *end = i;
    return s.substr(b, i - b);
}

// Removes `name(...);` statements and `import name;` lines.
std::string dropStatements(std::string s, const std::string &name) {
    const std::string import = "import " + name + ";";
    for (size_t p; (p = s.find(import)) != std::string::npos;)
        s.erase(p, import.size());
    for (size_t p = 0; (p = s.find(name + "(", p)) != std::string::npos;) {
        if (p > 0 && isIdent(s[p - 1])) {
            p += name.size();
            continue;
        }
        const size_t close = matchClose(s, p + name.size());
        const size_t semi = close == std::string::npos ? std::string::npos : s.find(';', close);
        if (semi == std::string::npos)
            break;
        s.erase(p, semi + 1 - p);
    }
    return s;
}

struct Global {
    const char *decl; // "type name = default"
    const char *name;
};

// VEX's surface globals as CVEX parameters (N, Ng: the authored normals).
constexpr Global kSurfaceGlobals[] = {
    {"vector P = 0", "P"},           {"vector I = {0, 0, -1}", "I"},       {"vector N = {0, 0, 1}", "N"},
    {"vector Ng = {0, 0, 1}", "Ng"}, {"float s = 0", "s"},                 {"float t = 0", "t"},
    {"vector dPds = {1, 0, 0}", "dPds"}, {"vector dPdt = {0, 1, 0}", "dPdt"}, {"float Time = 0", "Time"},
    {"vector Eye = 0", "Eye"},       {"export vector Cf = 0", "Cf"},       {"export vector Of = 1", "Of"},
    {"export float Af = 1", "Af"},   {"export vector Ce = 0", "Ce"},
};

// VEX's displacement globals; the function writes P and N.
constexpr Global kDisplaceGlobals[] = {
    {"export vector P = 0", "P"},    {"export vector N = {0, 0, 1}", "N"}, {"vector I = {0, 0, -1}", "I"},
    {"vector Ng = {0, 0, 1}", "Ng"}, {"float s = 0", "s"},                 {"float t = 0", "t"},
    {"vector dPds = {1, 0, 0}", "dPds"}, {"vector dPdt = {0, 1, 0}", "dPdt"}, {"float Time = 0", "Time"},
    {"vector Eye = 0", "Eye"},
};

constexpr const char *kNovaParams =
    "vector nova_wi = 0; float nova_sx = 0.5; float nova_sy = 0.5; "
    "export vector nova_light = 0; export float nova_light_pdf = 0; export vector nova_dir = 0; "
    "export vector nova_weight = 0; export float nova_pdf = 0; export int nova_bounce = 0; "
    "export vector nova_albedo = 0; export vector nova_opacity = 1; export vector nova_emission = 0";

constexpr const char *kPrelude = R"vex(// Nova CVEX bridge (houdini/cvex_bridge/transform.h): shading-only
// functions as CVEX stand-ins.
#define getraylevel() 0
#define getglobalraylevel() 0
#define isshadowray() 0
#define rayimport(name, value) 0
// Library code before the shader: shading globals it reads.
#define dPds set(1, 0, 0)
#define dPdt set(0, 1, 0)
#define isshadingRHS() 1
#define objectstate(name, value) 0
)vex";

constexpr const char *kLibraryEnd = R"vex(#undef dPds
#undef dPdt
// Nova: the shading context's two-argument frontface.
vector frontface(vector n; vector i) { return dot(n, i) > 0 ? -n : n; }
)vex";

constexpr const char *kTail = R"vex(
    // Nova: evaluate and sample the shader's bsdf (docs/design/M5.md D6).
    {
        vector nova_viewer = -normalize(I);
        int nova_all = bouncemask("all");
        float nova_p = 0;
        nova_light = eval_bsdf(F, nova_viewer, nova_wi, nova_p, nova_all);
        nova_light_pdf = nova_p;
        nova_albedo = albedo(F, nova_viewer, nova_all);
        vector nova_d = 0, nova_e = 0;
        float nova_sp = 0;
        int nova_t = 0;
        sample_bsdf(F, nova_viewer, nova_d, nova_e, nova_sp, nova_t, nova_sx, nova_sy, nova_all);
        nova_dir = nova_d;
        nova_weight = nova_e;
        nova_bounce = nova_t;
        float nova_q = 0;
        if (nova_t != 0)
            eval_bsdf(F, nova_viewer, nova_d, nova_q, nova_all);
        nova_pdf = nova_q;
        nova_opacity = Of;
        nova_emission = Ce;
    }
)vex";

// Without sample_bsdf (docs/design/M5.md, notes on M5.4: it faults on
// Houdini's native GGX lobes in some Houdini processes): the continuation is
// drawn from an equal mixture of a cosine lobe and Phong lobes (exponents 16,
// 128, 1024) around the mirror direction and weighted with eval_bsdf; the
// light's MIS pdf is the same mixture. Unbiased for non-delta lobes; perfect
// mirrors and refraction are not sampled. The pdfs are scaled by
// 2 pi luminance(albedo), the convention the bridge divides out.
constexpr const char *kSafeTail = R"vex(
    // Nova: evaluate the shader's bsdf, sample a fixed mixture.
    {
        float nova_pi = 3.14159265358979;
        float nova_x[] = {16, 128, 1024};
        vector nova_viewer = -normalize(I);
        int nova_all = bouncemask("all");
        float nova_p = 0;
        nova_light = eval_bsdf(F, nova_viewer, nova_wi, nova_p, nova_all);
        nova_albedo = albedo(F, nova_viewer, nova_all);
        float nova_scale = 2 * nova_pi * luminance(nova_albedo);
        vector nova_n = normalize(N);
        if (dot(nova_n, nova_viewer) < 0)
            nova_n = -nova_n;
        vector nova_r = normalize(2 * dot(nova_viewer, nova_n) * nova_n - nova_viewer);
        vector nova_l = normalize(nova_wi);
        float nova_ml = 0.25 * max(dot(nova_l, nova_n), 0) / nova_pi;
        foreach (float nova_e; nova_x)
            nova_ml += 0.25 * (nova_e + 1) / (2 * nova_pi) * pow(max(dot(nova_l, nova_r), 0), nova_e);
        nova_light_pdf = nova_scale * nova_ml;
        int nova_k = clamp(int(nova_sx * 4), 0, 3);
        float nova_w = nova_sx * 4 - nova_k;
        float nova_phi = 2 * nova_pi * nova_sy;
        vector nova_u = 0, nova_v = 0, nova_d = 0;
        if (nova_k == 0) {
            makebasis(nova_u, nova_v, nova_n);
            float nova_s = sqrt(nova_w);
            nova_d = nova_u * nova_s * cos(nova_phi) + nova_v * nova_s * sin(nova_phi) +
                     nova_n * sqrt(max(1 - nova_w, 0));
            nova_bounce = bouncemask("diffuse");
        } else {
            makebasis(nova_u, nova_v, nova_r);
            float nova_c = pow(nova_w, 1 / (nova_x[nova_k - 1] + 1));
            float nova_s = sqrt(max(1 - nova_c * nova_c, 0));
            nova_d = nova_u * nova_s * cos(nova_phi) + nova_v * nova_s * sin(nova_phi) + nova_r * nova_c;
            nova_bounce = bouncemask("reflect");
        }
        nova_d = normalize(nova_d);
        float nova_m = 0.25 * max(dot(nova_d, nova_n), 0) / nova_pi;
        foreach (float nova_e; nova_x)
            nova_m += 0.25 * (nova_e + 1) / (2 * nova_pi) * pow(max(dot(nova_d, nova_r), 0), nova_e);
        float nova_q = 0;
        vector nova_ev = eval_bsdf(F, nova_viewer, nova_d, nova_q, nova_all);
        if (!(nova_m > 0) || dot(nova_d, nova_n) <= 0)
            nova_bounce = 0;
        nova_dir = nova_d;
        nova_weight = nova_bounce != 0 ? nova_ev / (2 * nova_pi * nova_m) : 0;
        nova_pdf = nova_scale * nova_m;
        nova_opacity = Of;
        nova_emission = Ce;
    }
)vex";

} // namespace

std::vector<ParamDecl> parameters(const std::string &list) {
    std::vector<ParamDecl> out;
    for (const std::string &raw : splitTop(list, ';')) {
        std::string decl = trim(raw);
        bool exported = false;
        for (bool again = true; again;) {
            again = false;
            for (const char *qualifier : {"export ", "const ", "uniform ", "varying "})
                if (decl.rfind(qualifier, 0) == 0) {
                    exported |= qualifier[0] == 'e';
                    decl = trim(decl.substr(std::char_traits<char>::length(qualifier)));
                    again = true;
                }
        }
        size_t typeEnd = 0;
        const std::string type = leadingIdent(decl, &typeEnd);
        if (type.empty())
            continue;
        for (const std::string &part : splitTop(decl.substr(typeEnd), ',')) {
            ParamDecl p;
            p.type = type;
            p.exported = exported;
            p.name = leadingIdent(part);
            const size_t eq = part.find('=');
            if (eq != std::string::npos)
                p.defaultText = trim(part.substr(eq + 1));
            if (!p.name.empty())
                out.push_back(std::move(p));
        }
    }
    return out;
}

std::string unquote(const std::string &literal) {
    const std::string t = trim(literal);
    if (t.size() < 2 || t.front() != '"' || t.back() != '"')
        return t;
    std::string out;
    for (size_t i = 1; i + 1 < t.size(); ++i) {
        if (t[i] == '\\' && i + 2 < t.size())
            ++i;
        out += t[i];
    }
    return out;
}

std::string standInDefault(const std::string &name) {
    // Not P or N: ShaderLayer has fields of those names, which a macro would
    // break (init_layer's reads are rewritten instead).
    if (name == "Ng")
        return "set(0, 0, 1)";
    if (name == "I")
        return "set(0, 0, -1)";
    if (name == "Eye")
        return "set(0, 0, 0)";
    if (name == "s" || name == "t" || name == "Time")
        return "0";
    return {};
}

namespace {

// A shader function and how it becomes a CVEX function.
struct Kind {
    const char *keyword; // "surface", "displace"
    const Global *globals;
    size_t globalCount;
    const char *novaParams; // Nova's parameters ("": none)
    const char *locals;     // declarations at the start of the body
    const char *tail;       // statements at its end
};

Transformed transform(const std::string &source, const Kind &kind, const std::string &shaderLayerHeader,
                      const std::vector<std::string> &standIns) {
    Transformed out;
    const std::string keyword = kind.keyword;
    const size_t kl = keyword.size();
    // The function: the keyword at a line start, a name, "(".
    size_t at = std::string::npos, open = std::string::npos;
    for (size_t p = 0; (p = source.find(keyword, p)) != std::string::npos; p += kl) {
        size_t b = p;
        while (b > 0 && (source[b - 1] == ' ' || source[b - 1] == '\t'))
            --b;
        if ((b > 0 && source[b - 1] != '\n') || p + kl >= source.size() || isIdent(source[p + kl]))
            continue;
        size_t end = 0;
        const std::string name = leadingIdent(source.substr(p + kl), &end);
        size_t q = p + kl + end;
        while (q < source.size() && std::isspace(static_cast<unsigned char>(source[q])))
            ++q;
        if (!name.empty() && q < source.size() && source[q] == '(') {
            at = b;
            open = q;
            out.function = name;
            break;
        }
    }
    if (at == std::string::npos) {
        out.error = "no " + keyword + " function";
        return out;
    }
    const size_t close = matchClose(source, open);
    const size_t bodyOpen = close == std::string::npos ? std::string::npos : source.find('{', close);
    const size_t bodyClose = source.rfind('}');
    if (bodyOpen == std::string::npos || bodyClose == std::string::npos || bodyClose < bodyOpen) {
        out.error = "the " + keyword + " function is incomplete";
        return out;
    }
    const std::string params = trim(source.substr(open + 1, close - 1 - (open + 1)));
    out.params = parameters(params);
    // Ray-tracing calls need the renderer's ray tracer, which CVEX has not
    // (docs/design/M5.md D5): the material renders its preview instead.
    const std::string body0 = source.substr(bodyOpen, bodyClose - bodyOpen);
    for (size_t p = 0; (p = body0.find("Of", p)) != std::string::npos; p += 2)
        if ((p == 0 || !isIdent(body0[p - 1])) && (p + 2 >= body0.size() || !isIdent(body0[p + 2])))
            out.writesOpacity = true;
    for (const char *call : {"trace", "gather", "illuminance", "rayhittest", "fastshadow", "irradiance", "occlusion"}) {
        const std::string name = call;
        for (size_t p = 0; (p = body0.find(name, p)) != std::string::npos; p += name.size()) {
            size_t q = p + name.size();
            while (q < body0.size() && (body0[q] == ' ' || body0[q] == '\t'))
                ++q;
            if ((p > 0 && isIdent(body0[p - 1])) || q >= body0.size() || body0[q] != '(')
                continue;
            out.error = "the shader calls " + name + "(), which needs a ray tracer (not available in CVEX)";
            return out;
        }
    }

    std::string globals;
    for (size_t i = 0; i < kind.globalCount; ++i) {
        const Global &g = kind.globals[i];
        bool taken = false;
        for (const ParamDecl &p : out.params)
            taken |= p.name == g.name;
        if (!taken)
            globals += std::string(g.decl) + "; ";
    }

    std::string layer;
    if (!shaderLayerHeader.empty()) {
        layer = shaderLayerHeader;
        // init_layer reads the surface globals P and N.
        for (auto [from, to] : {std::pair{"layer.P = P;", "layer.P = 0;"},
                                std::pair{"layer.N = normalize(N);", "layer.N = {0, 0, 1};"}}) {
            const size_t p = layer.find(from);
            if (p != std::string::npos)
                layer.replace(p, std::char_traits<char>::length(from), to);
        }
        layer += "\n";
    }

    const std::string prefix = dropStatements(source.substr(0, at), "pbrlighting");
    const std::string body = dropStatements(source.substr(bodyOpen + 1, bodyClose - (bodyOpen + 1)), "pbrlighting");
    std::string define, undefine;
    for (const std::string &name : standIns) {
        const std::string value = standInDefault(name);
        if (!value.empty()) {
            define += "#define " + name + " " + value + "\n";
            undefine += "#undef " + name + "\n";
        }
    }
    std::string list = globals + kind.novaParams;
    if (!params.empty())
        list += (list.empty() || list.ends_with("; ") ? "" : "; ") + params;
    while (list.ends_with("; "))
        list.resize(list.size() - 2);
    // The body in a block of its own: its locals shadow the globals (now
    // parameters) as they shadow VEX's globals (builder code declares a
    // local Ce, for example). An early `return` skips the tail.
    // A name of its own per code: VEX keeps functions by name, and Houdini
    // may compile the original shader (same name, other context), or two
    // materials share a name with different code.
    uint64_t hash = 1469598103934665603ull;
    for (const char c : source)
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "_nova%016llx", static_cast<unsigned long long>(hash));
    out.cvexFunction = out.function + suffix;
    out.source = std::string(kPrelude) + define + layer + prefix + undefine + kLibraryEnd + "cvex\n" + out.cvexFunction +
                 "(" + list + ")\n{\n" + kind.locals + "    {\n" + body + "\n    }\n" + kind.tail + "}\n";
    out.ok = true;
    return out;
}

} // namespace

Transformed transformSurface(const std::string &source, const std::string &shaderLayerHeader,
                             const std::vector<std::string> &standIns, bool safeSampling) {
    const Kind kind{"surface",   kSurfaceGlobals, std::size(kSurfaceGlobals), kNovaParams, "    bsdf F;\n",
                    safeSampling ? kSafeTail : kTail};
    return transform(source, kind, shaderLayerHeader, standIns);
}

Transformed transformDisplacement(const std::string &source, const std::string &shaderLayerHeader,
                                  const std::vector<std::string> &standIns) {
    const Kind kind{"displace", kDisplaceGlobals, std::size(kDisplaceGlobals), "", "", ""};
    return transform(source, kind, shaderLayerHeader, standIns);
}

} // namespace nova::cvex
