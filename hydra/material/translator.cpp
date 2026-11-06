#include "translator.h"

#include "nodeDefDatabase.h"
#include "../colorSpace.h"
#include "../primvarUtil.h"

#include "core/bsdf/closure.h"
#include "core/bsdf/surface.h"

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Value.h>

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix3f.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <cstdlib>
#include <cstring>
#include <array>
#include <tuple>
#include <memory>
#include <functional>
#include <map>
#include <set>

namespace mx = MaterialX;

PXR_NAMESPACE_OPEN_SCOPE

namespace {

namespace sh = nova::shading;
namespace sf = nova::surface_field;
using sh::Global;
using sh::kNoRef;
using sh::Op;
using sh::Ref;

int typeWidth(std::string const &type) {
    if (type == "float" || type == "integer" || type == "boolean")
        return 1;
    if (type == "vector2")
        return 2;
    if (type == "color3" || type == "vector3")
        return 3;
    if (type == "color4" || type == "vector4")
        return 4;
    return 0; // strings, filenames, shaders, matrices
}

// matrix33 and matrix44 (row-major, row vectors: v' = v M, as in MaterialX).
int matrixSize(std::string const &type) { return type == "matrix33" ? 3 : type == "matrix44" ? 4 : 0; }

// The n*n values of a matrix literal; the identity if it does not parse.
void parseMatrix(int n, std::string const &text, float m[16]) {
    for (int i = 0; i < n * n; ++i)
        m[i] = i / n == i % n ? 1.0f : 0.0f;
    float v[16];
    int count = 0;
    char const *s = text.c_str();
    while (*s && count < 16) {
        char *end = nullptr;
        const float f = std::strtof(s, &end);
        if (end == s) {
            ++s;
            continue;
        }
        v[count++] = f;
        s = end;
    }
    if (count == n * n)
        for (int i = 0; i < count; ++i)
            m[i] = v[i];
}

bool parseFloats(std::string const &type, std::string const &text, float out[4]) {
    if (typeWidth(type) == 0 || text.empty())
        return false;
    mx::ValuePtr v;
    try {
        v = mx::Value::createValueFromStrings(text, type);
    } catch (...) {
        return false;
    }
    if (!v)
        return false;
    if (v->isA<float>()) {
        out[0] = v->asA<float>();
    } else if (v->isA<int>()) {
        out[0] = float(v->asA<int>());
    } else if (v->isA<bool>()) {
        out[0] = v->asA<bool>() ? 1.0f : 0.0f;
    } else if (v->isA<mx::Color3>()) {
        const mx::Color3 c = v->asA<mx::Color3>();
        for (int k = 0; k < 3; ++k)
            out[k] = c[k];
    } else if (v->isA<mx::Color4>()) {
        const mx::Color4 c = v->asA<mx::Color4>();
        for (int k = 0; k < 4; ++k)
            out[k] = c[k];
    } else if (v->isA<mx::Vector2>()) {
        const mx::Vector2 c = v->asA<mx::Vector2>();
        for (int k = 0; k < 2; ++k)
            out[k] = c[k];
    } else if (v->isA<mx::Vector3>()) {
        const mx::Vector3 c = v->asA<mx::Vector3>();
        for (int k = 0; k < 3; ++k)
            out[k] = c[k];
    } else if (v->isA<mx::Vector4>()) {
        const mx::Vector4 c = v->asA<mx::Vector4>();
        for (int k = 0; k < 4; ++k)
            out[k] = c[k];
    } else {
        return false;
    }
    return true;
}

std::string toText(VtValue const &v) {
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    if (v.IsHolding<SdfAssetPath>()) {
        SdfAssetPath const &a = v.UncheckedGet<SdfAssetPath>();
        return a.GetResolvedPath().empty() ? a.GetAssetPath() : a.GetResolvedPath();
    }
    if (v.IsHolding<bool>())
        return v.UncheckedGet<bool>() ? "true" : "false";
    if (v.IsHolding<int>())
        return std::to_string(v.UncheckedGet<int>());
    return TfStringify(v);
}

// A scalar enum input authored by name (USD declares e.g. UsdPreviewSurface
// opacityMode as a token, MaterialX as an integer with enum/enumvalues): the
// enumvalue at the name's index. Anything else is returned unchanged.
VtValue enumToValue(mx::InputPtr const &input, VtValue const &value) {
    if (typeWidth(input->getType()) != 1 || !(value.IsHolding<TfToken>() || value.IsHolding<std::string>()))
        return value;
    const std::vector<std::string> names = TfStringSplit(input->getAttribute("enum"), ",");
    const std::vector<std::string> values = TfStringSplit(input->getAttribute("enumvalues"), ",");
    if (names.empty() || names.size() != values.size())
        return value;
    const std::string text = TfStringTrim(toText(value));
    for (size_t i = 0; i < names.size(); ++i)
        if (TfStringTrim(names[i]) == text)
            return VtValue(TfStringTrim(values[i]));
    return value;
}

// USD shader ids and legacy names -> MaterialX node definitions.
std::string nodeDefName(std::string const &id) {
    static const std::map<std::string, std::string> aliases = {
        {"UsdPreviewSurface", "ND_UsdPreviewSurface_surfaceshader"},
        {"UsdUVTexture", "ND_UsdUVTexture_23"},
        {"UsdPrimvarReader_float", "ND_UsdPrimvarReader_float"},
        {"UsdPrimvarReader_float2", "ND_UsdPrimvarReader_vector2"},
        {"UsdPrimvarReader_float3", "ND_UsdPrimvarReader_vector3"},
        {"UsdPrimvarReader_normal", "ND_UsdPrimvarReader_vector3"},
        {"UsdPrimvarReader_point", "ND_UsdPrimvarReader_vector3"},
        {"UsdPrimvarReader_vector", "ND_UsdPrimvarReader_vector3"},
        {"UsdPrimvarReader_float4", "ND_UsdPrimvarReader_vector4"},
        {"UsdPrimvarReader_int", "ND_UsdPrimvarReader_integer"},
        {"UsdPrimvarReader_string", "ND_UsdPrimvarReader_string"},
        {"UsdTransform2d", "ND_UsdTransform2d"},
        {"ND_normalmap", "ND_normalmap_float"},
    };
    const auto it = aliases.find(id);
    return it != aliases.end() ? it->second : id;
}

std::string primaryUvName() {
    const std::string env = TfGetenv("USDMTLX_PRIMARY_UV_NAME");
    return env.empty() ? std::string("st") : env;
}

// Surface model inputs -> SurfaceInputs fields.
struct FieldMap {
    const char *input;
    uint16_t field;
    uint8_t width;
};
const FieldMap kStandardSurface[] = {
    {"base", sf::baseWeight, 1},
    {"base_color", sf::baseColor, 3},
    {"diffuse_roughness", sf::diffuseRoughness, 1},
    {"metalness", sf::metalness, 1},
    {"specular", sf::specularWeight, 1},
    {"specular_color", sf::specularColor, 3},
    {"specular_roughness", sf::specularRoughness, 1},
    {"specular_IOR", sf::specularIor, 1},
    {"specular_anisotropy", sf::specularAnisotropy, 1},
    {"specular_rotation", sf::specularRotation, 1},
    {"transmission", sf::transmissionWeight, 1},
    {"transmission_color", sf::transmissionColor, 3},
    {"transmission_depth", sf::transmissionDepth, 1},
    {"transmission_scatter", sf::transmissionScatter, 3},
    {"transmission_scatter_anisotropy", sf::transmissionScatterAnisotropy, 1},
    {"transmission_dispersion", sf::transmissionDispersion, 1},
    {"transmission_extra_roughness", sf::transmissionExtraRoughness, 1},
    {"subsurface", sf::subsurfaceWeight, 1},
    {"subsurface_color", sf::subsurfaceColor, 3},
    {"subsurface_radius", sf::subsurfaceRadius, 3},
    {"subsurface_scale", sf::subsurfaceScale, 1},
    {"subsurface_anisotropy", sf::subsurfaceAnisotropy, 1},
    {"sheen", sf::sheenWeight, 1},
    {"sheen_color", sf::sheenColor, 3},
    {"sheen_roughness", sf::sheenRoughness, 1},
    {"coat", sf::coatWeight, 1},
    {"coat_color", sf::coatColor, 3},
    {"coat_roughness", sf::coatRoughness, 1},
    {"coat_anisotropy", sf::coatAnisotropy, 1},
    {"coat_rotation", sf::coatRotation, 1},
    {"coat_IOR", sf::coatIor, 1},
    {"coat_normal", sf::coatNormal, 3},
    {"coat_affect_color", sf::coatAffectColor, 1},
    {"coat_affect_roughness", sf::coatAffectRoughness, 1},
    {"thin_film_thickness", sf::thinFilmThickness, 1},
    {"thin_film_IOR", sf::thinFilmIor, 1},
    {"emission", sf::emissionWeight, 1},
    {"emission_color", sf::emissionColor, 3},
    {"opacity", sf::opacity, 3},
    {"thin_walled", sf::thinWalled, 1},
    {"normal", sf::normal, 3},
    {"tangent", sf::tangent, 3},
};
const FieldMap kOpenPbr[] = {
    {"base_weight", sf::baseWeight, 1},
    {"base_color", sf::baseColor, 3},
    {"base_diffuse_roughness", sf::diffuseRoughness, 1},
    {"base_metalness", sf::metalness, 1},
    {"specular_weight", sf::specularWeight, 1},
    {"specular_color", sf::specularColor, 3},
    {"specular_roughness", sf::specularRoughness, 1},
    {"specular_ior", sf::specularIor, 1},
    {"specular_roughness_anisotropy", sf::specularAnisotropy, 1},
    {"transmission_weight", sf::transmissionWeight, 1},
    {"transmission_color", sf::transmissionColor, 3},
    {"transmission_depth", sf::transmissionDepth, 1},
    {"transmission_scatter", sf::transmissionScatter, 3},
    {"transmission_scatter_anisotropy", sf::transmissionScatterAnisotropy, 1},
    {"transmission_dispersion_abbe_number", sf::transmissionDispersion, 1},
    {"subsurface_weight", sf::subsurfaceWeight, 1},
    {"subsurface_color", sf::subsurfaceColor, 3},
    {"subsurface_radius", sf::subsurfaceScale, 1},
    {"subsurface_radius_scale", sf::subsurfaceRadius, 3},
    {"subsurface_scatter_anisotropy", sf::subsurfaceAnisotropy, 1},
    {"fuzz_weight", sf::sheenWeight, 1},
    {"fuzz_color", sf::sheenColor, 3},
    {"fuzz_roughness", sf::sheenRoughness, 1},
    {"coat_weight", sf::coatWeight, 1},
    {"coat_color", sf::coatColor, 3},
    {"coat_roughness", sf::coatRoughness, 1},
    {"coat_roughness_anisotropy", sf::coatAnisotropy, 1},
    {"coat_ior", sf::coatIor, 1},
    {"coat_darkening", sf::coatDarkening, 1},
    {"thin_film_weight", sf::thinFilmWeight, 1},
    {"thin_film_thickness", sf::thinFilmThickness, 1},
    {"thin_film_ior", sf::thinFilmIor, 1},
    {"emission_luminance", sf::emissionWeight, 1},
    {"emission_color", sf::emissionColor, 3},
    {"geometry_opacity", sf::opacity, 3},
    {"geometry_thin_walled", sf::thinWalled, 1},
    {"geometry_normal", sf::normal, 3},
    {"geometry_coat_normal", sf::coatNormal, 3},
    {"geometry_tangent", sf::tangent, 3},
    {"geometry_coat_tangent", sf::coatTangent, 3},
};
const FieldMap kUsdPreview[] = {
    {"diffuseColor", sf::baseColor, 3},
    {"emissiveColor", sf::emissionColor, 3},
    {"useSpecularWorkflow", sf::useSpecularWorkflow, 1},
    {"specularColor", sf::specularColor, 3},
    {"metallic", sf::metalness, 1},
    {"roughness", sf::specularRoughness, 1},
    {"clearcoat", sf::coatWeight, 1},
    {"clearcoatRoughness", sf::coatRoughness, 1},
    {"ior", sf::specularIor, 1},
};
const FieldMap kUnlit[] = {
    {"emission", sf::emissionWeight, 1},
    {"emission_color", sf::emissionColor, 3},
    {"transmission", sf::transmissionWeight, 1},
    {"transmission_color", sf::transmissionColor, 3},
    {"opacity", sf::opacity, 3},
};
// Karma Hair and Karma Fur (docs/design/M9.md D6): the fibre controls both
// share; Fur's medulla, second R lobe and coat warn (KarmaHairWarnings).
const FieldMap kKarmaHair[] = {
    {"base", sf::hairBase, 1},
    {"baseColor", sf::hairBaseColor, 3},
    {"melanin", sf::hairMelanin, 1},
    {"melaninRedness", sf::hairMelaninRedness, 1},
    {"melaninRandomize", sf::hairMelaninRandomize, 1},
    {"thicknessScale", sf::hairThicknessScale, 1},
    {"tint", sf::hairTint, 3},
    {"shift", sf::hairShift, 1},
    {"ior", sf::hairIor, 1},
    {"absorption", sf::hairAbsorption, 3},
    {"roughness", sf::hairRoughness, 1},
    {"roughnessAzimuthal", sf::hairRoughnessAzimuthal, 1},
    {"roughnessAnisotropic", sf::hairRoughnessAnisotropic, 1},
    {"roughnessRandomize", sf::hairRoughnessRandomize, 1},
    {"roughness_advanced", sf::hairRoughnessAdvanced, 1},
    {"roughness_ttscale", sf::hairTtScale, 1},
    {"roughness_trtscale", sf::hairTrtScale, 1},
    {"diffuse", sf::hairDiffuse, 1},
    {"diffuseColor", sf::hairDiffuseColor, 3},
};

struct Value {
    Ref ref = kNoRef;       // numeric value
    std::vector<Ref> rows;  // matrix value: n rows of width n
    std::string text;       // literal: strings, enums, or the numeric literal (empty if computed)
    std::string colorSpace; // authored colorSpace metadata (file inputs)
    // Array values (floatarray, vector3array, ..., stringarray): the
    // elements, `arrayWidth` floats each, or the strings.
    std::vector<float> array;
    int arrayWidth = 0;
    std::vector<std::string> strings;
    // Closures (M4.5): a BSDF node, an EDF, a VDF, a surface, displacement
    // or volume shader (M8.1) of the translator's closure tables (handle -1:
    // none).
    enum class Closure : uint8_t { None, Bsdf, Edf, Vdf, Surface, Displacement, Volume } closure = Closure::None;
    int handle = -1;
    bool geomDefault = false; // an unconnected input reading its defaultgeomprop (normal, tangent, ...)
};

// Element width of a MaterialX array type ("vector3array" -> 3), 0 if the
// type is not a numeric array.
int arrayWidth(std::string const &type) {
    if (!TfStringEndsWith(type, "array") || type == "stringarray")
        return 0;
    const std::string element = type.substr(0, type.size() - 5);
    return element == "integer" ? 1 : typeWidth(element);
}

// The elements of a value text "a, b, c" (numbers or names).
std::vector<std::string> listItems(std::string const &text) {
    std::vector<std::string> items;
    for (std::string const &item : TfStringSplit(text, ","))
        if (!TfStringTrim(item).empty())
            items.push_back(TfStringTrim(item));
    return items;
}
using ValueMap = std::map<std::string, Value>;

// The fields of a volume shader (core/volume/shader.h) and their widths.
constexpr std::pair<uint16_t, int> kVolumeFields[] = {
    {nova::volume_field::scattering, 3},          {nova::volume_field::absorption, 3},
    {nova::volume_field::emission, 3},            {nova::volume_field::anisotropy, 1},
    {nova::volume_field::secondaryAnisotropy, 1}, {nova::volume_field::secondaryMix, 1},
    {nova::volume_field::extinctionModifier, 1},  {nova::volume_field::contributionModifier, 1},
    {nova::volume_field::modifierStartLevel, 1},
};

class Translator {
public:
    Translator(HdNovaNetworkSource const &src, HdNovaTranslatedMaterial &out)
        : _src(src), _out(out), _lib(HdNovaNodeDefDatabase::Get().GetLibrary()), _b(out.graph) {}

    void Run();

private:
    HdNovaNetworkSource const &_src;
    HdNovaTranslatedMaterial &_out;
    mx::ConstDocumentPtr _lib;
    sh::IrBuilder _b;
    std::map<SdfPath, ValueMap> _userOutputs;
    std::set<SdfPath> _visiting;
    std::set<std::string> _warned;

    void Warn(std::string const &key, std::string const &message) {
        if (_warned.insert(key).second)
            _out.warnings.push_back(message);
    }
    mx::NodeDefPtr ResolveNodeDef(TfToken const &id) const {
        return _lib ? _lib->getNodeDef(nodeDefName(id.GetString())) : nullptr;
    }
    Ref Zero(int width) {
        const float z[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        return _b.constant(z, width);
    }
    Ref Param(float const *v, int width) {
        const uint32_t offset = uint32_t(_out.params.size());
        _out.params.insert(_out.params.end(), v, v + width);
        return _b.param(offset, width);
    }
    std::vector<Ref> MatrixRows(float const *m, int n, bool asParam) {
        std::vector<Ref> rows;
        for (int i = 0; i < n; ++i)
            rows.push_back(asParam ? Param(m + i * n, n) : _b.constant(m + i * n, n));
        return rows;
    }
    // A space name of MaterialX (object, model, world) or Karma (the same with
    // "space:", and current: Karma shades in world space, so its current
    // space is the world's; ndc for positions): 1 object, 2 world, 3 NDC
    // (with `ndc`); an empty name is object space (MaterialX's default).
    // Others warn once and give 0 (`fallback` names what happens then).
    int KarmaSpace(std::string s, bool ndc, char const *fallback) {
        if (s.rfind("space:", 0) == 0)
            s = s.substr(6);
        if (s.empty() || s == "object" || s == "model")
            return 1;
        if (s == "world" || s == "current")
            return 2;
        if (ndc && s == "ndc")
            return 3;
        Warn("space:" + s, "space '" + s + "' is not supported; " + fallback);
        return 0;
    }
    // A world-space vector (or point) in object space: Cramer's rule on the
    // object axes.
    Ref WorldToObject(Ref v, bool point) {
        const Ref X = _b.global(Global::ObjX), Y = _b.global(Global::ObjY), Z = _b.global(Global::ObjZ);
        const Ref d = point ? _b.sub(v, _b.global(Global::ObjT)) : v;
        const Ref det = _b.dot(X, _b.cross(Y, Z));
        const Ref r = _b.concat(_b.concat(_b.dot(d, _b.cross(Y, Z)), _b.dot(d, _b.cross(Z, X))), _b.dot(d, _b.cross(X, Y)));
        return _b.div(r, det);
    }
    // An object-space normal in world space (unnormalized): the cofactor
    // transform times the handedness, i.e. the inverse-transpose direction,
    // as core/scene/hit.h does for Global::N (mirrored instances keep the
    // authored orientation).
    Ref ObjectToWorldNormal(Ref v) {
        const Ref X = _b.global(Global::ObjX), Y = _b.global(Global::ObjY), Z = _b.global(Global::ObjZ);
        const Ref n = _b.add(_b.add(_b.mul(_b.cross(Y, Z), _b.component(v, 0)),
                                    _b.mul(_b.cross(Z, X), _b.component(v, 1))),
                             _b.mul(_b.cross(X, Y), _b.component(v, 2)));
        const Ref handed = _b.ternary(Op::Select, _b.binary(Op::Greater, Zero(1), _b.dot(X, _b.cross(Y, Z))),
                                      _b.constant(-1.0f), _b.constant(1.0f));
        return _b.mul(n, handed);
    }
    // Karma's tangent frame (mxstd_computeTangents): T = norm(dPdu - N(N.dPdu)),
    // B = norm(N x T) sign((T x dPdv).N), from the primary uv set.
    Ref WorldN() { return _b.normalize(_b.global(Global::N)); }
    Ref KarmaT() {
        const Ref n = WorldN(), du = _b.global(Global::DPdu);
        return _b.normalize(_b.sub(du, _b.mul(n, _b.dot(n, du))));
    }
    Ref KarmaB() {
        const Ref n = WorldN(), t = KarmaT();
        return _b.mul(_b.normalize(_b.cross(n, t)),
                      _b.unary(Op::Sign, _b.dot(_b.cross(t, _b.global(Global::DPdv)), n)));
    }
    // Karma's MikkT normal maps (the karma_mikkt input; docs/research/m4/
    // nodes-and-limits.md 3.1): r = norm(v.x T + v.y s (N x T) + v.z N) with
    // T the tangentu primvar (object space, PolyFrame's MikkT; unnormalized)
    // and s the signs primvar (-1 if missing); without tangentu, Karma's uv
    // tangent. v.xy are already scaled.
    Ref MikktNormal(Ref v, Ref n) {
        const Ref tu = _b.primvar("tangentu", 3, Zero(3));
        const Ref world = _b.add(_b.add(_b.mul(_b.global(Global::ObjX), _b.component(tu, 0)),
                                        _b.mul(_b.global(Global::ObjY), _b.component(tu, 1))),
                                 _b.mul(_b.global(Global::ObjZ), _b.component(tu, 2)));
        const Ref T = _b.ternary(Op::Select, _b.binary(Op::Greater, _b.dot(world, world), Zero(1)), world, KarmaT());
        const Ref s = _b.primvar("signs", 1, _b.constant(-1.0f));
        return _b.normalize(_b.add(_b.add(_b.mul(T, _b.component(v, 0)), _b.mul(_b.mul(_b.cross(n, T), s), _b.component(v, 1))),
                                   _b.mul(n, _b.component(v, 2))));
    }
    // Karma's guard: where r falls on the other side of Ng than N, r -= Ng (r.Ng) 1.001.
    Ref NgGuard(Ref r, Ref n) {
        const Ref ng = _b.normalize(_b.global(Global::Ng));
        const Ref rd = _b.dot(r, ng), nd2 = _b.dot(n, ng);
        const Ref same = _b.binary(Op::Equal, _b.unary(Op::Sign, rd), _b.unary(Op::Sign, nd2));
        return _b.ternary(Op::Select, same, r, _b.sub(r, _b.mul(ng, _b.mul(rd, _b.constant(1.001f)))));
    }
    // The karma_mikkt input of a node, or of the node graph being expanded.
    int _mikkt = 0;
    static bool Enabled(std::string const &text) { return !text.empty() && text != "0" && text != "false"; }
    // A world-space direction in the given space (object/model: the object's).
    Ref InSpace(Ref worldDir, std::string const &space) {
        return space == "object" || space == "model" ? _b.normalize(WorldToObject(worldDir, false)) : worldDir;
    }
    Ref GeomProp(std::string const &name, int width);
    Value DefaultValue(mx::InputPtr const &input, bool asParam);
    Value UserValue(std::string const &type, VtValue const &value);
    static Value PickOutput(ValueMap const &outputs, std::string const &name) {
        if (!name.empty()) {
            const auto it = outputs.find(name);
            if (it != outputs.end())
                return it->second;
        }
        const auto out = outputs.find("out");
        if (out != outputs.end())
            return out->second;
        return outputs.empty() ? Value{} : outputs.begin()->second;
    }
    Value InputOf(SdfPath const &path, HdMaterialNode2 const &node, mx::InputPtr const &input);
    ValueMap LowerUser(SdfPath const &path);
    void LowerNode(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out, int depth);
    bool LowerNative(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out);
    bool LowerMatrix(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out);
    bool LowerTexture(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out);
    bool LowerHexTile(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out);
    // The slot of a texture lookup: one per (texture, state).
    uint32_t TextureSlot(HdNovaTextureRequest const &request, nova::TexSlot const &slot) {
        std::string key = request.path + '\x1f' + request.colorSpace + '\x1f' + request.layer + '\x1f';
        key += char('0' + int(slot.wrapS));
        key += char('0' + int(slot.wrapT));
        key += char('0' + int(slot.filter));
        key += char('0' + int(slot.channels));
        const auto it = _textureSlots.find(key);
        if (it != _textureSlots.end())
            return it->second;
        const uint32_t s = _b.textureSlot(slot);
        _out.textures.push_back(request);
        _textureSlots.emplace(key, s);
        return s;
    }
    std::map<std::string, uint32_t> _textureSlots;
    bool LowerPbrUtility(std::string const &category, ValueMap &in, ValueMap &out);
    bool LowerKarma(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out);
    Ref Ramp(Ref t, Value const &keys, Value const &values, Value const &bases, int width);
    Ref RayVariable(std::string name);
    bool ZeroDisplacement(SdfPath const &path) const;
    bool LowerGraph(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out, int depth);
    void DefaultOutputs(mx::NodeDefPtr const &nd, ValueMap &out);

    // ---- Generic closures (M4.5; core/bsdf/closure_graph.h) ----
    struct ClosureNode {
        nova::GraphOp op = nova::GraphOp::Leaf;
        nova::LeafKind kind = nova::LeafKind::Diffuse;
        uint8_t flags = 0;
        int a = -1, b = -1;
        std::vector<Ref> params;           // the record in order (leaves), or the factor (mix, multiply, vdf)
        Ref normal = kNoRef, tangent = kNoRef; // leaves: the frame (kNoRef: the geometry's)
        bool film = false;                 // legacy thin_film_bsdf: params thickness, ior
    };
    struct EmissionTerm {
        Ref color = kNoRef;   // 3 wide, every linear factor applied
        Ref schlick = kNoRef; // 7 wide: color0, color90, exponent
    };
    struct SurfaceSegment {
        nova::SurfaceModel model = nova::SurfaceModel::Graph;
        int root = -1;
        std::vector<EmissionTerm> emission;
        Ref weight = kNoRef;     // 1 wide; kNoRef: 1
        Ref opacity = kNoRef;    // 3 wide; kNoRef: 1
        Ref thinWalled = kNoRef; // 1 wide
        nova::SurfaceInputs const *defaults = nullptr; // native: the model's defaults
        std::vector<std::tuple<uint16_t, int, Ref>> fields; // native: field, width, value
    };
    // A medium (VDF): coefficients per unit length (3 wide each) and the
    // Henyey-Greenstein g of its scattering (1 wide).
    struct VdfTerm {
        Ref absorption = kNoRef;
        Ref scattering = kNoRef;
        Ref anisotropy = kNoRef;
    };
    // A volume shader (docs/design/M8.md D4): the fields of
    // core/volume/shader.h, each kNoRef for its default.
    struct VolumeShader {
        Ref field[nova::volume_field::kCount] = {};
        VolumeShader() { std::fill(std::begin(field), std::end(field), kNoRef); }
    };
    std::vector<ClosureNode> _closures;
    std::vector<std::vector<EmissionTerm>> _edfs;
    std::vector<VdfTerm> _vdfs;
    std::vector<VolumeShader> _volumeShaders;
    // The volume terminal as the volume outputs; false if there is none.
    bool LowerVolume();
    std::vector<std::vector<SurfaceSegment>> _surfaces;
    std::vector<std::unique_ptr<nova::SurfaceInputs>> _segmentDefaults;
    std::vector<Ref> _displacements; // object-space offsets (3)
    // The displacement terminal (M4.6) as the kDisplacement output.
    void LowerDisplacement();
    // Karma's material AOVs (kma_aov): the surface shader's aov:<name>
    // inputs as outputs behind the fields in use (docs/design/M6.md D9).
    void LowerAovs(HdMaterialNode2 const &shader);
    void KarmaHairWarnings(std::string const &category, HdMaterialNode2 const &shader);
    // Object-space unit normal and Karma's tangent frame in object space
    // (vector displacement, docs/research/m4/displacement.md 2.2).
    Ref ObjectN() { return _b.normalize(_b.global(Global::Nobject)); }
    void ObjectFrame(Ref &t, Ref &b) {
        const Ref n = ObjectN();
        const Ref du = WorldToObject(_b.global(Global::DPdu), false), dv = WorldToObject(_b.global(Global::DPdv), false);
        t = _b.normalize(_b.sub(du, _b.mul(n, _b.dot(n, du))));
        b = _b.mul(_b.normalize(_b.cross(n, t)), _b.unary(Op::Sign, _b.dot(_b.cross(t, dv), n)));
    }
    static bool ForceExpand(std::string const &category);
    static bool NativeModel(std::string const &category) {
        return category == "standard_surface" || category == "open_pbr_surface" || category == "surface_unlit" ||
               category == "UsdPreviewSurface" || category == "kma_hair" || category == "kma_fur" || category == "kma_fur_2";
    }
    bool LowerClosure(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out);
    bool NativeSegment(mx::NodeDefPtr const &nd, ValueMap &in, SurfaceSegment &seg);
    bool EmitGraphSurface(Value const &surface);
    bool IsConst(Ref r, float *values = nullptr) const {
        if (r == kNoRef || _out.graph.nodes[r].op != Op::Const)
            return false;
        if (values)
            for (int k = 0; k < _out.graph.nodes[r].width; ++k)
                values[k] = _out.graph.constants[_out.graph.nodes[r].imm + uint32_t(k)];
        return true;
    }
};

Ref Translator::GeomProp(std::string const &name, int width) {
    mx::GeomPropDefPtr def = _lib ? _lib->getGeomPropDef(name) : nullptr;
    const std::string prop = def ? def->getGeomProp() : name;
    const std::string space = def ? def->getSpace() : std::string();
    if (prop == "texcoord")
        return _b.primvar(primaryUvName(), width, Zero(1));
    if (prop == "position")
        return _b.convert(_b.global(space == "world" ? Global::P : Global::Pobject), width);
    if (prop == "normal")
        return _b.convert(space == "world" ? _b.normalize(_b.global(Global::N)) : _b.global(Global::Nobject), width);
    if (prop == "geomcolor")
        return _b.primvar("displayColor", width, _b.constant(1.0f));
    if (prop == "viewdirection") // the incident direction, as Karma's I
        return _b.convert(space == "world" ? _b.global(Global::I) : WorldToObject(_b.global(Global::I), false), width);
    if (prop == "tangent" || prop == "bitangent")
        return _b.convert(InSpace(prop == "tangent" ? KarmaT() : KarmaB(), space.empty() ? "object" : space), width);
    Warn("geomprop:" + name, "geometric property '" + name + "' is not supported yet; using 0");
    return Zero(width);
}

Value Translator::DefaultValue(mx::InputPtr const &input, bool asParam) {
    Value v;
    v.text = input->getValueString();
    if (input->getType() == "stringarray") {
        v.strings = listItems(v.text);
        return v;
    }
    if (const int aw = arrayWidth(input->getType())) {
        for (std::string const &item : listItems(v.text))
            v.array.push_back(float(std::atof(item.c_str())));
        v.arrayWidth = aw;
        return v;
    }
    if (const int n = matrixSize(input->getType())) {
        float m[16];
        parseMatrix(n, v.text, m);
        v.rows = MatrixRows(m, n, asParam);
        return v;
    }
    const int w = typeWidth(input->getType());
    if (w == 0)
        return v;
    const std::string geomprop = input->getDefaultGeomPropString();
    if (!geomprop.empty()) {
        v.ref = GeomProp(geomprop, w);
        v.geomDefault = true;
        return v;
    }
    float f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    parseFloats(input->getType(), v.text, f);
    v.ref = asParam ? Param(f, w) : _b.constant(f, w);
    return v;
}

Value Translator::UserValue(std::string const &type, VtValue const &value) {
    Value v;
    v.text = toText(value);
    if (type == "stringarray") {
        if (value.IsHolding<VtArray<TfToken>>())
            for (TfToken const &t : value.UncheckedGet<VtArray<TfToken>>())
                v.strings.push_back(t.GetString());
        else if (value.IsHolding<VtArray<std::string>>())
            for (std::string const &s : value.UncheckedGet<VtArray<std::string>>())
                v.strings.push_back(s);
        return v;
    }
    if (const int aw = arrayWidth(type)) {
        int dataWidth = 0;
        if (HdNovaPrimvarToFloats(value, &dataWidth, &v.array) && dataWidth == aw)
            v.arrayWidth = aw;
        else
            v.array.clear();
        return v;
    }
    if (const int n = matrixSize(type)) {
        float m[16];
        parseMatrix(n, v.text, m);
        auto take = [&](auto const &M, int size) {
            if (size == n)
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j)
                        m[i * n + j] = float(M[i][j]);
        };
        if (value.IsHolding<GfMatrix3d>())
            take(value.UncheckedGet<GfMatrix3d>(), 3);
        else if (value.IsHolding<GfMatrix3f>())
            take(value.UncheckedGet<GfMatrix3f>(), 3);
        else if (value.IsHolding<GfMatrix4d>())
            take(value.UncheckedGet<GfMatrix4d>(), 4);
        else if (value.IsHolding<GfMatrix4f>())
            take(value.UncheckedGet<GfMatrix4f>(), 4);
        v.rows = MatrixRows(m, n, true);
        return v;
    }
    const int w = typeWidth(type);
    if (w == 0)
        return v;
    float f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> data;
    int dataWidth = 0;
    if (HdNovaPrimvarToFloats(value, &dataWidth, &data) && !data.empty()) {
        for (int k = 0; k < w; ++k)
            f[k] = dataWidth == 1 ? data[0] : (k < dataWidth && size_t(k) < data.size() ? data[size_t(k)] : 0.0f);
    } else {
        parseFloats(type, v.text, f);
    }
    v.ref = Param(f, w);
    return v;
}

// The value of a user node's input: connected, authored (a parameter), or
// the nodedef default (a parameter, or a geometry read).
Value Translator::InputOf(SdfPath const &path, HdMaterialNode2 const &node, mx::InputPtr const &input) {
    const TfToken name(input->getName());
    const int w = typeWidth(input->getType());
    const auto conn = node.inputConnections.find(name);
    if (conn != node.inputConnections.end() && !conn->second.empty()) {
        HdMaterialConnection2 const &c = conn->second.front();
        Value v = PickOutput(LowerUser(c.upstreamNode), c.upstreamOutputName.GetString());
        if (const size_t n = size_t(matrixSize(input->getType())); n > 0 && v.rows.size() != n)
            return DefaultValue(input, true); // unknown upstream node
        if (v.ref != kNoRef && w > 0)
            v.ref = _b.convert(v.ref, w);
        else if (v.ref == kNoRef && w > 0)
            v.ref = Zero(w); // unknown upstream node
        return v;
    }
    const auto p = node.parameters.find(name);
    if (p != node.parameters.end()) {
        Value v = UserValue(input->getType(), enumToValue(input, p->second));
        const auto meta = _src.meta.find(path);
        if (meta != _src.meta.end()) {
            const auto m = meta->second.find(name);
            if (m != meta->second.end())
                v.colorSpace = m->second.colorSpace.GetString();
        }
        return v;
    }
    return DefaultValue(input, true);
}

ValueMap Translator::LowerUser(SdfPath const &path) {
    const auto memo = _userOutputs.find(path);
    if (memo != _userOutputs.end())
        return memo->second;
    const auto it = _src.net.nodes.find(path);
    if (it == _src.net.nodes.end() || !_visiting.insert(path).second) {
        Warn("missing:" + path.GetString(), "node '" + path.GetString() + "' is missing or part of a cycle");
        return {};
    }
    HdMaterialNode2 const &node = it->second;
    ValueMap out;
    const mx::NodeDefPtr nd = ResolveNodeDef(node.nodeTypeId);
    if (node.nodeTypeId == "kma_aov") {
        // Karma's AOV node 1.0: its output, named after the AOV, passes its
        // input through (the AOV itself is M6).
        const auto c = node.inputConnections.find(TfToken("input"));
        if (c != node.inputConnections.end() && !c->second.empty())
            out["out"] = PickOutput(LowerUser(c->second.front().upstreamNode), c->second.front().upstreamOutputName.GetString());
    } else if (!nd) {
        Warn("unknown:" + node.nodeTypeId.GetString(),
             "unknown node type '" + node.nodeTypeId.GetString() + "' (" + path.GetString() + "); using 0");
    } else {
        ValueMap in;
        for (mx::InputPtr const &input : nd->getActiveInputs())
            in[input->getName()] = InputOf(path, node, input);
        // Authored parameters the definition lacks, as text (UsdUVTexture's
        // sourceColorSpace).
        for (auto const &[name, value] : node.parameters)
            if (!in.count(name.GetString()) && name.GetString().find(':') == std::string::npos)
                in[name.GetString()].text = toText(value);
        LowerNode(nd, in, out, 0);
    }
    _visiting.erase(path);
    _userOutputs[path] = out;
    return out;
}

void Translator::LowerNode(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out, int depth) {
    if (LowerNative(nd, in, out))
        return;
    if (depth < 48 && LowerGraph(nd, in, out, depth))
        return;
    Warn("unsupported:" + nd->getName(),
         "node '" + nd->getName() + "' is not supported yet; using its default outputs");
    DefaultOutputs(nd, out);
}

void Translator::DefaultOutputs(mx::NodeDefPtr const &nd, ValueMap &out) {
    for (mx::OutputPtr const &o : nd->getActiveOutputs()) {
        const int w = typeWidth(o->getType());
        Value v;
        if (w > 0) {
            float f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            parseFloats(o->getType(), o->getValueString(), f);
            v.ref = _b.constant(f, w);
        }
        out[o->getName()] = v;
    }
}

bool Translator::LowerGraph(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out, int depth) {
    mx::InterfaceElementPtr impl = nd->getImplementation();
    mx::NodeGraphPtr graph = impl ? impl->asA<mx::NodeGraph>() : nullptr;
    if (!graph)
        return false;
    // Karma's karma_mikkt on a graph (e.g. hmtlxpbrtextureset) reaches the
    // normal maps inside it.
    const auto mikkt = in.find("karma_mikkt");
    const bool mikktHere = mikkt != in.end() && Enabled(mikkt->second.text);
    struct MikktScope {
        int &flag;
        bool on;
        ~MikktScope() {
            if (on)
                --flag;
        }
    } scope{_mikkt, mikktHere};
    if (mikktHere)
        ++_mikkt;
    auto interfaceValue = [&](std::string const &name) -> Value {
        const auto it = in.find(name);
        if (it != in.end())
            return it->second;
        if (mx::InputPtr di = nd->getActiveInput(name))
            return DefaultValue(di, false);
        return {};
    };
    std::map<std::string, ValueMap> nodeOutputs;
    for (mx::ElementPtr const &e : graph->topologicalSort()) {
        mx::NodePtr node = e->asA<mx::Node>();
        if (!node)
            continue;
        mx::NodeDefPtr cnd = node->getNodeDef();
        ValueMap &cout = nodeOutputs[node->getName()];
        if (!cnd) {
            Warn("internal:" + node->getCategory(), "node '" + node->getCategory() + "' inside '" + nd->getName() +
                                                        "' has no definition; using 0");
            continue;
        }
        ValueMap cin;
        for (mx::InputPtr const &di : cnd->getActiveInputs()) {
            mx::InputPtr ni = node->getInput(di->getName());
            const int w = typeWidth(di->getType());
            Value v;
            if (ni && !ni->getInterfaceName().empty()) {
                v = interfaceValue(ni->getInterfaceName());
            } else if (ni && !ni->getNodeName().empty()) {
                v = PickOutput(nodeOutputs[ni->getNodeName()], ni->getOutputString());
            } else if (ni && ni->hasValue()) {
                v.text = ni->getValueString();
                float f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (w > 0) {
                    parseFloats(di->getType(), v.text, f);
                    v.ref = _b.constant(f, w);
                }
            } else {
                v = DefaultValue(di, false);
            }
            if (w > 0)
                v.ref = v.ref != kNoRef ? _b.convert(v.ref, w) : Zero(w);
            if (const int n = matrixSize(di->getType()); n > 0 && v.rows.size() != size_t(n)) {
                float m[16];
                parseMatrix(n, ni && ni->hasValue() ? ni->getValueString() : di->getValueString(), m);
                v.rows = MatrixRows(m, n, false);
            }
            cin[di->getName()] = v;
        }
        LowerNode(cnd, cin, cout, depth + 1);
    }
    for (mx::OutputPtr const &o : graph->getActiveOutputs()) {
        Value v;
        if (!o->getInterfaceName().empty())
            v = interfaceValue(o->getInterfaceName());
        else if (!o->getNodeName().empty())
            v = PickOutput(nodeOutputs[o->getNodeName()], o->getOutputString());
        // The definition's type wins: Houdini's kma_camerafalloff graph
        // declares its color3 output as float (Karma follows the nodedef).
        const mx::OutputPtr declared = nd->getActiveOutput(o->getName());
        const int w = typeWidth(declared ? declared->getType() : o->getType());
        if (w > 0)
            v.ref = v.ref != kNoRef ? _b.convert(v.ref, w) : Zero(w);
        out[o->getName()] = v;
    }
    return true;
}

// Matrix nodes (MaterialX row vectors: v' = v M; a product A B applies A
// first). Matrices are n rows of width n; most are uniform and fold or run in
// the prologue.
bool Translator::LowerMatrix(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out) {
    const std::vector<mx::OutputPtr> outputs = nd->getActiveOutputs();
    if (outputs.size() != 1)
        return false;
    int n = matrixSize(outputs[0]->getType());
    for (mx::InputPtr const &i : nd->getActiveInputs())
        n = std::max(n, matrixSize(i->getType()));
    if (n == 0)
        return false;
    const std::string category = nd->getNodeString();
    auto isMatrix = [&](char const *name) {
        mx::InputPtr i = nd->getActiveInput(name);
        return i && matrixSize(i->getType()) == n;
    };
    auto mat = [&](char const *name) {
        const auto it = in.find(name);
        if (it != in.end() && it->second.rows.size() == size_t(n))
            return it->second.rows;
        float m[16];
        parseMatrix(n, "", m);
        return MatrixRows(m, n, false);
    };
    auto vec = [&](char const *name, int width) {
        const auto it = in.find(name);
        return it == in.end() || it->second.ref == kNoRef ? Zero(width) : _b.convert(it->second.ref, width);
    };
    auto setMat = [&](std::vector<Ref> rows) {
        Value v;
        v.rows = std::move(rows);
        out[outputs[0]->getName()] = v;
        return true;
    };
    auto setVec = [&](Ref r) {
        Value v;
        v.ref = r;
        out[outputs[0]->getName()] = v;
        return true;
    };
    auto c = [&](Ref r, int k) { return _b.component(r, k); };
    // v M for a row vector v of width n.
    auto transform = [&](Ref v, std::vector<Ref> const &M) {
        Ref r = _b.mul(c(v, 0), M[0]);
        for (int i = 1; i < n; ++i)
            r = _b.add(r, _b.mul(c(v, i), M[size_t(i)]));
        return r;
    };
    auto product = [&](std::vector<Ref> const &A, std::vector<Ref> const &B) {
        std::vector<Ref> r;
        for (int i = 0; i < n; ++i)
            r.push_back(transform(A[size_t(i)], B));
        return r;
    };
    auto transpose = [&](std::vector<Ref> const &A) {
        std::vector<Ref> r;
        for (int j = 0; j < n; ++j) {
            Ref row = c(A[0], j);
            for (int i = 1; i < n; ++i)
                row = _b.concat(row, c(A[size_t(i)], j));
            r.push_back(row);
        }
        return r;
    };
    // Determinant and inverse (adjugate over the determinant; a singular
    // matrix inverts to 0 under safe division).
    auto inverse = [&](std::vector<Ref> const &A, Ref *detOut) {
        std::vector<Ref> r;
        if (n == 3) {
            const Ref c0 = _b.cross(A[1], A[2]), c1 = _b.cross(A[2], A[0]), c2 = _b.cross(A[0], A[1]);
            const Ref det = _b.dot(A[0], c0);
            if (detOut)
                *detOut = det;
            for (int i = 0; i < 3; ++i)
                r.push_back(_b.div(_b.concat(_b.concat(c(c0, i), c(c1, i)), c(c2, i)), det));
            return r;
        }
        Ref a[4][4];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                a[i][j] = c(A[size_t(i)], j);
        auto m2 = [&](Ref p, Ref q, Ref s, Ref t) { return _b.sub(_b.mul(p, q), _b.mul(s, t)); };
        const Ref s0 = m2(a[0][0], a[1][1], a[1][0], a[0][1]), s1 = m2(a[0][0], a[1][2], a[1][0], a[0][2]);
        const Ref s2 = m2(a[0][0], a[1][3], a[1][0], a[0][3]), s3 = m2(a[0][1], a[1][2], a[1][1], a[0][2]);
        const Ref s4 = m2(a[0][1], a[1][3], a[1][1], a[0][3]), s5 = m2(a[0][2], a[1][3], a[1][2], a[0][3]);
        const Ref c5 = m2(a[2][2], a[3][3], a[3][2], a[2][3]), c4 = m2(a[2][1], a[3][3], a[3][1], a[2][3]);
        const Ref c3 = m2(a[2][1], a[3][2], a[3][1], a[2][2]), c2 = m2(a[2][0], a[3][3], a[3][0], a[2][3]);
        const Ref c1 = m2(a[2][0], a[3][2], a[3][0], a[2][2]), c0 = m2(a[2][0], a[3][1], a[3][0], a[2][1]);
        const Ref det = _b.add(_b.add(_b.sub(_b.mul(s0, c5), _b.mul(s1, c4)), _b.add(_b.mul(s2, c3), _b.mul(s3, c2))),
                               _b.sub(_b.mul(s5, c0), _b.mul(s4, c1)));
        if (detOut)
            *detOut = det;
        // e(x, p, y, q, z, r) = x p - y q + z r
        auto e = [&](Ref x, Ref p, Ref y, Ref q, Ref z, Ref s) {
            return _b.add(_b.sub(_b.mul(x, p), _b.mul(y, q)), _b.mul(z, s));
        };
        auto neg = [&](Ref x) { return _b.unary(Op::Neg, x); };
        const Ref b[4][4] = {
            {e(a[1][1], c5, a[1][2], c4, a[1][3], c3), neg(e(a[0][1], c5, a[0][2], c4, a[0][3], c3)),
             e(a[3][1], s5, a[3][2], s4, a[3][3], s3), neg(e(a[2][1], s5, a[2][2], s4, a[2][3], s3))},
            {neg(e(a[1][0], c5, a[1][2], c2, a[1][3], c1)), e(a[0][0], c5, a[0][2], c2, a[0][3], c1),
             neg(e(a[3][0], s5, a[3][2], s2, a[3][3], s1)), e(a[2][0], s5, a[2][2], s2, a[2][3], s1)},
            {e(a[1][0], c4, a[1][1], c2, a[1][3], c0), neg(e(a[0][0], c4, a[0][1], c2, a[0][3], c0)),
             e(a[3][0], s4, a[3][1], s2, a[3][3], s0), neg(e(a[2][0], s4, a[2][1], s2, a[2][3], s0))},
            {neg(e(a[1][0], c3, a[1][1], c1, a[1][2], c0)), e(a[0][0], c3, a[0][1], c1, a[0][2], c0),
             neg(e(a[3][0], s3, a[3][1], s1, a[3][2], s0)), e(a[2][0], s3, a[2][1], s1, a[2][2], s0)},
        };
        for (int i = 0; i < 4; ++i)
            r.push_back(_b.div(_b.concat(_b.concat(b[i][0], b[i][1]), _b.concat(b[i][2], b[i][3])), det));
        return r;
    };
    auto rowWise = [&](Op op) {
        const std::vector<Ref> A = mat("in1");
        std::vector<Ref> r;
        if (isMatrix("in2")) {
            const std::vector<Ref> B = mat("in2");
            for (int i = 0; i < n; ++i)
                r.push_back(_b.binary(op, A[size_t(i)], B[size_t(i)]));
        } else {
            const Ref s = vec("in2", 1);
            for (int i = 0; i < n; ++i)
                r.push_back(_b.binary(op, A[size_t(i)], s));
        }
        return setMat(r);
    };

    if (category == "constant")
        return setMat(mat("value"));
    if (category == "dot")
        return setMat(mat("in"));
    if (category == "add")
        return rowWise(Op::Add);
    if (category == "subtract")
        return rowWise(Op::Sub);
    if (category == "multiply")
        return isMatrix("in2") ? setMat(product(mat("in1"), mat("in2"))) : rowWise(Op::Mul);
    if (category == "divide")
        return isMatrix("in2") ? setMat(product(mat("in1"), inverse(mat("in2"), nullptr))) : rowWise(Op::Div);
    if (category == "transpose")
        return setMat(transpose(mat("in")));
    if (category == "invertmatrix")
        return setMat(inverse(mat("in"), nullptr));
    if (category == "determinant") {
        Ref det = kNoRef;
        inverse(mat("in"), &det);
        return setVec(det);
    }
    if (category == "creatematrix") {
        mx::InputPtr i1 = nd->getActiveInput("in1");
        const int wi = i1 ? typeWidth(i1->getType()) : 3;
        std::vector<Ref> rows;
        for (int i = 0; i < n; ++i) {
            const std::string name = "in" + std::to_string(i + 1);
            Ref row = vec(name.c_str(), wi);
            if (wi < n) // vector3 rows of a matrix44: (x, y, z, 0), the last (x, y, z, 1)
                row = _b.concat(row, _b.constant(i == n - 1 ? 1.0f : 0.0f));
            rows.push_back(row);
        }
        return setMat(rows);
    }
    if (category == "extract") {
        const auto it = in.find("index");
        const int index = std::max(0, std::min(n - 1, std::atoi(it != in.end() ? it->second.text.c_str() : "0")));
        return setVec(mat("in")[size_t(index)]);
    }
    if (category == "transformmatrix") {
        const int wv = typeWidth(outputs[0]->getType());
        const Ref v = vec("in", wv);
        const std::vector<Ref> M = mat("mat");
        if (wv == n)
            return setVec(transform(v, M));
        // vector2 with a matrix33, vector3 with a matrix44: a point (w = 1)
        return setVec(_b.convert(transform(_b.concat(v, _b.constant(1.0f)), M), wv));
    }
    if (category == "ifgreater" || category == "ifgreatereq" || category == "ifequal") {
        const Op cmp = category == "ifgreater" ? Op::Greater : category == "ifgreatereq" ? Op::GreaterEq : Op::Equal;
        const auto it = in.find("value1");
        const int wv = it != in.end() && it->second.ref != kNoRef ? _b.width(it->second.ref) : 1;
        const Ref cond = _b.binary(cmp, vec("value1", wv), vec("value2", wv));
        const std::vector<Ref> A = mat("in1"), B = mat("in2");
        std::vector<Ref> r;
        for (int i = 0; i < n; ++i)
            r.push_back(_b.ternary(Op::Select, cond, A[size_t(i)], B[size_t(i)]));
        return setMat(r);
    }
    return false;
}

// Image textures: MaterialX image (color3/color4 signatures in Karma's
// "auto" color space unless the file input names one; float and vector
// signatures raw) and UsdUVTexture (sourceColorSpace raw / sRGB / auto,
// result * scale + bias). An empty file gives the fallback.
bool Translator::LowerTexture(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out) {
    const std::string category = nd->getNodeString();
    if (category != "image" && category != "UsdUVTexture")
        return false;
    auto text = [&](char const *name) {
        const auto it = in.find(name);
        return it != in.end() ? it->second.text : std::string();
    };
    auto get = [&](char const *name, int width) {
        const auto it = in.find(name);
        return it == in.end() || it->second.ref == kNoRef ? Zero(width) : _b.convert(it->second.ref, width);
    };
    auto wrapOf = [&](std::string const &mode) {
        if (mode == "clamp")
            return nova::TexWrap::Clamp;
        if (mode == "mirror")
            return nova::TexWrap::Mirror;
        if (mode == "constant")
            return nova::TexWrap::Constant;
        if (mode == "black" || mode == "useMetadata") // converted files carry black wrap metadata
            return nova::TexWrap::Black;
        return nova::TexWrap::Periodic; // periodic, repeat
    };
    const auto fileIt = in.find("file");
    HdNovaTextureRequest request;
    request.path = fileIt != in.end() ? fileIt->second.text : std::string();
    const std::string fileColorSpace = fileIt != in.end() ? fileIt->second.colorSpace : std::string();
    nova::TexSlot slot;
    if (category == "image") {
        const std::vector<mx::OutputPtr> outputs = nd->getActiveOutputs();
        if (outputs.size() != 1)
            return false;
        const std::string type = outputs[0]->getType();
        const int w = typeWidth(type);
        if (w <= 0)
            return false;
        const Ref fallback = get("default", w);
        Value v;
        if (request.path.empty()) {
            v.ref = fallback;
        } else {
            const bool color = type == "color3" || type == "color4";
            request.colorSpace = color ? fileColorSpace : std::string("raw");
            request.layer = text("layer");
            slot.wrapS = wrapOf(text("uaddressmode"));
            slot.wrapT = wrapOf(text("vaddressmode"));
            const std::string filter = text("filtertype");
            slot.filter = filter == "closest" ? nova::TexFilter::Closest
                          : filter == "cubic" ? nova::TexFilter::Cubic
                                              : nova::TexFilter::Linear;
            slot.channels = uint8_t(w);
            v.ref = _b.tex2D(TextureSlot(request, slot), get("texcoord", 2), fallback);
        }
        out[outputs[0]->getName()] = v;
        return true;
    }
    // UsdUVTexture
    const Ref fallback = get("fallback", 4);
    Ref rgba = fallback;
    if (!request.path.empty()) {
        const std::string source = text("sourceColorSpace");
        request.colorSpace = source == "raw" ? std::string("raw")
                             : source == "sRGB" ? std::string("srgb_texture")
                                                : fileColorSpace;
        slot.wrapS = wrapOf(text("wrapS"));
        slot.wrapT = wrapOf(text("wrapT"));
        slot.channels = 4;
        rgba = _b.tex2D(TextureSlot(request, slot), get("st", 2), fallback);
    }
    rgba = _b.add(_b.mul(rgba, get("scale", 4)), get("bias", 4));
    const char *names[4] = {"r", "g", "b", "a"};
    for (int k = 0; k < 4; ++k) {
        Value v;
        v.ref = _b.component(rgba, k);
        out[names[k]] = v;
    }
    Value rgb;
    rgb.ref = _b.swizzle(rgba, 0, 1, 2);
    out["rgb"] = rgb;
    Value all;
    all.ref = rgba;
    out["rgba"] = all;
    return true;
}

// Hex tiling (MaterialX hextiledimage / hextilednormalmap as Karma renders
// them; Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 11(2) 2022): the
// texture coordinate on a triangle grid; per grid vertex a hash of its id
// (Karma's: fmod keeps the sign) rotates, scales and offsets the lookup; the
// three lookups blend with weights bary^7 (times luminance) and an optional
// Schlick gain. The lookups filter with the unrotated coordinate's
// derivatives times the average scale, as Karma's. Normal maps rotate their
// tangent frame by -angle about N (strength cancels in Karma's
// orthonormalization) and blend surface gradients.
//
// Karma's own kma_color_hextiling / kma_normalmap_hextiling (native in Karma,
// its hash and parameter mapping are not public) use the same core: tiles of
// `size`, rotation rot + rand_rot (turns), scale `scale` up to (1 +
// rand_scale), the MaterialX offsets, contrast as the falloff, contrast_falloff
// as the luminance contrast, weightexp as the exponent and `seed` in the
// hash: the same look, another random tile layout (docs/design/M4.md notes
// on M4.3).
bool Translator::LowerHexTile(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out) {
    const std::string category = nd->getNodeString();
    const bool kma = category == "kma_color_hextiling" || category == "kma_normalmap_hextiling";
    const bool image = category == "hextiledimage" || category == "kma_color_hextiling";
    const bool normalMap = category == "hextilednormalmap" || category == "kma_normalmap_hextiling";
    if (!image && !normalMap)
        return false;
    const std::vector<mx::OutputPtr> outputs = nd->getActiveOutputs();
    const int w = typeWidth(outputs.empty() ? std::string() : outputs[0]->getType());
    auto get = [&](char const *name, int width) {
        const auto it = in.find(name);
        return it == in.end() || it->second.ref == kNoRef ? Zero(width) : _b.convert(it->second.ref, width);
    };
    auto text = [&](char const *name) {
        const auto it = in.find(name);
        return it != in.end() ? it->second.text : std::string();
    };
    // Karma's registry gives kma inputs zero defaults where Karma reads the
    // geometry: an unset input is a zero constant.
    auto unset = [&](char const *name) {
        const auto it = in.find(name);
        if (it == in.end() || it->second.ref == kNoRef)
            return true;
        sh::IrNode const &n = _out.graph.nodes[it->second.ref];
        if (n.op != Op::Const)
            return false;
        for (int k = 0; k < n.width; ++k)
            if (_out.graph.constants[n.imm + uint32_t(k)] != 0.0f)
                return false;
        return true;
    };
    auto num = [&](float x) { return _b.constant(x); };
    const Ref one = num(1.0f);

    Value result, weights;
    const auto fileIt = in.find("file");
    HdNovaTextureRequest request;
    request.path = fileIt != in.end() ? fileIt->second.text : std::string();
    if (request.path.empty()) {
        result.ref = kma ? (normalMap ? _b.normalize(_b.global(Global::N)) : Zero(w)) : get("default", w);
        weights.ref = _b.constant(1.0f, 0.0f, 0.0f);
        out[outputs[0]->getName()] = result;
        out["weights"] = weights;
        return true;
    }
    if (kma && image) {
        const std::string source = text("sourceColorSpace");
        request.colorSpace = source == "raw" || source == "linear" ? std::string("raw")
                             : source == "sRGB" || source == "srgb" ? std::string("srgb_texture")
                                                                    : fileIt->second.colorSpace;
    } else {
        request.colorSpace = image ? fileIt->second.colorSpace : std::string("raw");
    }
    nova::TexSlot slot;
    slot.wrapS = slot.wrapT = nova::TexWrap::Periodic;
    slot.channels = uint8_t(image ? (kma ? 4 : w) : 3);
    const uint32_t tex = TextureSlot(request, slot);

    // Parameters of the core.
    Ref coord, rotation, rotRange, scale, scaleRange, offset, offsetRange, falloff, contrast, luma, exponent;
    float seedOffset = 0.12345f;
    if (kma) {
        const Ref texcoord = unset("texcoord") ? GeomProp("UV0", 2) : get("texcoord", 2);
        coord = _b.div(texcoord, _b.binary(Op::Max, get("size", 1), num(1e-6f)));
        const Ref turns = get("rot", 1);
        rotation = one;
        rotRange = _b.mul(_b.concat(turns, _b.add(turns, get("rand_rot", 1))), num(6.28318530718f));
        scale = one;
        scaleRange = _b.concat(get("scale", 1), _b.mul(get("scale", 1), _b.add(one, get("rand_scale", 1))));
        offset = one;
        offsetRange = _b.concat(Zero(1), one);
        falloff = get("contrast", 1);
        contrast = get("contrast_falloff", 1);
        luma = _b.constant(0.2722287f, 0.6740818f, 0.0536895f);
        exponent = get("weightexp", 1);
        seedOffset += 0.6180339f * float(std::atoi(text("seed").c_str()));
    } else {
        coord = _b.mul(get("texcoord", 2), get("tiling", 2));
        rotation = get("rotation", 1);
        rotRange = _b.mul(get("rotationrange", 2), num(0.01745329252f));
        scale = get("scale", 1);
        scaleRange = get("scalerange", 2);
        offset = get("offset", 1);
        offsetRange = get("offsetrange", 2);
        falloff = get("falloff", 1);
        contrast = image ? get("falloffcontrast", 1) : one;
        luma = image ? get("lumacoeffs", 3) : one;
        exponent = num(7.0f);
    }

    auto fmod1 = [&](Ref x) { return _b.sub(x, _b.unary(Op::Trunc, x)); }; // Karma's _mx_fmod(x, 1)
    auto mix = [&](Ref a, Ref b, Ref t) { return _b.ternary(Op::Mix, a, b, t); };
    auto c = [&](Ref v, int k) { return _b.component(v, k); };
    const float sqrt3x2 = 3.46410162f;
    const Ref st = _b.mul(coord, num(sqrt3x2));
    const Ref skx = _b.add(c(st, 0), _b.mul(c(st, 1), num(-0.57735027f))), sky = _b.mul(c(st, 1), num(1.15470054f));
    const Ref frx = _b.unary(Op::Fract, skx), fry = _b.unary(Op::Fract, sky);
    const Ref tz = _b.sub(_b.sub(one, frx), fry);
    const Ref sf = _b.ternary(Op::Select, _b.binary(Op::Greater, tz, Zero(1)), Zero(1), one);
    const Ref s2 = _b.sub(_b.mul(sf, num(2.0f)), one);
    const Ref tw[3] = {_b.mul(_b.unary(Op::Neg, tz), s2), _b.sub(sf, _b.mul(fry, s2)), _b.sub(sf, _b.mul(frx, s2))};
    const Ref bx = _b.unary(Op::Floor, skx), by = _b.unary(Op::Floor, sky), isf = _b.sub(one, sf);
    const Ref ids[3][2] = {{_b.add(bx, sf), _b.add(by, sf)},
                           {_b.add(bx, sf), _b.add(by, isf)},
                           {_b.add(bx, isf), _b.add(by, sf)}};
    Ref tc[3], rot[3];
    for (int i = 0; i < 3; ++i) {
        const Ref ix = ids[i][0], iy = ids[i][1];
        const Ref ctr = _b.mul(_b.concat(_b.add(ix, _b.mul(iy, num(0.5f))), _b.div(iy, num(1.15470054f))),
                               num(1.0f / sqrt3x2));
        // Karma's hash of the id (+ 0.12345).
        const Ref px = _b.add(ix, num(seedOffset)), py = _b.add(iy, num(seedOffset));
        const Ref a = fmod1(_b.mul(_b.concat(_b.concat(px, py), px), _b.constant(0.1031f, 0.1030f, 0.0973f)));
        const Ref a2 = _b.add(a, _b.dot(a, _b.add(_b.swizzle(a, 1, 2, 0), num(33.33f))));
        const Ref rnd = fmod1(_b.mul(_b.concat(_b.add(c(a2, 0), c(a2, 1)), _b.add(c(a2, 0), c(a2, 2))),
                                     _b.concat(c(a2, 2), c(a2, 1))));
        rot[i] = mix(c(rotRange, 0), c(rotRange, 1), _b.mul(c(rnd, 0), rotation));
        const Ref sc = mix(one, mix(c(scaleRange, 0), c(scaleRange, 1), c(rnd, 1)), scale);
        const Ref off = _b.concat(mix(c(offsetRange, 0), c(offsetRange, 1), _b.mul(c(rnd, 0), offset)),
                                  mix(c(offsetRange, 0), c(offsetRange, 1), _b.mul(c(rnd, 1), offset)));
        const Ref d = _b.sub(coord, ctr), cs = _b.unary(Op::Cos, rot[i]), sn = _b.unary(Op::Sin, rot[i]);
        const Ref turned = _b.concat(_b.sub(_b.mul(c(d, 0), cs), _b.mul(c(d, 1), sn)),
                                     _b.add(_b.mul(c(d, 0), sn), _b.mul(c(d, 1), cs)));
        tc[i] = _b.add(_b.add(_b.div(turned, sc), ctr), off);
    }
    // Footprint: the coordinate's derivatives times the average scale.
    const Ref avg = _b.mul(mix(one, _b.mul(_b.add(c(scaleRange, 0), c(scaleRange, 1)), num(0.5f)), scale), num(0.5f));
    const Ref ds = _b.mul(_b.deriv(c(coord, 0)), avg), dt = _b.mul(_b.deriv(c(coord, 1)), avg);
    const Ref derivs = _b.concat(_b.concat(c(ds, 0), c(dt, 0)), _b.concat(c(ds, 1), c(dt, 1)));
    const int lookupWidth = slot.channels;
    const Ref fallback = kma ? Zero(lookupWidth) : get("default", lookupWidth);
    Ref looked[3];
    for (int i = 0; i < 3; ++i)
        looked[i] = _b.tex2D(tex, tc[i], derivs, fallback);

    // Blend weights: lum bary^exponent normalized, then Schlick's gain unless
    // the falloff is 0.5.
    auto blend = [&](Ref const *lum) {
        Ref wt[3], sum;
        for (int i = 0; i < 3; ++i) {
            wt[i] = _b.mul(lum[i], _b.binary(Op::Pow, tw[i], exponent));
            sum = i == 0 ? wt[0] : _b.add(sum, wt[i]);
        }
        Ref g[3], gsum;
        const Ref rr = _b.ternary(Op::Clamp, falloff, num(0.001f), num(0.999f));
        for (int i = 0; i < 3; ++i) {
            wt[i] = _b.div(wt[i], sum);
            const Ref a = _b.mul(_b.sub(_b.div(one, rr), num(2.0f)), _b.sub(one, _b.mul(wt[i], num(2.0f))));
            g[i] = _b.ternary(Op::Select, _b.binary(Op::Greater, num(0.5f), wt[i]), _b.div(wt[i], _b.add(a, one)),
                              _b.div(_b.sub(a, wt[i]), _b.sub(a, one)));
            gsum = i == 0 ? g[0] : _b.add(gsum, g[i]);
        }
        const Ref gain = _b.unary(Op::Not, _b.binary(Op::Equal, falloff, num(0.5f)));
        for (int i = 0; i < 3; ++i)
            wt[i] = _b.ternary(Op::Select, gain, _b.div(g[i], gsum), wt[i]);
        return std::array<Ref, 3>{wt[0], wt[1], wt[2]};
    };
    const Ref ones[3] = {one, one, one};
    if (image) {
        Ref lum[3];
        for (int i = 0; i < 3; ++i)
            lum[i] = mix(one, _b.dot(_b.swizzle(looked[i], 0, 1, 2), luma), contrast);
        const auto wc = blend(lum);
        Ref rgb = _b.mul(_b.swizzle(looked[0], 0, 1, 2), wc[0]);
        for (int i = 1; i < 3; ++i)
            rgb = _b.add(rgb, _b.mul(_b.swizzle(looked[i], 0, 1, 2), wc[i]));
        if (lookupWidth == 4) {
            const auto wa = blend(ones);
            Ref alpha = _b.mul(c(looked[0], 3), wa[0]);
            for (int i = 1; i < 3; ++i)
                alpha = _b.add(alpha, _b.mul(c(looked[i], 3), wa[i]));
            rgb = _b.concat(rgb, alpha);
        }
        result.ref = rgb;
        weights.ref = _b.concat(_b.concat(wc[0], wc[1]), wc[2]);
    } else {
        const Ref N = _b.normalize(kma && unset("normal") ? _b.global(Global::N) : get("normal", 3));
        const Ref T = kma && unset("tangent") ? KarmaT() : get("tangent", 3);
        const Ref B = kma && unset("bitangent") ? KarmaB() : get("bitangent", 3);
        const bool flip = text("flip_g") == "true" || text("flip_g") == "1";
        const bool objectSpace = kma && text("space") == "object";
        // Rodrigues about N by -angle.
        auto turn = [&](Ref v, Ref angle) {
            const Ref cs = _b.unary(Op::Cos, angle), sn = _b.unary(Op::Neg, _b.unary(Op::Sin, angle));
            return _b.add(_b.add(_b.mul(v, cs), _b.mul(_b.cross(N, v), sn)),
                          _b.mul(N, _b.mul(_b.dot(N, v), _b.sub(one, cs))));
        };
        const auto wn = blend(ones);
        Ref gg;
        for (int i = 0; i < 3; ++i) {
            Ref nn = looked[i];
            if (flip)
                nn = _b.concat(_b.concat(c(nn, 0), _b.sub(one, c(nn, 1))), c(nn, 2));
            nn = _b.sub(_b.mul(nn, num(2.0f)), one);
            if (kma) {
                // height (0.1: the map as it is) scales the slope; invert flips it.
                Ref k = _b.mul(get("height", 1), num(10.0f));
                if (text("invert") == "1")
                    k = _b.unary(Op::Neg, k);
                nn = _b.concat(_b.mul(_b.swizzle(nn, 0, 1), k), c(nn, 2));
            }
            Ref ni;
            if (objectSpace) {
                ni = _b.normalize(ObjectToWorldNormal(nn));
            } else {
                const Ref Ti = turn(T, rot[i]), Bi = turn(B, rot[i]);
                const Ref Tn = _b.normalize(_b.sub(Ti, _b.mul(N, _b.dot(Ti, N))));
                const Ref Bn = _b.normalize(_b.sub(_b.sub(Bi, _b.mul(N, _b.dot(Bi, N))), _b.mul(Tn, _b.dot(Bi, Tn))));
                ni = _b.normalize(_b.add(_b.add(_b.mul(Tn, c(nn, 0)), _b.mul(Bn, c(nn, 1))), _b.mul(N, c(nn, 2))));
            }
            const Ref dd = _b.dot(N, ni);
            const Ref g = _b.div(_b.sub(_b.mul(N, dd), ni), _b.binary(Op::Max, num(1e-8f), _b.unary(Op::Abs, dd)));
            const Ref term = _b.mul(g, _b.ternary(Op::Clamp, wn[size_t(i)], Zero(1), one));
            gg = i == 0 ? term : _b.add(gg, term);
        }
        result.ref = _b.normalize(_b.sub(N, gg));
        weights.ref = _b.concat(_b.concat(wn[0], wn[1]), wn[2]);
    }
    out[outputs[0]->getName()] = result;
    out["weights"] = weights;
    return true;
}

// pbrlib utility nodes with closed forms (Karma's mxpbr.h; blackbody as
// measured from Karma, core/volume/blackbody.h).
bool Translator::LowerPbrUtility(std::string const &category, ValueMap &in, ValueMap &out) {
    auto get = [&](char const *name, int width) {
        const auto it = in.find(name);
        return it == in.end() || it->second.ref == kNoRef ? Zero(width) : _b.convert(it->second.ref, width);
    };
    auto put = [&](char const *name, Ref r) {
        Value v;
        v.ref = r;
        out[name] = v;
    };
    auto k = [&](float x) { return _b.constant(x); };
    auto c = [&](Ref r, int i) { return _b.component(r, i); };
    auto clamp = [&](Ref x, float lo, float hi) { return _b.ternary(Op::Clamp, x, k(lo), k(hi)); };
    auto vec2 = [&](Ref x, Ref y) { return _b.concat(x, y); };
    const float eps = 1e-6f; // MX_FLOAT_EPS (mxpbr.h)
    if (category == "roughness_anisotropy") {
        const Ref r = get("roughness", 1), aniso = get("anisotropy", 1);
        const Ref r2 = clamp(_b.mul(r, r), eps, 1.0f);
        const Ref aspect = _b.unary(Op::Sqrt, _b.sub(k(1.0f), clamp(aniso, 0.0f, 0.98f)));
        const Ref aniso2 = vec2(_b.binary(Op::Min, _b.div(r2, aspect), k(1.0f)), _b.mul(r2, aspect));
        put("out", _b.ternary(Op::Select, _b.binary(Op::Greater, aniso, k(0.0f)), aniso2, vec2(r2, r2)));
        return true;
    }
    if (category == "roughness_dual") {
        const Ref r = get("roughness", 2);
        const Ref x = c(r, 0), y = c(r, 1);
        const Ref rx = clamp(_b.mul(x, x), eps, 1.0f);
        const Ref ry = _b.ternary(Op::Select, _b.binary(Op::Greater, k(0.0f), y), rx, clamp(_b.mul(y, y), eps, 1.0f));
        put("out", vec2(rx, ry));
        return true;
    }
    if (category == "artistic_ior") {
        const Ref r = clamp(get("reflectivity", 3), 0.0f, 0.99f), edge = get("edge_color", 3);
        const Ref rs = _b.unary(Op::Sqrt, r), one = k(1.0f);
        const Ref nmin = _b.div(_b.sub(one, r), _b.add(one, r));
        const Ref nmax = _b.div(_b.add(one, rs), _b.sub(one, rs));
        const Ref ior = _b.ternary(Op::Mix, nmax, nmin, edge);
        const Ref np1 = _b.add(ior, one), nm1 = _b.sub(ior, one);
        const Ref k2 = _b.div(_b.sub(_b.mul(_b.mul(np1, np1), r), _b.mul(nm1, nm1)), _b.sub(one, r));
        put("ior", ior);
        put("extinction", _b.unary(Op::Sqrt, k2));
        return true;
    }
    if (category == "deon_hair_absorption_from_melanin") {
        const Ref melanin = _b.unary(Op::Neg, _b.unary(Op::Log, _b.binary(Op::Max, _b.sub(k(1.0f), get("melanin_concentration", 1)), k(0.0001f))));
        const Ref red = clamp(get("melanin_redness", 1), 0.0f, 1.0f);
        const Ref eu = _b.mul(melanin, _b.sub(k(1.0f), red)), pheo = _b.mul(melanin, red);
        const Ref a = _b.add(_b.mul(eu, _b.unary(Op::Neg, _b.unary(Op::Log, get("eumelanin_color", 3)))),
                             _b.mul(pheo, _b.unary(Op::Neg, _b.unary(Op::Log, get("pheomelanin_color", 3)))));
        put("absorption", _b.binary(Op::Max, a, k(0.0f)));
        return true;
    }
    if (category == "chiang_hair_absorption_from_color") {
        const Ref b = clamp(get("azimuthal_roughness", 1), 0.0f, 1.0f);
        const Ref b2 = _b.mul(b, b), b4 = _b.mul(b2, b2);
        Ref fac = _b.sub(k(5.969f), _b.mul(k(0.215f), b));
        fac = _b.add(fac, _b.mul(k(2.532f), b2));
        fac = _b.sub(fac, _b.mul(k(10.73f), _b.mul(b2, b)));
        fac = _b.add(fac, _b.mul(k(5.574f), b4));
        fac = _b.add(fac, _b.mul(k(0.245f), _b.mul(b4, b)));
        const Ref sigma = _b.div(_b.unary(Op::Log, clamp(get("color", 3), 0.001f, 1.0f)), fac);
        put("absorption", _b.mul(sigma, sigma));
        return true;
    }
    if (category == "chiang_hair_roughness") {
        const Ref lr = clamp(get("longitudinal", 1), 0.001f, 1.0f), ar = clamp(get("azimuthal", 1), 0.001f, 1.0f);
        Ref v = _b.add(_b.add(_b.mul(k(0.726f), lr), _b.mul(k(0.812f), _b.mul(lr, lr))),
                       _b.mul(k(3.7f), _b.binary(Op::Pow, lr, k(20.0f))));
        v = _b.mul(v, v);
        const Ref s = _b.add(_b.add(_b.mul(k(0.265f), ar), _b.mul(k(1.194f), _b.mul(ar, ar))),
                             _b.mul(k(5.372f), _b.binary(Op::Pow, ar, k(22.0f))));
        const Ref tt = get("scale_TT", 1), trt = get("scale_TRT", 1);
        put("roughness_R", vec2(v, s));
        put("roughness_TT", vec2(_b.mul(v, _b.mul(tt, tt)), s));
        put("roughness_TRT", vec2(_b.mul(v, _b.mul(trt, trt)), s));
        return true;
    }
    if (category == "blackbody") { // Karma's, measured (docs/design/M8.md D14)
        put("out", _b.node(Op::Blackbody, 3, get("temperature", 1), kNoRef, kNoRef, sh::noiseImm(1, 3)));
        return true;
    }
    return false;
}

// Houdini's ramps (kma_ramp, kma_rampconst; VEX spline() semantics measured
// on Karma, docs/design/M4.md notes on M4.3): keys and values are material
// parameters (editable without recompiling), the key count and the bases
// structure. t outside the keys clamps to the end values. Each segment
// interpolates by its left key's basis:
//   constant, linear;
//   catmull-rom: cubic Hermite in the segment parameter with index-space
//   tangents (v[i+1] - v[i-1]) / 2, the end values duplicated (exact);
//   monotonecubic: the same with the average of the two neighbouring
//   differences where they agree in sign, else 0, and 0 at the ends (Karma
//   uses Kupan's slopes: within 0.006 on the probes);
//   bspline (when every key is bspline): the uniform cubic B-spline of the
//   points (key, value) with tripled end points, solved for x(u) = t (exact);
//   bezier (every key, 3k + 1 keys): cubic Bezier segments of the points
//   (key, value), solved likewise (exact);
//   hermite and other bezier ramps: linear, with a warning.
Ref Translator::Ramp(Ref t, Value const &keys, Value const &values, Value const &bases, int width) {
    const size_t n = std::min(keys.array.size(), values.arrayWidth > 0 ? values.array.size() / size_t(values.arrayWidth) : 0);
    if (n == 0)
        return Zero(width);
    std::vector<Ref> k(n), v(n);
    for (size_t i = 0; i < n; ++i) {
        k[i] = Param(&keys.array[i], 1);
        v[i] = _b.convert(Param(&values.array[i * size_t(values.arrayWidth)], values.arrayWidth), width);
    }
    if (n == 1)
        return v[0];
    auto basis = [&](size_t i) {
        return i < bases.strings.size() ? bases.strings[i] : std::string("linear");
    };
    const Ref zero = Zero(1), one = _b.constant(1.0f);
    auto num = [&](float x) { return _b.constant(x); };
    bool allBspline = true, allBezier = true;
    for (size_t i = 0; i < n; ++i) {
        allBspline &= basis(i) == "bspline";
        allBezier &= basis(i) == "bezier";
    }
    // A parametric cubic segment (x and y control points c[4]) solved for
    // x(u) = t by Newton from the linear guess between x0 and x1 (x is
    // monotone), returning y(u); `curve` evaluates the basis or its
    // derivative.
    auto solve = [&](Ref const *px, Ref const *py, Ref x0, Ref x1, auto const &curve) {
        Ref u = _b.ternary(Op::Clamp, _b.div(_b.sub(t, x0), _b.sub(x1, x0)), zero, one);
        for (int it = 0; it < 6; ++it) {
            const Ref f = _b.sub(curve(px, u, false), t), d = curve(px, u, true);
            u = _b.ternary(Op::Clamp, _b.sub(u, _b.div(f, d)), zero, one);
        }
        Ref r = curve(py, u, false);
        r = _b.ternary(Op::Select, _b.binary(Op::Greater, k[0], t), v[0], r);
        return _b.ternary(Op::Select, _b.binary(Op::GreaterEq, t, k[n - 1]), v[n - 1], r);
    };
    auto weighted = [&](Ref const *c, Ref b0, Ref b1, Ref b2, Ref b3) {
        return _b.add(_b.add(_b.mul(c[0], b0), _b.mul(c[1], b1)), _b.add(_b.mul(c[2], b2), _b.mul(c[3], b3)));
    };
    if (allBezier && n >= 4 && (n - 1) % 3 == 0) {
        auto bezier = [&](Ref const *c, Ref u, bool derivative) {
            const Ref w = _b.sub(one, u), u2 = _b.mul(u, u), w2 = _b.mul(w, w);
            if (!derivative)
                return weighted(c, _b.mul(w2, w), _b.mul(_b.mul(w2, u), num(3.0f)), _b.mul(_b.mul(w, u2), num(3.0f)),
                                _b.mul(u2, u));
            return weighted(c, _b.mul(w2, num(-3.0f)), _b.mul(_b.sub(w2, _b.mul(_b.mul(w, u), num(2.0f))), num(3.0f)),
                            _b.mul(_b.sub(_b.mul(_b.mul(w, u), num(2.0f)), u2), num(3.0f)), _b.mul(u2, num(3.0f)));
        };
        Ref P[4], Y[4];
        for (int j = 0; j < 4; ++j) {
            P[j] = k[size_t(j)];
            Y[j] = v[size_t(j)];
        }
        Ref x0 = k[0], x1 = k[3];
        for (size_t s = 3; s + 3 < n; s += 3) {
            const Ref in = _b.binary(Op::GreaterEq, t, k[s]);
            for (int j = 0; j < 4; ++j) {
                P[j] = _b.ternary(Op::Select, in, k[s + size_t(j)], P[j]);
                Y[j] = _b.ternary(Op::Select, in, v[s + size_t(j)], Y[j]);
            }
            x0 = _b.ternary(Op::Select, in, k[s], x0);
            x1 = _b.ternary(Op::Select, in, k[s + 3], x1);
        }
        return solve(P, Y, x0, x1, bezier);
    }

    if (allBspline) {
        // Control points with the ends tripled: Q[0..n+3]; segment s uses
        // Q[s..s+3] and starts at x = (Q[s] + 4 Q[s+1] + Q[s+2]) / 6.
        std::vector<Ref> qx, qy;
        auto q = [&](size_t i, std::vector<Ref> const &p) { return p[std::min(i < 2 ? 0 : i - 2, n - 1)]; };
        for (size_t i = 0; i < n + 4; ++i) {
            qx.push_back(q(i, k));
            qy.push_back(q(i, v));
        }
        const size_t segments = n + 1;
        auto start = [&](size_t s) {
            return _b.mul(_b.add(_b.add(qx[s], _b.mul(qx[s + 1], num(4.0f))), qx[s + 2]), num(1.0f / 6.0f));
        };
        Ref X0 = start(0), X1 = start(1), P[4], Y[4];
        for (int j = 0; j < 4; ++j) {
            P[j] = qx[size_t(j)];
            Y[j] = qy[size_t(j)];
        }
        for (size_t s = 1; s < segments; ++s) {
            const Ref in = _b.binary(Op::GreaterEq, t, start(s));
            const Ref nextStart = s + 1 < segments ? start(s + 1) : k[n - 1];
            X0 = _b.ternary(Op::Select, in, start(s), X0);
            X1 = _b.ternary(Op::Select, in, nextStart, X1);
            for (int j = 0; j < 4; ++j) {
                P[j] = _b.ternary(Op::Select, in, qx[s + size_t(j)], P[j]);
                Y[j] = _b.ternary(Op::Select, in, qy[s + size_t(j)], Y[j]);
            }
        }
        // Uniform cubic B-spline basis and its derivative at u.
        auto bspline = [&](Ref const *c, Ref u, bool derivative) {
            const Ref u2 = _b.mul(u, u), u3 = _b.mul(u2, u), w = _b.sub(one, u);
            if (!derivative)
                return weighted(c, _b.mul(_b.mul(_b.mul(w, w), w), num(1.0f / 6.0f)),
                                _b.mul(_b.add(_b.sub(_b.mul(u3, num(3.0f)), _b.mul(u2, num(6.0f))), num(4.0f)),
                                       num(1.0f / 6.0f)),
                                _b.mul(_b.add(_b.add(_b.add(_b.mul(u3, num(-3.0f)), _b.mul(u2, num(3.0f))),
                                                     _b.mul(u, num(3.0f))),
                                              one),
                                       num(1.0f / 6.0f)),
                                _b.mul(u3, num(1.0f / 6.0f)));
            return weighted(c, _b.mul(_b.mul(w, w), num(-0.5f)), _b.sub(_b.mul(u2, num(1.5f)), _b.mul(u, num(2.0f))),
                            _b.add(_b.add(_b.mul(u2, num(-1.5f)), u), num(0.5f)), _b.mul(u2, num(0.5f)));
        };
        return solve(P, Y, X0, X1, bspline);
    }

    // Index-space tangents (catmull-rom, monotonecubic).
    auto diff = [&](size_t i) { return _b.sub(v[i + 1], v[i]); }; // v[i+1] - v[i]
    std::vector<Ref> cr(n), mono(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t lo = i > 0 ? i - 1 : 0, hi = std::min(i + 1, n - 1);
        cr[i] = _b.mul(_b.sub(v[hi], v[lo]), num(0.5f));
        if (i == 0 || i == n - 1) {
            mono[i] = Zero(width);
        } else {
            const Ref d0 = diff(i - 1), d1 = diff(i);
            mono[i] = _b.ternary(Op::Select, _b.binary(Op::Greater, _b.mul(d0, d1), zero),
                                 _b.mul(_b.add(d0, d1), num(0.5f)), Zero(width));
        }
    }
    Ref r = v[0];
    for (size_t i = 0; i + 1 < n; ++i) {
        const std::string b = basis(i);
        const Ref u = _b.ternary(Op::Clamp, _b.div(_b.sub(t, k[i]), _b.sub(k[i + 1], k[i])), zero, one);
        Ref s;
        if (b == "constant") {
            s = v[i];
        } else if (b == "catmull-rom" || b == "monotonecubic" || b == "bspline") {
            const Ref u2 = _b.mul(u, u), u3 = _b.mul(u2, u);
            const Ref h01 = _b.sub(_b.mul(u2, num(3.0f)), _b.mul(u3, num(2.0f))), h00 = _b.sub(one, h01);
            const Ref h10 = _b.add(_b.sub(u3, _b.mul(u2, num(2.0f))), u), h11 = _b.sub(u3, u2);
            std::vector<Ref> const &m = b == "monotonecubic" ? mono : cr;
            s = _b.add(_b.add(_b.mul(v[i], h00), _b.mul(m[i], h10)), _b.add(_b.mul(v[i + 1], h01), _b.mul(m[i + 1], h11)));
            if (b == "bspline")
                Warn("ramp:mixedbspline", "a ramp mixing bspline with other bases: its bspline segments are approximated");
        } else {
            if (b != "linear")
                Warn("ramp:" + b, "ramp basis '" + b + "' is approximated as linear");
            s = _b.ternary(Op::Mix, v[i], v[i + 1], u);
        }
        r = _b.ternary(Op::Select, _b.binary(Op::GreaterEq, t, k[i]), s, r);
    }
    return _b.ternary(Op::Select, _b.binary(Op::GreaterEq, t, k[n - 1]), v[n - 1], r);
}

// Karma's ray variables (kma_rayimport, MaterialX dot nodes noted "ray:<var>";
// docs/research/m4/karma-nodes.md 3.3, 4.2), world space; kNoRef for those
// Nova does not have.
Ref Translator::RayVariable(std::string name) {
    if (TfStringStartsWith(name, "ray:"))
        name = name.substr(4);
    auto dir = [&] { return _b.normalize(_b.global(Global::I)); };
    auto facing = [&](Global n) { return _b.unary(Op::Abs, _b.dot(dir(), _b.normalize(_b.global(n)))); };
    if (name == "direction")
        return dir();
    if (name == "hitdist")
        return _b.global(Global::HitDist);
    if (name == "hitN")
        return _b.normalize(_b.global(Global::N));
    if (name == "hitNg")
        return _b.normalize(_b.global(Global::Ng));
    if (name == "hitN_facingratio")
        return facing(Global::N);
    if (name == "hitNg_facingratio")
        return facing(Global::Ng);
    if (name == "hitP")
        return _b.global(Global::P);
    if (name == "origin")
        return _b.sub(_b.global(Global::P), _b.mul(dir(), _b.global(Global::HitDist)));
    if (name == "isshadowray")
        return _b.binary(Op::Equal, _b.global(Global::RayType), _b.constant(float(nova::RayClass::Shadow)));
    if (name == "level")
        return _b.global(Global::RayDepth);
    if (name == "backface")
        return _b.global(Global::Backface);
    if (name == "primid")
        return _b.global(Global::PrimId);
    return kNoRef;
}

// Karma's ray switch and ray imports, and the dot convention for the latter.
bool Translator::LowerKarma(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out) {
    const std::string category = nd->getNodeString();
    auto get = [&](char const *name, int width) -> Ref {
        const auto it = in.find(name);
        if (it == in.end() || it->second.ref == kNoRef)
            return Zero(width);
        return _b.convert(it->second.ref, width);
    };
    auto text = [&](char const *name) {
        const auto it = in.find(name);
        return it != in.end() ? it->second.text : std::string();
    };
    auto outWidth = [&] {
        const std::vector<mx::OutputPtr> outputs = nd->getActiveOutputs();
        return outputs.empty() ? 0 : typeWidth(outputs[0]->getType());
    };
    // The value of ray variable `name`, else `fallback` (with one warning
    // for a named variable Nova does not have).
    auto rayImport = [&](std::string const &name, int width) {
        Value v;
        const Ref r = RayVariable(name);
        if (r == kNoRef) {
            if (!name.empty() && name != "ray:")
                Warn("rayimport:" + name, "ray variable '" + name + "' is not supported; using the node's input");
            v.ref = get("in", width);
        } else {
            v.ref = _b.convert(r, width);
        }
        return v;
    };
    if (TfStringStartsWith(category, "kma_rayswitch_")) {
        // Karma's ray classes (nova::RayClass): camera 0 ... shadow 6.
        static char const *const kInputs[] = {"camera", "diffuse", "glossy_reflection", "glossy_transmission",
                                              "sss",    "volume",  "shadow"};
        const int w = category == "kma_rayswitch_color" ? 3 : 1;
        const Ref type = _b.global(Global::RayType);
        Ref r = get(kInputs[0], w);
        for (int k = 1; k < 7; ++k)
            r = _b.ternary(Op::Select, _b.binary(Op::Equal, type, _b.constant(float(k))), get(kInputs[k], w), r);
        out["result"].ref = r;
        out["raytype"].ref = type;
        return true;
    }
    if (TfStringStartsWith(category, "kma_rayimport_")) {
        const int w = outWidth();
        if (w == 0)
            return false; // matrices
        out["out"] = rayImport(text("name"), w);
        return true;
    }
    if (category == "kma_ao" || category == "kma_distance" || category == "kma_curvature" ||
        category == "kma_roundededge") {
        // Ray queries (core/shading/query.h). Karma traces around the
        // shading normal unless nN is connected (authored constants are
        // ignored, karma-nodes.md 4.3); without a scene the rounded normal
        // is the direction.
        Ref nN = _b.global(Global::N);
        if (const auto it = in.find("nN"); it != in.end() && it->second.ref != kNoRef) {
            const Op op = _out.graph.nodes[it->second.ref].op;
            if (op != Op::Const && op != Op::Param)
                nN = _b.convert(it->second.ref, 3);
        }
        auto unsupported = [&](char const *name, char const *standard, char const *what) {
            const std::string s = text(name);
            if (!s.empty() && s != standard)
                Warn(category + ":" + name, category + " " + name + " '" + s + "' is not supported; " + what);
        };
        auto pack = [&](Ref a, Ref b, Ref c, Ref d) { return _b.concat(_b.concat(a, b), _b.concat(c, d)); };
        auto flag = [&](char const *name) { // 0 or 1
            return _b.binary(Op::Greater, _b.unary(Op::Abs, get(name, 1)), _b.constant(0.5f));
        };
        if (category == "kma_ao") {
            unsupported("traceset", "*", "tracing all objects");
            out["visibility"].ref =
                _b.query(nova::QueryKind::Occlusion, nN,
                         pack(get("coneangle", 1), get("maxdistance", 1), get("samples", 1), flag("frontface")));
            return true;
        }
        if (category == "kma_distance") {
            unsupported("traceset", "*", "tracing all objects");
            unsupported("objectspace", "0", "distances are in world space");
            const Ref flags = _b.add(_b.binary(Op::Greater, get("mode", 1), _b.constant(0.5f)),
                                     _b.add(_b.mul(flag("normalize"), _b.constant(2.0f)),
                                            _b.mul(flag("frontface"), _b.constant(4.0f))));
            const Ref q = _b.query(nova::QueryKind::Distance, nN,
                                   pack(get("coneangle", 1), get("maxdistance", 1), get("samples", 1), flags));
            out["dist"].ref = _b.component(q, 0);
            out["dir"].ref = _b.concat(_b.concat(_b.component(q, 1), _b.component(q, 2)), _b.component(q, 3));
            return true;
        }
        if (category == "kma_curvature") {
            // convex and concave scaled, clamped and given contrast by
            // Perlin's gain (0.5: none); the help documents them only as
            // "multiplies" and "contrast control", Karma's order is not
            // measured.
            const Ref q = _b.query(nova::QueryKind::Curvature, nN,
                                   pack(get("maxdistance", 1), get("samples", 1), Zero(1), Zero(1)));
            auto shape = [&](Ref x, char const *scale, char const *bias) {
                const Ref t = _b.ternary(Op::Clamp, _b.mul(x, get(scale, 1)), Zero(1), _b.constant(1.0f));
                // gain(g, t) = t < 0.5 ? b(2t) / 2 : 1 - b(2 - 2t) / 2, b(x) = x^(ln(1 - g) / ln 0.5)
                const Ref g = _b.ternary(Op::Clamp, get(bias, 1), _b.constant(1e-4f), _b.constant(1.0f - 1e-4f));
                const Ref e = _b.div(_b.unary(Op::Log, _b.sub(_b.constant(1.0f), g)), _b.constant(std::log(0.5f)));
                const Ref two = _b.constant(2.0f), half = _b.constant(0.5f);
                const Ref lo = _b.mul(_b.binary(Op::Pow, _b.mul(two, t), e), half);
                const Ref hi = _b.sub(_b.constant(1.0f),
                                      _b.mul(_b.binary(Op::Pow, _b.sub(two, _b.mul(two, t)), e), half));
                return _b.ternary(Op::Select, _b.binary(Op::Greater, half, t), lo, hi);
            };
            const Ref convex = shape(_b.component(q, 0), "convexscale", "convexbias");
            const Ref concave = shape(_b.component(q, 1), "concavescale", "concavebias");
            const Ref k = _b.sub(convex, concave);
            const Ref unit = _b.add(_b.constant(0.5f), _b.mul(_b.constant(0.5f), k));
            out["curvature"].ref = _b.ternary(Op::Select, _b.binary(Op::Greater, get("space", 1), _b.constant(0.5f)), k, unit);
            out["convex"].ref = convex;
            out["concave"].ref = concave;
            return true;
        }
        unsupported("scope", "scope:self", "rounding against the object itself");
        const Ref q = _b.query(nova::QueryKind::RoundedEdge, nN,
                               pack(get("radius", 1), get("samples", 1), get("mode", 1), get("cuspangle", 1)));
        out["roundN"].ref = _b.concat(_b.concat(_b.component(q, 0), _b.component(q, 1)), _b.component(q, 2));
        out["roundMask"].ref = _b.component(q, 3);
        return true;
    }
    if (TfStringStartsWith(category, "kma_voronoinoise")) {
        // Karma's Voronoi: position * freq - offset; distances F1..F3 by the
        // output's width, the seed points back in the input's space.
        const bool d2 = TfStringStartsWith(category, "kma_voronoinoise2d");
        const int pw = d2 ? 2 : 3;
        const Ref freq = get("freq", pw), offset = get("offset", pw);
        const Ref p = _b.sub(_b.mul(get(d2 ? "texcoord" : "position", pw), freq), offset);
        const int metric = std::clamp(std::atoi(text("metric").c_str()), 0, 3);
        const int dw = category.back() == '2' ? 2 : category.back() == '3' ? 3 : 1;
        out["distance"].ref = _b.voronoi(p, get("jitter", 1), metric, 0, dw);
        const char *names[3] = {"p1", "p2", "p3"};
        for (int k = 0; k < 3; ++k) {
            const Ref q = _b.convert(_b.voronoi(p, get("jitter", 1), metric, k + 1, 3), pw);
            out[names[k]].ref = _b.div(_b.add(q, offset), freq);
        }
        return true;
    }
    if (category == "kma_ocio_transform") {
        // Between two color spaces whose transforms to scene_linear are
        // matrices (e.g. kma_melanin's Linear Rec.709 -> scene_linear).
        const Ref from = get("from", 3);
        const HdNovaColorTransform a = HdNovaGetColorTransform(text("fromspace"));
        const HdNovaColorTransform b = HdNovaGetColorTransform(text("tospace"));
        const std::string mode = text("mode");
        if ((!mode.empty() && mode != "space") || a.kind == HdNovaColorTransform::Kind::Nonlinear ||
            b.kind == HdNovaColorTransform::Kind::Nonlinear) {
            Warn("ocio:" + text("fromspace") + ":" + text("tospace"),
                 "OCIO transform '" + text("fromspace") + "' -> '" + text("tospace") + "' (" + mode +
                     ") is not a matrix; passed through");
            out["to"].ref = from;
            return true;
        }
        GfMatrix3d A(1.0), B(1.0);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                A[i][j] = a.matrix[i * 3 + j];
                B[i][j] = b.matrix[i * 3 + j];
            }
        const GfMatrix3d M = B.GetInverse() * A; // rgb_to = B^-1 A rgb_from
        Ref rows[3];
        for (int i = 0; i < 3; ++i)
            rows[i] = _b.dot(from, _b.constant(float(M[i][0]), float(M[i][1]), float(M[i][2])));
        out["to"].ref = _b.concat(_b.concat(rows[0], rows[1]), rows[2]);
        return true;
    }
    if (TfStringStartsWith(category, "kma_stderiv_")) {
        // The input's derivatives with respect to the st coordinates: the
        // differences along the two footprint vectors, solved against st's;
        // derivScale is the footprint's size in st (sqrt of its area).
        const int w = category == "kma_stderiv_float" ? 1 : category == "kma_stderiv_vector2" ? 2 : 3;
        const Ref x = get("input", w), st = GeomProp("UV0", 2);
        const Ref ds = _b.deriv(_b.component(st, 0)), dt = _b.deriv(_b.component(st, 1));
        const Ref sx = _b.component(ds, 0), sy = _b.component(ds, 1), tx = _b.component(dt, 0), ty = _b.component(dt, 1);
        const Ref det = _b.sub(_b.mul(sx, ty), _b.mul(tx, sy));
        const Ref valid = _b.binary(Op::Greater, _b.unary(Op::Abs, det), _b.constant(1e-20f));
        const Ref inv = _b.ternary(Op::Select, valid, _b.div(_b.constant(1.0f), det), Zero(1));
        Ref dS, dT;
        for (int k = 0; k < w; ++k) {
            const Ref d = _b.deriv(w == 1 ? x : _b.component(x, k));
            const Ref xa = _b.component(d, 0), xb = _b.component(d, 1);
            const Ref a = _b.mul(_b.sub(_b.mul(xa, ty), _b.mul(tx, xb)), inv);
            const Ref b = _b.mul(_b.sub(_b.mul(sx, xb), _b.mul(sy, xa)), inv);
            dS = k == 0 ? a : _b.concat(dS, a);
            dT = k == 0 ? b : _b.concat(dT, b);
        }
        out["derivS"].ref = dS;
        out["derivT"].ref = dT;
        out["derivScale"].ref = _b.unary(Op::Sqrt, _b.unary(Op::Abs, det));
        return true;
    }
    if (TfStringStartsWith(category, "kma_ramp_") || TfStringStartsWith(category, "kma_rampconst_")) {
        const bool color = TfStringEndsWith(category, "_color");
        const bool rampConst = TfStringStartsWith(category, "kma_rampconst_");
        const char *k = rampConst ? (color ? "vkeys" : "fkeys") : "keys";
        const char *v = rampConst ? (color ? "vvalues" : "fvalues") : "values";
        const char *b = rampConst ? (color ? "vbasis" : "fbasis") : "basis";
        auto value = [&](char const *name) {
            const auto it = in.find(name);
            return it != in.end() ? it->second : Value{};
        };
        out["out"].ref = Ramp(get("t", 1), value(k), value(v), value(b), color ? 3 : 1);
        return true;
    }
    if (category == "flake2d" || category == "flake3d") {
        // MaterialX flakes (Karma renders the same, docs/research/m4/
        // nodes-and-limits.md 3.3): the flake op finds the cell; the GGX
        // flake normal is sampled here from its noise.
        Ref p = category == "flake2d" ? _b.concat(get("texcoord", 2), Zero(1)) : get("position", 3);
        p = _b.div(p, get("size", 1));
        const Ref f = _b.flake(p, get("coverage", 1));
        const Ref priority = _b.component(f, 0), present = _b.binary(Op::Greater, priority, Zero(1));
        const Ref rnd = _b.component(f, 3), xi1 = _b.component(f, 2);
        const Ref r = get("roughness", 1);
        const Ref phi = _b.mul(_b.component(f, 1), _b.constant(6.28318530718f));
        const Ref tanT = _b.div(_b.mul(_b.mul(r, r), _b.unary(Op::Sqrt, xi1)),
                                _b.unary(Op::Sqrt, _b.sub(_b.constant(1.0f), xi1)));
        const Ref sinT = _b.div(tanT, _b.unary(Op::Sqrt, _b.add(_b.constant(1.0f), _b.mul(tanT, tanT))));
        const Ref cosT = _b.unary(Op::Sqrt, _b.sub(_b.constant(1.0f), _b.mul(sinT, sinT)));
        const Ref n = get("normal", 3);
        const Ref fn = _b.normalize(_b.add(_b.add(_b.mul(get("tangent", 3), _b.mul(_b.unary(Op::Cos, phi), sinT)),
                                                  _b.mul(get("bitangent", 3), _b.mul(_b.unary(Op::Sin, phi), sinT))),
                                           _b.mul(n, cosT)));
        out["id"].ref = _b.ternary(Op::Select, present, _b.unary(Op::Floor, _b.mul(rnd, _b.constant(16777215.0f))), Zero(1));
        out["rand"].ref = _b.ternary(Op::Select, present, rnd, Zero(1));
        out["presence"].ref = priority;
        out["flakenormal"].ref = _b.ternary(Op::Select, present, fn, n);
        return true;
    }
    if (category == "dot" && TfStringStartsWith(text("note"), "ray:")) {
        const int w = outWidth();
        if (w == 0)
            return false;
        out["out"] = rayImport(text("note"), w);
        return true;
    }
    return false;
}

bool Translator::LowerNative(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out) {
    const std::string category = nd->getNodeString();
    if (LowerClosure(nd, in, out) || LowerKarma(nd, in, out) || LowerMatrix(nd, in, out) || LowerPbrUtility(category, in, out) ||
        LowerTexture(nd, in, out) || LowerHexTile(nd, in, out))
        return true;
    const std::vector<mx::OutputPtr> outputs = nd->getActiveOutputs();
    if (outputs.size() != 1)
        return false;
    const int w = typeWidth(outputs[0]->getType());
    auto get = [&](char const *name, int width) -> Ref {
        const auto it = in.find(name);
        if (it == in.end() || it->second.ref == kNoRef)
            return Zero(width);
        return _b.convert(it->second.ref, width);
    };
    auto width = [&](char const *name) {
        const auto it = in.find(name);
        return it != in.end() && it->second.ref != kNoRef ? _b.width(it->second.ref) : 1;
    };
    auto text = [&](char const *name) {
        const auto it = in.find(name);
        return it != in.end() ? it->second.text : std::string();
    };
    auto set = [&](Ref r) {
        Value v;
        v.ref = w > 0 ? _b.convert(r, w) : r;
        out["out"] = v;
        return true;
    };
    const std::string otype = outputs[0]->getType();
    if ((otype == "string" || otype == "filename") && (category == "dot" || category == "constant")) {
        // Text pass-throughs (their only implementations are per-target
        // code): the whole value, so a file path keeps its colour space.
        const auto it = in.find(category == "dot" ? "in" : "value");
        out[outputs[0]->getName()] = it != in.end() ? it->second : Value{};
        return true;
    }
    if (w == 0)
        return false; // shaders, other strings, matrices: not native here

    static const std::map<std::string, Op> unary = {
        {"absval", Op::Abs}, {"floor", Op::Floor}, {"ceil", Op::Ceil}, {"round", Op::Round}, {"sign", Op::Sign},
        {"sqrt", Op::Sqrt},  {"exp", Op::Exp},     {"ln", Op::Log},     {"sin", Op::Sin},     {"cos", Op::Cos},
        {"tan", Op::Tan},    {"asin", Op::Asin},   {"acos", Op::Acos},  {"fract", Op::Fract}, {"not", Op::Not},
    };
    static const std::map<std::string, Op> binary = {
        {"add", Op::Add}, {"subtract", Op::Sub}, {"multiply", Op::Mul}, {"divide", Op::Div}, {"modulo", Op::Mod},
        {"power", Op::Pow}, {"min", Op::Min},    {"max", Op::Max},      {"and", Op::And},    {"or", Op::Or},
    };
    if (const auto u = unary.find(category); u != unary.end())
        return set(_b.unary(u->second, get("in", w)));
    if (const auto bi = binary.find(category); bi != binary.end()) {
        const int w2 = width("in2") == 1 ? 1 : w;
        return set(_b.binary(bi->second, get("in1", w), get("in2", w2)));
    }
    if (category == "constant")
        return set(get("value", w));
    if (category == "dot")
        return set(get("in", w));
    if (category == "convert") {
        if (width("in") != 1 || w != 1)
            return false; // vector conversions are nodegraphs (alpha etc.)
        return set(get("in", 1));
    }
    if (category == "atan2")
        return set(_b.binary(Op::Atan2, get("iny", w), get("inx", w)));
    if (category == "atan2_1_38") // MaterialX 1.38: in1 is y, in2 is x
        return set(_b.binary(Op::Atan2, get("in1", w), get("in2", w)));
    if (category == "swizzle") { // MaterialX 1.38: channels from "rgba", "xyzw", "0" and "1"
        mx::InputPtr input = nd->getActiveInput("in");
        const int wi = input ? std::max(1, typeWidth(input->getType())) : 1;
        const Ref x = get("in", wi);
        const std::string channels = text("channels");
        Ref r = kNoRef;
        for (int k = 0; k < w; ++k) {
            const char ch = k < int(channels.size()) ? channels[size_t(k)] : '0';
            static constexpr char kChannels[] = "rgbaxyzw";
            char const *pos = ch ? std::strchr(kChannels, ch) : nullptr;
            const int index = pos ? int(pos - kChannels) % 4 : -1;
            Ref comp;
            if (ch == '1')
                comp = _b.constant(1.0f);
            else if (index < 0)
                comp = Zero(1);
            else if (wi == 1)
                comp = x;
            else
                comp = index < wi ? _b.component(x, index) : Zero(1);
            r = r == kNoRef ? comp : _b.concat(r, comp);
        }
        return set(r);
    }
    if (category == "viewdirection") {
        const std::string s = text("space");
        const Ref i = _b.global(Global::I);
        return set(s == "object" || s == "model" ? WorldToObject(i, false) : i);
    }
    if (category == "ambientocclusion") {
        // The withdrawn proposal (legacy_1_39): Karma's AO with its defaults,
        // 4 samples around the face-forwarded shading normal.
        const Ref params = _b.concat(_b.concat(get("coneangle", 1), get("maxdistance", 1)),
                                     _b.constant(4.0f, 1.0f, 0.0f));
        return set(_b.query(nova::QueryKind::Occlusion, _b.global(Global::N), _b.convert(params, 4)));
    }
    if (category == "curveadjust") {
        Warn("curveadjust", "curveadjust was rolled back to a proposal in MaterialX 1.39.2; passing its input through");
        return set(get("in", w));
    }
    if (category == "invert") {
        const int wa = width("amount") == 1 ? 1 : w;
        return set(_b.binary(Op::Sub, get("amount", wa), get("in", w)));
    }
    if (category == "clamp") {
        const int wl = width("low") == 1 ? 1 : w;
        return set(_b.ternary(Op::Clamp, get("in", w), get("low", wl), get("high", wl)));
    }
    if (category == "mix") {
        const int wm = width("mix") == 1 ? 1 : w;
        return set(_b.ternary(Op::Mix, get("bg", w), get("fg", w), get("mix", wm)));
    }
    if (category == "normalize")
        return set(_b.normalize(get("in", w)));
    if (category == "magnitude")
        return set(_b.length(get("in", width("in"))));
    if (category == "dotproduct") {
        const int wi = width("in1");
        return set(_b.dot(get("in1", wi), get("in2", wi)));
    }
    if (category == "crossproduct")
        return set(_b.cross(get("in1", 3), get("in2", 3)));
    if (category == "remap") {
        // Karma's efit: a zero input range maps to the middle of the output range.
        const int wr = width("inlow") == 1 ? 1 : w;
        const Ref x = get("in", w), lo = get("inlow", wr), hi = get("inhigh", wr);
        const Ref olo = get("outlow", wr), ohi = get("outhigh", wr);
        const Ref range = _b.sub(hi, lo);
        const Ref t = _b.ternary(Op::Select, _b.binary(Op::Equal, range, Zero(1)), _b.constant(0.5f),
                                 _b.div(_b.sub(x, lo), range));
        return set(_b.add(olo, _b.mul(_b.sub(ohi, olo), t)));
    }
    if (category == "smoothstep") {
        const int ws = width("low") == 1 ? 1 : w;
        return set(_b.ternary(Op::Smoothstep, get("low", ws), get("high", ws), get("in", w)));
    }
    if (category == "luminance") {
        const Ref c = get("in", w);
        const Ref y = _b.dot(_b.convert(c, 3), get("lumacoeffs", 3));
        if (w == 4)
            return set(_b.concat(_b.convert(y, 3), _b.component(c, 3)));
        return set(_b.convert(y, w));
    }
    if (category == "ifgreater" || category == "ifgreatereq" || category == "ifequal") {
        const Op cmp = category == "ifgreater" ? Op::Greater : category == "ifgreatereq" ? Op::GreaterEq : Op::Equal;
        const int wv = width("value1");
        const Ref c = _b.binary(cmp, get("value1", wv), get("value2", wv));
        if (outputs[0]->getType() == "boolean")
            return set(c);
        return set(_b.ternary(Op::Select, c, get("in1", w), get("in2", w)));
    }
    if (category == "combine2")
        return set(_b.concat(get("in1", width("in1")), get("in2", width("in2"))));
    if (category == "combine3")
        return set(_b.concat(_b.concat(get("in1", 1), get("in2", 1)), get("in3", 1)));
    if (category == "combine4")
        return set(_b.concat(_b.concat(get("in1", 1), get("in2", 1)), _b.concat(get("in3", 1), get("in4", 1))));
    if (category == "extract") {
        const int wi = width("in");
        const int index = std::max(0, std::min(wi - 1, std::atoi(text("index").c_str())));
        return set(_b.component(get("in", wi), index));
    }
    if (category == "texcoord")
        return set(_b.primvar(primaryUvName(), w, Zero(1)));
    if (category == "position" || category == "normal") {
        // Karma's spaces too (space:world, space:current = world, space:ndc
        // for positions: docs/design/M11-P2.md).
        const int space = KarmaSpace(text("space"), category == "position", "using object space");
        if (category == "position")
            return set(_b.global(space == 2 ? Global::P : space == 3 ? Global::Pndc : Global::Pobject));
        return set(space == 2 ? _b.normalize(_b.global(Global::N)) : _b.global(Global::Nobject));
    }
    if (category == "tangent" || category == "bitangent") // Karma's frame from the primary uv set
        return set(InSpace(category == "tangent" ? KarmaT() : KarmaB(), text("space")));
    if (category == "normalmap" || category == "normalmap_1_38") {
        // Karma (mxstd_normalmap, karma_mikkt off): Gram-Schmidt T and B against
        // N, r = norm(T v.x s.x + B v.y s.y + N v.z); where r falls on the
        // other side of Ng than N, r -= Ng (r.Ng) 1.001.
        const Ref one = _b.constant(1.0f);
        const Ref v = _b.sub(_b.mul(get("in", 3), _b.constant(2.0f)), one);
        if (text("space") == "object") // MaterialX 1.38 object-space maps: the (signed) cofactor transform
            return set(_b.normalize(ObjectToWorldNormal(v)));
        const bool scale2 = width("scale") == 2;
        const Ref sc = get("scale", scale2 ? 2 : 1);
        const Ref sx = scale2 ? _b.component(sc, 0) : sc, sy = scale2 ? _b.component(sc, 1) : sc;
        const Ref n = _b.normalize(get("normal", 3)), ng = _b.normalize(_b.global(Global::Ng));
        if (_mikkt > 0 || Enabled(text("karma_mikkt"))) {
            const Ref scaled = _b.concat(_b.concat(_b.mul(_b.component(v, 0), sx), _b.mul(_b.component(v, 1), sy)),
                                         _b.component(v, 2));
            return set(NgGuard(MikktNormal(scaled, n), n));
        }
        const Ref T = get("tangent", 3);
        const Ref t = _b.normalize(_b.sub(T, _b.mul(n, _b.dot(n, T))));
        const Ref B = nd->getActiveInput("bitangent") ? get("bitangent", 3) : _b.cross(n, t);
        const Ref bt = _b.normalize(_b.sub(B, _b.mul(n, _b.dot(n, B))));
        const Ref r = _b.normalize(_b.add(_b.add(_b.mul(t, _b.mul(_b.component(v, 0), sx)),
                                                 _b.mul(bt, _b.mul(_b.component(v, 1), sy))),
                                          _b.mul(n, _b.component(v, 2))));
        const Ref rd = _b.dot(r, ng), nd2 = _b.dot(n, ng);
        const Ref same = _b.binary(Op::Equal, _b.unary(Op::Sign, rd), _b.unary(Op::Sign, nd2));
        return set(_b.ternary(Op::Select, same, r, _b.sub(r, _b.mul(ng, _b.mul(rd, _b.constant(1.001f))))));
    }
    if (category == "heighttonormal") {
        // Karma: a world-space bump. The surface moved to P + Ng h scale, its
        // normal from the differences along the footprint vectors, expressed
        // in the frame (dPdu, dPdv projected off Ng, Ng) and encoded x 0.5 +
        // 0.5. The texcoord input is not used.
        const Ref ng = _b.normalize(_b.global(Global::Ng)), scale = get("scale", 1);
        const Ref d = _b.deriv(get("in", 1));
        const Ref P = _b.global(Global::P);
        const Ref fx = _b.sub(_b.global(Global::PDx), P), fy = _b.sub(_b.global(Global::PDy), P);
        const Ref dx = _b.add(fx, _b.mul(ng, _b.mul(_b.component(d, 0), scale)));
        const Ref dy = _b.add(fy, _b.mul(ng, _b.mul(_b.component(d, 1), scale)));
        Ref wn = _b.normalize(_b.cross(dx, dy));
        wn = _b.ternary(Op::Select, _b.binary(Op::Greater, Zero(1), _b.dot(wn, ng)), _b.unary(Op::Neg, wn), wn);
        // No footprints (degenerate differences): the unbumped normal.
        wn = _b.ternary(Op::Select, _b.binary(Op::Greater, _b.dot(wn, wn), _b.constant(0.5f)), wn, ng);
        const Ref du = _b.global(Global::DPdu), dv = _b.global(Global::DPdv);
        const Ref e1 = _b.normalize(_b.sub(du, _b.mul(ng, _b.dot(ng, du))));
        const Ref e2 = _b.normalize(_b.sub(dv, _b.mul(ng, _b.dot(ng, dv))));
        const Ref local = _b.div(_b.concat(_b.concat(_b.dot(wn, _b.cross(e2, ng)), _b.dot(wn, _b.cross(ng, e1))),
                                           _b.dot(wn, _b.cross(e1, e2))),
                                 _b.dot(e1, _b.cross(e2, ng)));
        const Ref half = _b.constant(0.5f);
        return set(_b.add(_b.mul(_b.normalize(local), half), half));
    }
    if (category == "geompropvalue") {
        const std::string name = text("geomprop");
        Ref fallback = get("default", w);
        if (!name.empty())
            return set(_b.primvar(name, w, fallback));
        return set(fallback);
    }
    if (category == "geomcolor")
        return set(_b.primvar("displayColor", w, _b.constant(1.0f)));
    if (category == "ramplr")
        return set(_b.ternary(Op::Mix, get("valuel", w), get("valuer", w), _b.component(get("texcoord", 2), 0)));
    if (category == "ramptb") // Karma: valuet at v = 0 (MaterialX before 1.39.5)
        return set(_b.ternary(Op::Mix, get("valuet", w), get("valueb", w), _b.component(get("texcoord", 2), 1)));
    if (category == "splitlr") {
        const Ref u = _b.component(get("texcoord", 2), 0);
        return set(_b.ternary(Op::Select, _b.binary(Op::GreaterEq, u, get("center", 1)), get("valuer", w),
                              get("valuel", w)));
    }
    if (category == "splittb") { // Karma: v above the center gives valueb
        const Ref v = _b.component(get("texcoord", 2), 1);
        return set(_b.ternary(Op::Select, _b.binary(Op::GreaterEq, v, get("center", 1)), get("valueb", w),
                              get("valuet", w)));
    }
    // ---- M3.7: procedurals, color, spaces, application, compositing ----
    if (category == "noise2d" || category == "noise3d") {
        const bool is2 = category == "noise2d";
        const Ref p = get(is2 ? "texcoord" : "position", is2 ? 2 : 3);
        const int wa = width("amplitude") == 1 ? 1 : w;
        return set(_b.add(_b.mul(_b.noise(p, w), get("amplitude", wa)), get("pivot", 1)));
    }
    if (category == "fractal2d" || category == "fractal3d") {
        const bool is2 = category == "fractal2d";
        const Ref p = get(is2 ? "texcoord" : "position", is2 ? 2 : 3);
        const int wa = width("amplitude") == 1 ? 1 : w;
        return set(_b.mul(_b.fractal(p, get("octaves", 1), get("lacunarity", 1), get("diminish", 1), w),
                          get("amplitude", wa)));
    }
    if (category == "cellnoise2d" || category == "cellnoise3d") {
        const bool is2 = category == "cellnoise2d";
        return set(_b.cellNoise(get(is2 ? "texcoord" : "position", is2 ? 2 : 3), w == 1 ? 1 : 3));
    }
    if (category == "worleynoise2d" || category == "worleynoise3d") {
        const bool is2 = category == "worleynoise2d";
        const int style = std::atoi(text("style").c_str()) == 1 ? 1 : 0;
        return set(_b.worley(get(is2 ? "texcoord" : "position", is2 ? 2 : 3), get("jitter", 1), style, w));
    }
    if (category == "rgbtohsv" || category == "hsvtorgb") {
        const Ref c = get("in", w);
        const Ref r = category == "rgbtohsv" ? _b.rgbToHsv(c) : _b.hsvToRgb(c);
        return set(w == 4 ? _b.concat(r, _b.constant(1.0f)) : r); // Karma: alpha 1
    }
    if (category == "frame")
        return set(_b.global(Global::Frame));
    if (category == "time")
        return set(_b.global(Global::Time));
    if (category == "transformpoint" || category == "transformvector" || category == "transformnormal") {
        auto space = [&](char const *name) {
            const std::string s = text(name);
            if (s.empty())
                return 0; // unspecified: no transform
            return KarmaSpace(s, false, "using no transform");
        };
        const int from = space("fromspace"), to = space("tospace");
        const Ref v = get("in", 3);
        if (from == 0 || to == 0 || from == to)
            return set(v);
        const Ref X = _b.global(Global::ObjX), Y = _b.global(Global::ObjY), Z = _b.global(Global::ObjZ);
        const Ref T = _b.global(Global::ObjT);
        auto c = [&](Ref r, int k) { return _b.component(r, k); };
        const bool toWorld = from == 1;
        if (category == "transformnormal") {
            Ref n;
            if (toWorld) // cofactor transform, signed by the handedness
                n = ObjectToWorldNormal(v);
            else // transpose (the inverse direction up to a positive factor)
                n = _b.concat(_b.concat(_b.dot(X, v), _b.dot(Y, v)), _b.dot(Z, v));
            return set(_b.normalize(n));
        }
        const bool point = category == "transformpoint";
        if (toWorld) {
            Ref r = _b.add(_b.add(_b.mul(X, c(v, 0)), _b.mul(Y, c(v, 1))), _b.mul(Z, c(v, 2)));
            return set(point ? _b.add(r, T) : r);
        }
        return set(WorldToObject(v, point));
    }
    if (category == "rotate2d") {
        const Ref a = _b.mul(get("amount", 1), _b.constant(float(3.14159265358979323846 / 180.0)));
        const Ref s = _b.unary(Op::Sin, a), co = _b.unary(Op::Cos, a);
        const Ref v = get("in", 2);
        const Ref x = _b.component(v, 0), y = _b.component(v, 1);
        return set(_b.concat(_b.add(_b.mul(co, x), _b.mul(s, y)), _b.sub(_b.mul(co, y), _b.mul(s, x))));
    }
    if (category == "rotate3d") {
        const Ref a = _b.mul(get("amount", 1), _b.constant(float(3.14159265358979323846 / 180.0)));
        const Ref s = _b.unary(Op::Sin, a), co = _b.unary(Op::Cos, a);
        const Ref v = get("in", 3), axis = _b.normalize(get("axis", 3));
        return set(_b.add(_b.add(_b.mul(v, co), _b.mul(_b.cross(v, axis), s)),
                          _b.mul(axis, _b.mul(_b.dot(axis, v), _b.sub(_b.constant(1.0f), co)))));
    }
    if (category == "plus" || category == "minus" || category == "difference" || category == "screen" ||
        category == "burn" || category == "dodge") {
        const Ref fg = get("fg", w), bg = get("bg", w), m = get("mix", 1), one = _b.constant(1.0f);
        if (category == "burn" || category == "dodge") { // Karma (mxstd.h): lerp(bg, op, mix), safe division
            const Ref op = category == "burn" ? _b.sub(one, _b.div(_b.sub(one, bg), fg))
                                              : _b.sub(one, _b.div(bg, _b.sub(one, fg)));
            return set(_b.ternary(Op::Mix, bg, op, m));
        }
        Ref op;
        if (category == "plus")
            op = _b.add(bg, fg);
        else if (category == "minus")
            op = _b.sub(bg, fg);
        else if (category == "difference")
            op = _b.unary(Op::Abs, _b.sub(bg, fg));
        else
            op = _b.sub(one, _b.mul(_b.sub(one, fg), _b.sub(one, bg)));
        return set(_b.add(_b.mul(m, op), _b.mul(_b.sub(one, m), bg)));
    }
    if (category == "inside")
        return set(_b.mul(get("in", w), get("mask", 1)));
    if (category == "outside")
        return set(_b.mul(get("in", w), _b.sub(_b.constant(1.0f), get("mask", 1))));
    if (category == "premult" || category == "unpremult") {
        const Ref c = get("in", 4), rgb = _b.swizzle(c, 0, 1, 2), a = _b.component(c, 3);
        return set(_b.concat(category == "premult" ? _b.mul(rgb, a) : _b.div(rgb, a), a));
    }
    if (category == "over" || category == "in" || category == "mask" || category == "matte" || category == "out" ||
        category == "disjointover") {
        const Ref fg = get("fg", 4), bg = get("bg", 4), m = get("mix", 1), one = _b.constant(1.0f);
        const Ref fa = _b.component(fg, 3), ba = _b.component(bg, 3);
        Ref op;
        if (category == "over") {
            op = _b.add(fg, _b.mul(bg, _b.sub(one, fa)));
        } else if (category == "in") {
            op = _b.mul(fg, ba);
        } else if (category == "mask") {
            op = _b.mul(bg, fa);
        } else if (category == "out") {
            op = _b.mul(fg, _b.sub(one, ba));
        } else if (category == "matte") {
            const Ref rgb = _b.add(_b.mul(_b.swizzle(fg, 0, 1, 2), fa), _b.mul(_b.swizzle(bg, 0, 1, 2), _b.sub(one, fa)));
            op = _b.concat(rgb, _b.add(fa, _b.mul(ba, _b.sub(one, fa))));
        } else { // disjointover (MaterialX mx_disjointover_color4)
            const Ref sum = _b.add(fa, ba);
            const Ref frgb = _b.swizzle(fg, 0, 1, 2), brgb = _b.swizzle(bg, 0, 1, 2);
            const Ref scaled = _b.ternary(Op::Select, _b.binary(Op::Greater, _b.unary(Op::Abs, ba), _b.constant(1.1920929e-7f)),
                                          _b.add(frgb, _b.mul(brgb, _b.div(_b.sub(one, fa), ba))), Zero(3));
            const Ref rgb = _b.ternary(Op::Select, _b.binary(Op::GreaterEq, one, sum), _b.add(frgb, brgb), scaled);
            op = _b.concat(rgb, _b.binary(Op::Min, sum, one));
        }
        return set(_b.add(_b.mul(op, m), _b.mul(bg, _b.sub(one, m))));
    }
    return false;
}


// The categories NOVA_MTLX_EXPAND names (comma separated) expand through
// their MaterialX nodegraphs instead of the native models: the equivalence
// oracle of the generic closures (docs/research/m4/bsdf-graphs.md 6, 7.2).
bool Translator::ForceExpand(std::string const &category) {
    char const *env = std::getenv("NOVA_MTLX_EXPAND");
    if (!env || !*env)
        return false;
    for (std::string const &item : TfStringSplit(env, ","))
        if (TfStringTrim(item) == category)
            return true;
    return false;
}

// A native surface model as a segment of a generic surface (inside a
// mix_surfaceshader or a nodegraph): its inputs at a block offset assigned
// when the surface is emitted.
bool Translator::NativeSegment(mx::NodeDefPtr const &nd, ValueMap &in, SurfaceSegment &seg) {
    const std::string category = nd->getNodeString();
    FieldMap const *map = nullptr;
    size_t mapSize = 0;
    auto defaults = std::make_unique<nova::SurfaceInputs>();
    if (category == "standard_surface") {
        seg.model = nova::SurfaceModel::StandardSurface;
        *defaults = nova::standardSurfaceDefaults(TfStringEndsWith(nd->getName(), "_100"));
        map = kStandardSurface;
        mapSize = std::size(kStandardSurface);
    } else if (category == "open_pbr_surface") {
        seg.model = nova::SurfaceModel::OpenPBR;
        *defaults = nova::openPbrDefaults();
        map = kOpenPbr;
        mapSize = std::size(kOpenPbr);
    } else if (category == "surface_unlit") {
        seg.model = nova::SurfaceModel::Unlit;
        *defaults = nova::unlitDefaults();
        map = kUnlit;
        mapSize = std::size(kUnlit);
    } else if (category == "UsdPreviewSurface") {
        seg.model = nova::SurfaceModel::UsdPreview;
        *defaults = nova::usdPreviewDefaults();
        map = kUsdPreview;
        mapSize = std::size(kUsdPreview);
        Warn("mix:UsdPreviewSurface", "UsdPreviewSurface inside a surface mix: its opacity modes and normal map are "
                                      "not applied");
    } else if (category == "kma_hair" || category == "kma_fur" || category == "kma_fur_2") {
        seg.model = nova::SurfaceModel::Hair;
        *defaults = nova::hairDefaults();
        map = kKarmaHair;
        mapSize = std::size(kKarmaHair);
    } else {
        return false;
    }
    // Anisotropy in use: the default tangent is Karma's.
    bool anisotropic = false;
    for (const char *name : {"specular_anisotropy", "coat_anisotropy", "specular_roughness_anisotropy",
                             "coat_roughness_anisotropy"}) {
        const auto it = in.find(name);
        float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (it != in.end() && it->second.ref != kNoRef)
            anisotropic |= !IsConst(it->second.ref, c) || c[0] != 0.0f;
    }
    for (size_t i = 0; i < mapSize; ++i) {
        const auto it = in.find(map[i].input);
        if (it == in.end() || it->second.ref == kNoRef)
            continue;
        if (it->second.geomDefault) {
            if (anisotropic && (map[i].field == sf::tangent || map[i].field == sf::coatTangent))
                seg.fields.emplace_back(map[i].field, 3, KarmaT());
            continue;
        }
        seg.fields.emplace_back(map[i].field, map[i].width, _b.convert(it->second.ref, map[i].width));
        if (map[i].field == sf::opacity)
            seg.opacity = _b.convert(it->second.ref, 3);
    }
    seg.defaults = defaults.get();
    _segmentDefaults.push_back(std::move(defaults));
    return true;
}

// pbrlib closures, EDFs, VDFs and surface shaders as closure-table handles
// (docs/research/m4/bsdf-graphs.md 5.5).
bool Translator::LowerClosure(mx::NodeDefPtr const &nd, ValueMap &in, ValueMap &out) {
    const std::vector<mx::OutputPtr> outputs = nd->getActiveOutputs();
    if (outputs.size() != 1)
        return false;
    const std::string type = outputs[0]->getType();
    using C = Value::Closure;
    const std::string category = nd->getNodeString();
    if (type == "material" && category == "volumematerial") {
        const auto it = in.find("volumeshader");
        out[outputs[0]->getName()] = it != in.end() ? it->second : Value{};
        return true;
    }
    if (type == "material" && category == "surfacematerial") {
        // Inside node graphs (LamaSurface): the surface shader; a back
        // surface shader is ignored, as at the terminal.
        const auto back = in.find("backsurfaceshader");
        if (back != in.end() && back->second.closure == C::Surface && back->second.handle >= 0)
            for (SurfaceSegment const &s : _surfaces[size_t(back->second.handle)])
                if (s.root >= 0 || s.model != nova::SurfaceModel::Graph) {
                    Warn("backsurfaceshader", "surfacematerial's backsurfaceshader is not supported yet; using the front");
                    break;
                }
        const auto it = in.find("surfaceshader");
        out[outputs[0]->getName()] = it != in.end() ? it->second : Value{};
        return true;
    }
    // Karma's registry types kma_volume's output as a string.
    const C kind = type == "BSDF" ? C::Bsdf : type == "EDF" ? C::Edf : type == "VDF" ? C::Vdf
                   : type == "surfaceshader" ? C::Surface : type == "displacementshader" ? C::Displacement
                   : type == "volumeshader" || category == "kma_volume" ? C::Volume : C::None;
    if (kind == C::None)
        return false;
    auto num = [&](char const *name, int width) -> Ref {
        const auto it = in.find(name);
        return it == in.end() || it->second.ref == kNoRef ? Zero(width) : _b.convert(it->second.ref, width);
    };
    auto text = [&](char const *name) {
        const auto it = in.find(name);
        return it != in.end() ? it->second.text : std::string();
    };
    auto closureIn = [&](char const *name, C want) {
        const auto it = in.find(name);
        return it != in.end() && it->second.closure == want ? it->second.handle : -1;
    };
    auto result = [&](C k, int handle) {
        Value v;
        v.closure = k;
        v.handle = handle;
        out[outputs[0]->getName()] = v;
        return true;
    };
    auto node = [&](ClosureNode n) {
        _closures.push_back(std::move(n));
        return int(_closures.size()) - 1;
    };
    auto nonNegative = [&](Ref r) { return _b.binary(Op::Max, r, Zero(1)); };
    auto unit = [&](Ref r) { return _b.ternary(Op::Clamp, r, Zero(1), _b.constant(1.0f)); };

    if (category == "dot") { // pass-through of any closure
        const auto it = in.find("in");
        out[outputs[0]->getName()] = it != in.end() ? it->second : Value{};
        return true;
    }

    // ---- Displacement: an object-space offset (M4.6) ----
    if (kind == C::Displacement) {
        auto displacement = [&](Ref offset) {
            _displacements.push_back(offset);
            return result(C::Displacement, int(_displacements.size()) - 1);
        };
        auto offsetIn = [&](char const *name) {
            const int h = closureIn(name, C::Displacement);
            return h >= 0 ? _displacements[size_t(h)] : Zero(3);
        };
        if (category == "displacement") {
            // float: along the normal; vector3: in (T, B, N) of the st set
            // (MaterialX, measured on Karma), object space.
            const auto d = in.find("displacement");
            const bool vector = d != in.end() && d->second.ref != kNoRef && _b.width(d->second.ref) == 3;
            const Ref scale = num("scale", 1);
            if (!vector)
                return displacement(_b.mul(ObjectN(), _b.mul(num("displacement", 1), scale)));
            Ref t, b;
            ObjectFrame(t, b);
            const Ref v = num("displacement", 3);
            const Ref o = _b.add(_b.add(_b.mul(t, _b.component(v, 0)), _b.mul(b, _b.component(v, 1))),
                                 _b.mul(ObjectN(), _b.component(v, 2)));
            return displacement(_b.mul(o, scale));
        }
        if (category == "mix") {
            const Ref m = unit(num("mix", 1));
            return displacement(_b.ternary(Op::Mix, offsetIn("bg"), offsetIn("fg"), m));
        }
        return false;
    }

    // ---- BSDFs ----
    if (kind == C::Bsdf) {
        // The frame of a leaf: a connected normal, and the tangent where the
        // roughness may be anisotropic (Karma's tangent by default).
        auto frameOf = [&](ClosureNode &n, bool hasTangent, Ref alpha) {
            const auto ni = in.find("normal");
            if (ni != in.end() && ni->second.ref != kNoRef && !ni->second.geomDefault)
                n.normal = _b.convert(ni->second.ref, 3);
            if (!hasTangent)
                return;
            float a[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (IsConst(alpha, a) && a[0] == a[1])
                return; // isotropic
            const auto ti = in.find("tangent");
            if (ti != in.end() && ti->second.ref != kNoRef && !ti->second.geomDefault)
                n.tangent = _b.convert(ti->second.ref, 3);
            else
                n.tangent = KarmaT();
        };
        auto scatter = [&]() -> uint8_t {
            const std::string m = text("scatter_mode");
            return m == "T" ? nova::kLeafTransmit : m == "RT" ? uint8_t(nova::kLeafReflect | nova::kLeafTransmit)
                                                             : nova::kLeafReflect;
        };
        auto retro = [&]() {
            if (Enabled(text("retroreflective")))
                Warn("retroreflective", category + ": retroreflective is not supported yet; reflecting normally");
        };
        ClosureNode n;
        if (category == "oren_nayar_diffuse_bsdf" || category == "burley_diffuse_bsdf") {
            n.kind = category == "burley_diffuse_bsdf" ? nova::LeafKind::Burley : nova::LeafKind::Diffuse;
            if (Enabled(text("energy_compensation")))
                n.flags |= nova::kLeafEnergyComp;
            n.params = {num("weight", 1), num("color", 3), num("roughness", 1)};
            frameOf(n, false, kNoRef);
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "translucent_bsdf" || category == "subsurface_bsdf") {
            n.kind = category == "translucent_bsdf" ? nova::LeafKind::Translucent : nova::LeafKind::Subsurface;
            n.params = {num("weight", 1), num("color", 3)};
            if (n.kind == nova::LeafKind::Subsurface) { // the walk's medium (M9)
                n.params.push_back(num("radius", 3));
                n.params.push_back(num("anisotropy", 1));
            }
            frameOf(n, false, kNoRef);
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "sheen_bsdf") {
            n.kind = nova::LeafKind::Sheen;
            if (text("mode") == "zeltner")
                n.flags |= nova::kLeafZeltner;
            n.params = {num("weight", 1), num("color", 3), num("roughness", 1)};
            frameOf(n, false, kNoRef);
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "dielectric_bsdf") {
            retro();
            n.kind = nova::LeafKind::Dielectric;
            n.flags = scatter();
            const Ref alpha = num("roughness", 2);
            n.params = {num("weight", 1), num("tint", 3), num("ior", 1), alpha, num("thinfilm_thickness", 1),
                        num("thinfilm_ior", 1)};
            frameOf(n, true, alpha);
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "conductor_bsdf") {
            retro();
            n.kind = nova::LeafKind::Conductor;
            const Ref alpha = num("roughness", 2);
            n.params = {num("weight", 1), num("ior", 3), num("extinction", 3), alpha, num("thinfilm_thickness", 1),
                        num("thinfilm_ior", 1)};
            frameOf(n, true, alpha);
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "generalized_schlick_bsdf") {
            retro();
            n.kind = nova::LeafKind::Schlick;
            n.flags = scatter();
            if (n.flags & nova::kLeafTransmit)
                Warn("schlick:transmission", "generalized_schlick_bsdf transmission refracts with the index of its "
                                             "average color0 and an exact dielectric Fresnel (approximation)");
            const Ref alpha = num("roughness", 2);
            n.params = {num("weight", 1), num("color0", 3), num("color82", 3), num("color90", 3), num("exponent", 1),
                        alpha, num("thinfilm_thickness", 1), num("thinfilm_ior", 1)};
            frameOf(n, true, alpha);
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "thin_film_bsdf") { // MaterialX 1.38: rewritten by the layer above it
            n.film = true;
            n.params = {num("thickness", 1), num("ior", 1)};
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "chiang_hair_bsdf") {
            // The MaterialX spec's Chiang fibre (docs/design/M9.md D6): Karma
            // lowers this node to something else (its furnace is 0.04-0.06,
            // research 3.7); Nova follows the spec, as Karma Hair renders it.
            n.kind = nova::LeafKind::ChiangHair;
            n.params = {_b.constant(1.0f),       num("tint_R", 3),       num("tint_TT", 3),
                        num("tint_TRT", 3),      num("ior", 1),          num("roughness_R", 2),
                        num("roughness_TT", 2),  num("roughness_TRT", 2), num("cuticle_angle", 1),
                        num("absorption_coefficient", 3)};
            frameOf(n, false, kNoRef);
            const auto ti = in.find("curve_direction");
            if (ti != in.end() && ti->second.ref != kNoRef && !ti->second.geomDefault)
                n.tangent = _b.convert(ti->second.ref, 3);
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "layer") {
            const int top = closureIn("top", C::Bsdf);
            const int baseVdf = closureIn("base", C::Vdf);
            if (baseVdf >= 0) { // a BSDF over a medium
                if (top < 0)
                    return result(C::Bsdf, -1);
                // Inside a surface only absorption renders: warn about a
                // scattering value known here (a constant or the material's value).
                const Ref sc = _vdfs[size_t(baseVdf)].scattering;
                float s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                bool known = IsConst(sc, s);
                if (!known && _out.graph.nodes[sc].op == Op::Param &&
                    size_t(_out.graph.nodes[sc].imm) + 3 <= _out.params.size()) {
                    for (int k = 0; k < 3; ++k)
                        s[k] = _out.params[_out.graph.nodes[sc].imm + uint32_t(k)];
                    known = true;
                }
                if (!known || s[0] != 0.0f || s[1] != 0.0f || s[2] != 0.0f)
                    Warn("vdf:scattering", "volume scattering inside surfaces is not rendered; only absorption is");
                n.op = nova::GraphOp::LayerVdf;
                n.a = top;
                n.params = {_vdfs[size_t(baseVdf)].absorption};
                return result(C::Bsdf, node(std::move(n)));
            }
            const int base = closureIn("base", C::Bsdf);
            if (top >= 0 && _closures[size_t(top)].film) {
                // MaterialX's 1.39 upgrade: the film's thickness and index on
                // every reflecting interface below, the layer replaced by its base.
                std::vector<int> stack{base};
                std::set<int> seen;
                while (!stack.empty()) {
                    const int i = stack.back();
                    stack.pop_back();
                    if (i < 0 || !seen.insert(i).second)
                        continue;
                    ClosureNode &c = _closures[size_t(i)];
                    if (c.op != nova::GraphOp::Leaf) {
                        stack.push_back(c.a);
                        stack.push_back(c.b);
                        continue;
                    }
                    const bool tOnly = (c.flags & (nova::kLeafReflect | nova::kLeafTransmit)) == nova::kLeafTransmit;
                    const size_t at = c.kind == nova::LeafKind::Dielectric || c.kind == nova::LeafKind::Conductor ? 4
                                      : c.kind == nova::LeafKind::Schlick                                         ? 6
                                                                                                                  : 0;
                    if (at && !tOnly) {
                        c.params[at] = _closures[size_t(top)].params[0];
                        c.params[at + 1] = _closures[size_t(top)].params[1];
                    }
                }
                return result(C::Bsdf, base);
            }
            if (top < 0 || base < 0)
                return result(C::Bsdf, top < 0 ? base : top);
            n.op = nova::GraphOp::Layer;
            n.a = top;
            n.b = base;
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "mix") {
            const int fg = closureIn("fg", C::Bsdf), bg = closureIn("bg", C::Bsdf);
            const Ref m = unit(num("mix", 1));
            if (fg < 0 && bg < 0)
                return result(C::Bsdf, -1);
            if (fg < 0 || bg < 0) { // one side: a weight
                n.op = nova::GraphOp::Multiply;
                n.a = fg < 0 ? bg : fg;
                n.params = {_b.convert(fg < 0 ? _b.sub(_b.constant(1.0f), m) : m, 3)};
                return result(C::Bsdf, node(std::move(n)));
            }
            n.op = nova::GraphOp::Mix;
            n.a = fg;
            n.b = bg;
            n.params = {m};
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "add") {
            const int a = closureIn("in1", C::Bsdf), b = closureIn("in2", C::Bsdf);
            if (a < 0 || b < 0)
                return result(C::Bsdf, a < 0 ? b : a);
            n.op = nova::GraphOp::Add;
            n.a = a;
            n.b = b;
            return result(C::Bsdf, node(std::move(n)));
        }
        if (category == "multiply") {
            const int a = closureIn("in1", C::Bsdf);
            if (a < 0)
                return result(C::Bsdf, -1);
            n.op = nova::GraphOp::Multiply;
            n.a = a;
            n.params = {nonNegative(num("in2", 3))}; // >= 0 only (M4 D8)
            return result(C::Bsdf, node(std::move(n)));
        }
        return false;
    }

    // ---- EDFs: lists of terms, linear in IR ----
    if (kind == C::Edf) {
        auto terms = [&](char const *name) {
            const int h = closureIn(name, C::Edf);
            return h >= 0 ? _edfs[size_t(h)] : std::vector<EmissionTerm>{};
        };
        auto edf = [&](std::vector<EmissionTerm> t) {
            _edfs.push_back(std::move(t));
            return result(C::Edf, int(_edfs.size()) - 1);
        };
        auto scaled = [&](std::vector<EmissionTerm> t, Ref factor) {
            for (EmissionTerm &e : t)
                e.color = _b.mul(e.color, factor);
            return t;
        };
        if (category == "uniform_edf" || category == "conical_edf" || category == "measured_edf") {
            if (category != "uniform_edf")
                Warn(category, category + " is rendered as uniform emission (as Karma does)");
            return edf({EmissionTerm{num("color", 3), kNoRef}});
        }
        if (category == "generalized_schlick_edf") {
            std::vector<EmissionTerm> t = terms("base");
            const Ref s = _b.concat(_b.concat(num("color0", 3), num("color90", 3)), num("exponent", 1));
            for (EmissionTerm &e : t) {
                if (e.schlick != kNoRef)
                    Warn("schlick_edf:nested", "nested generalized_schlick_edf: only the outer one applies");
                e.schlick = s;
            }
            return edf(std::move(t));
        }
        if (category == "mix") {
            const Ref m = unit(num("mix", 1));
            std::vector<EmissionTerm> t = scaled(terms("fg"), m);
            for (EmissionTerm &e : scaled(terms("bg"), _b.sub(_b.constant(1.0f), m)))
                t.push_back(e);
            return edf(std::move(t));
        }
        if (category == "add") {
            std::vector<EmissionTerm> t = terms("in1");
            for (EmissionTerm &e : terms("in2"))
                t.push_back(e);
            return edf(std::move(t));
        }
        if (category == "multiply")
            return edf(scaled(terms("in1"), num("in2", 3)));
        return false;
    }

    // ---- VDFs: media (absorption, scattering and its g) ----
    // g of a sum of media: weighted by the parts' mean scattering.
    auto weightedG = [&](Ref sa, Ref ga, Ref sb, Ref gb) {
        const Ref third = _b.constant(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
        const Ref wa = _b.dot(sa, third), wb = _b.dot(sb, third);
        return _b.div(_b.add(_b.mul(wa, ga), _b.mul(wb, gb)), _b.add(wa, wb));
    };
    if (kind == C::Vdf) {
        auto vdf = [&](VdfTerm t) {
            _vdfs.push_back(t);
            return result(C::Vdf, int(_vdfs.size()) - 1);
        };
        auto termOf = [&](char const *name) {
            const int h = closureIn(name, C::Vdf);
            return h >= 0 ? _vdfs[size_t(h)] : VdfTerm{Zero(3), Zero(3), Zero(1)};
        };
        if (category == "absorption_vdf")
            return vdf({num("absorption", 3), Zero(3), Zero(1)});
        if (category == "anisotropic_vdf")
            return vdf({num("absorption", 3), num("scattering", 3), num("anisotropy", 1)});
        if (category == "mix") {
            const Ref m = unit(num("mix", 1));
            const VdfTerm fg = termOf("fg"), bg = termOf("bg");
            const Ref sf = _b.mul(fg.scattering, m), sb = _b.mul(bg.scattering, _b.sub(_b.constant(1.0f), m));
            return vdf({_b.ternary(Op::Mix, bg.absorption, fg.absorption, m), _b.add(sf, sb),
                        weightedG(sf, fg.anisotropy, sb, bg.anisotropy)});
        }
        if (category == "add") {
            const VdfTerm a = termOf("in1"), c = termOf("in2");
            return vdf({_b.add(a.absorption, c.absorption), _b.add(a.scattering, c.scattering),
                        weightedG(a.scattering, a.anisotropy, c.scattering, c.anisotropy)});
        }
        if (category == "multiply") {
            const VdfTerm a = termOf("in1");
            const Ref f = num("in2", 3);
            return vdf({_b.mul(a.absorption, f), _b.mul(a.scattering, f), a.anisotropy});
        }
        return false;
    }

    // ---- Volume shaders (docs/design/M8.md D4) ----
    if (kind == C::Volume) {
        namespace vf = nova::volume_field;
        auto shader = [&](VolumeShader s) {
            _volumeShaders.push_back(s);
            return result(C::Volume, int(_volumeShaders.size()) - 1);
        };
        if (category == "kma_volume") {
            VolumeShader s;
            s.field[vf::absorption] = num("absorption", 3);
            s.field[vf::scattering] = num("scattering", 3);
            s.field[vf::emission] = num("emission", 3);
            s.field[vf::anisotropy] = num("anisotropy", 1);
            s.field[vf::secondaryMix] = num("secondarymix", 1);
            s.field[vf::secondaryAnisotropy] = num("secondaryanisotropy", 1);
            s.field[vf::extinctionModifier] = num("extinctionmodifier", 1);
            s.field[vf::contributionModifier] = num("contributionmodifier", 1);
            s.field[vf::modifierStartLevel] = num("modifierstartlevel", 1);
            return shader(s);
        }
        if (category == "volume") { // MaterialX: a medium and its emission
            VolumeShader s;
            if (const int v = closureIn("vdf", C::Vdf); v >= 0) {
                s.field[vf::absorption] = _vdfs[size_t(v)].absorption;
                s.field[vf::scattering] = _vdfs[size_t(v)].scattering;
                s.field[vf::anisotropy] = _vdfs[size_t(v)].anisotropy;
            }
            if (const int e = closureIn("edf", C::Edf); e >= 0) {
                Ref sum = kNoRef;
                for (EmissionTerm const &t : _edfs[size_t(e)])
                    sum = sum == kNoRef ? t.color : _b.add(sum, t.color);
                s.field[vf::emission] = sum;
            }
            return shader(s);
        }
        // Mixes and sums of volume shaders: every field blended, the
        // anisotropies weighted by scattering.
        const nova::VolumeInputs defaults = nova::volumeDefaults();
        auto fieldOf = [&](char const *name, uint16_t f) {
            const int h = closureIn(name, C::Volume);
            const int width = f < vf::anisotropy ? 3 : 1;
            if (h >= 0 && _volumeShaders[size_t(h)].field[f] != kNoRef)
                return _b.convert(_volumeShaders[size_t(h)].field[f], width);
            return _b.constant(&defaults.v[f], width);
        };
        if (category == "mix" || category == "add") {
            const bool mix = category == "mix";
            const char *a = mix ? "bg" : "in1", *c = mix ? "fg" : "in2";
            const Ref m = mix ? unit(num("mix", 1)) : kNoRef;
            VolumeShader s;
            for (auto const &[f, width] : kVolumeFields) {
                if (f == vf::anisotropy)
                    continue;
                s.field[f] = mix ? _b.ternary(Op::Mix, fieldOf(a, f), fieldOf(c, f), m) : _b.add(fieldOf(a, f), fieldOf(c, f));
            }
            const Ref one = _b.constant(1.0f);
            const Ref sa = mix ? _b.mul(fieldOf(a, vf::scattering), _b.sub(one, m)) : fieldOf(a, vf::scattering);
            const Ref sc = mix ? _b.mul(fieldOf(c, vf::scattering), m) : fieldOf(c, vf::scattering);
            s.field[vf::anisotropy] = weightedG(sa, fieldOf(a, vf::anisotropy), sc, fieldOf(c, vf::anisotropy));
            if (!mix) // modifiers and the secondary lobe do not add up
                for (uint16_t f : {vf::secondaryAnisotropy, vf::secondaryMix, vf::extinctionModifier,
                                   vf::contributionModifier, vf::modifierStartLevel})
                    s.field[f] = fieldOf(a, f);
            return shader(s);
        }
        return false;
    }

    // ---- Surface shaders: segments ----
    auto surface = [&](std::vector<SurfaceSegment> segs) {
        _surfaces.push_back(std::move(segs));
        return result(C::Surface, int(_surfaces.size()) - 1);
    };
    if (NativeModel(category) && !ForceExpand(category)) {
        SurfaceSegment seg;
        if (!NativeSegment(nd, in, seg))
            return false;
        return surface({std::move(seg)});
    }
    if (category == "surface") {
        SurfaceSegment seg;
        seg.root = closureIn("bsdf", C::Bsdf);
        const int e = closureIn("edf", C::Edf);
        if (e >= 0)
            seg.emission = _edfs[size_t(e)];
        seg.opacity = _b.convert(num("opacity", 1), 3);
        seg.thinWalled = num("thin_walled", 1);
        return surface({std::move(seg)});
    }
    if (category == "mix") {
        const Ref m = unit(num("mix", 1));
        std::vector<SurfaceSegment> segs;
        auto add = [&](char const *name, Ref factor) {
            const int h = closureIn(name, C::Surface);
            if (h < 0)
                return;
            for (SurfaceSegment s : _surfaces[size_t(h)]) {
                s.weight = s.weight == kNoRef ? factor : _b.mul(s.weight, factor);
                segs.push_back(std::move(s));
            }
        };
        add("fg", m);
        add("bg", _b.sub(_b.constant(1.0f), m));
        return surface(std::move(segs));
    }
    if (category == "multiply") { // MaterialX 1.38: a weighted surface shader
        const int h = closureIn("in1", C::Surface);
        if (h < 0)
            return surface({});
        const auto f = in.find("in2");
        Ref factor = num("in2", 1);
        if (f != in.end() && f->second.ref != kNoRef && _b.width(f->second.ref) == 3) {
            Warn("multiply:surfaceshader", "a surface shader multiplied by a color is weighted by the color's average");
            const Ref c = f->second.ref;
            factor = _b.mul(_b.add(_b.add(_b.component(c, 0), _b.component(c, 1)), _b.component(c, 2)),
                            _b.constant(1.0f / 3.0f));
        }
        std::vector<SurfaceSegment> segs = _surfaces[size_t(h)];
        for (SurfaceSegment &s : segs)
            s.weight = s.weight == kNoRef ? nonNegative(factor) : _b.mul(s.weight, nonNegative(factor));
        return surface(std::move(segs));
    }
    return false;
}

// A generic surface into the IR outputs and the closure structure.
bool Translator::EmitGraphSurface(Value const &v) {
    if (v.closure != Value::Closure::Surface || v.handle < 0) {
        _out.error = "the surface terminal yields no surface shader";
        return false;
    }
    std::vector<SurfaceSegment> segs = _surfaces[size_t(v.handle)];
    if (segs.size() > size_t(nova::ClosureGraph::kMaxSegments)) {
        Warn("segments", "more than 4 surface shaders mixed; the rest are dropped");
        segs.resize(nova::ClosureGraph::kMaxSegments);
    }
    auto G = std::make_shared<nova::ClosureGraph>();
    uint16_t next = sf::kCount;
    bool overflow = false;
    auto block = [&](int floats) {
        const uint16_t o = next;
        next = uint16_t(std::min<int>(int(next) + floats, 0xFFF0));
        overflow |= next > sf::kBlockSize;
        return o;
    };
    auto put = [&](Ref r, int width) {
        const uint16_t o = block(width);
        if (!overflow)
            _b.output(o, width, _b.convert(r, width));
        return o;
    };
    _out.defaults = nova::SurfaceInputs{};
    _out.defaults.model = nova::SurfaceModel::Graph;
    _out.defaults.set(sf::opacity, nova::spectrum(1.0f));

    // Frames by (normal, tangent).
    std::vector<std::pair<Ref, Ref>> frames;
    auto frameOf = [&](Ref normal, Ref tangent) {
        for (size_t i = 0; i < frames.size(); ++i)
            if (frames[i].first == normal && frames[i].second == tangent)
                return uint8_t(i);
        if (frames.size() >= size_t(nova::ClosureGraph::kMaxFrames)) {
            Warn("frames", "more than 4 distinct normals or tangents in one surface; the rest use the first");
            return uint8_t(0);
        }
        frames.emplace_back(normal, tangent);
        return uint8_t(frames.size() - 1);
    };
    // Closure nodes in post-order (shared nodes once).
    std::map<int, int> index;
    int leaves = 0;
    bool tooMany = false;
    std::function<int(int)> visit = [&](int i) -> int {
        if (const auto it = index.find(i); it != index.end())
            return it->second;
        ClosureNode const &c = _closures[size_t(i)];
        nova::GraphNode gn{};
        gn.op = c.op;
        gn.kind = c.kind;
        gn.flags = c.flags;
        if (c.a >= 0)
            gn.a = uint16_t(visit(c.a));
        if (c.b >= 0)
            gn.b = uint16_t(visit(c.b));
        if (c.film) { // a film layer with nothing to rewrite: no scattering of its own
            Warn("thin_film_bsdf", "thin_film_bsdf outside a layer has no effect");
            gn.kind = nova::LeafKind::Diffuse;
            gn.param = put(Zero(1), 1); // weight 0
            put(Zero(4), 4);
        } else {
            gn.param = next;
            for (Ref r : c.params)
                put(r, _b.width(r));
        }
        if (c.op == nova::GraphOp::Leaf) {
            ++leaves;
            gn.frame = frameOf(c.normal, c.tangent);
        }
        if (G->nodeCount >= nova::ClosureGraph::kMaxNodes) {
            tooMany = true;
            return 0;
        }
        G->node[G->nodeCount] = gn;
        const int k = G->nodeCount++;
        index.emplace(i, k);
        return k;
    };

    Ref opacity = kNoRef;
    int nativeSegments = 0;
    for (SurfaceSegment const &s : segs) {
        nova::GraphSegment gs{};
        gs.model = s.model;
        gs.root = nova::ClosureGraph::kNoNode;
        gs.weight = s.weight != kNoRef ? put(s.weight, 1) : nova::kNoSlot;
        gs.thinWalled = nova::kNoSlot;
        gs.emissionBegin = gs.emissionEnd = G->emissionCount;
        if (s.model != nova::SurfaceModel::Graph) {
            ++nativeSegments;
            gs.base = block(sf::kCount);
            if (!overflow) {
                std::copy(s.defaults->v, s.defaults->v + sf::kCount, _out.defaults.v + gs.base);
                for (auto const &[field, width, ref] : s.fields)
                    _b.output(uint16_t(gs.base + field), width, ref);
            }
        } else {
            if (s.root >= 0)
                gs.root = uint8_t(visit(s.root));
            if (s.thinWalled != kNoRef)
                gs.thinWalled = put(s.thinWalled, 1);
            for (EmissionTerm const &e : s.emission) {
                if (G->emissionCount >= nova::ClosureGraph::kMaxEmission) {
                    Warn("emission", "more than 8 emission terms; the rest are dropped");
                    break;
                }
                nova::GraphEmission &ge = G->emission[G->emissionCount++];
                ge.color = put(e.color, 3);
                ge.schlick = e.schlick != kNoRef ? put(e.schlick, 7) : nova::kNoSlot;
            }
            gs.emissionEnd = G->emissionCount;
        }
        G->segment[G->segmentCount++] = gs;
        // The opacities mix with the segments' weights (Karma: linear).
        const Ref o = s.opacity != kNoRef ? s.opacity : _b.constant(1.0f, 1.0f, 1.0f);
        const Ref wo = s.weight != kNoRef ? _b.mul(o, s.weight) : o;
        opacity = opacity == kNoRef ? wo : _b.add(opacity, wo);
    }
    if (tooMany) {
        _out.error = "the surface has more than 48 closure nodes";
        return false;
    }
    if (overflow) {
        _out.error = "the surface needs more than " + std::to_string(sf::kBlockSize) + " input floats";
        return false;
    }
    G->frameCount = uint8_t(std::max<size_t>(frames.size(), 1));
    for (size_t i = 0; i < frames.size(); ++i) {
        G->frame[i].normal = frames[i].first != kNoRef ? put(frames[i].first, 3) : nova::kNoSlot;
        G->frame[i].tangent = frames[i].second != kNoRef ? put(frames[i].second, 3) : nova::kNoSlot;
    }
    if (frames.empty())
        G->frame[0] = nova::GraphFrame{nova::kNoSlot, nova::kNoSlot};
    if (overflow) {
        _out.error = "the surface needs more than " + std::to_string(sf::kBlockSize) + " input floats";
        return false;
    }
    if (opacity != kNoRef)
        _b.output(sf::opacity, 3, opacity);
    nova::finalizeClosureGraph(*G);
    if (leaves + 8 * nativeSegments > nova::ClosureList::kMaxLobes)
        Warn("lobes", "this surface may need more than 16 lobes; the smallest are dropped where it does");
    _out.graph.model = nova::SurfaceModel::Graph;
    _out.graph.closure = std::move(G);
    // Everything reserved is in use, including native segments' whole
    // blocks (only their mapped inputs are outputs): later slots (material
    // AOVs) must start behind them, as core/scene/material.cpp assumes.
    _out.defaults.size = std::max<uint16_t>(_out.defaults.size, next);
    return true;
}

void Translator::Run() {
    _out.graph.tangentUv = _src.tangentUv.empty() ? primaryUvName() : _src.tangentUv;
    if (!_lib) {
        _out.error = "no MaterialX libraries";
        return;
    }
    // The volume terminal (docs/design/M8.md D4): volume outputs beside the
    // surface's; a material with only a volume shader is complete.
    const bool volume = LowerVolume();
    const auto terminal = _src.net.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminal == _src.net.terminals.end()) {
        if (volume) {
            _out.defaults = nova::lambertDefaults();
            _out.ok = true;
            return;
        }
        _out.error = "no surface terminal";
        return;
    }
    SdfPath shaderPath = terminal->second.upstreamNode;
    auto node = _src.net.nodes.find(shaderPath);
    if (node == _src.net.nodes.end()) {
        _out.error = "the surface terminal's node is missing";
        return;
    }
    mx::NodeDefPtr nd = ResolveNodeDef(node->second.nodeTypeId);
    // Nodes in between: a surfacematerial's surfaceshader input, pass-through
    // dot nodes (Karma's fakecaustics and nesteddielectrics node graphs).
    for (int hops = 0; nd && hops < 16; ++hops) {
        const std::string between = nd->getNodeString();
        if (between != "surfacematerial" && between != "dot")
            break;
        const auto c = node->second.inputConnections.find(TfToken(between == "dot" ? "in" : "surfaceshader"));
        if (c == node->second.inputConnections.end() || c->second.empty()) {
            _out.error = between + " without a surface shader";
            return;
        }
        shaderPath = c->second.front().upstreamNode;
        node = _src.net.nodes.find(shaderPath);
        nd = node != _src.net.nodes.end() ? ResolveNodeDef(node->second.nodeTypeId) : nullptr;
    }
    if (!nd) {
        _out.error = "unknown surface shader '" + (node != _src.net.nodes.end() ? node->second.nodeTypeId.GetString() : "") + "'";
        return;
    }
    const std::string category = nd->getNodeString();
    if (!NativeModel(category) || ForceExpand(category)) {
        // A generic surface (M4.5): pbrlib graphs, surface mixes, nodegraphs.
        if (!EmitGraphSurface(PickOutput(LowerUser(shaderPath), "out")))
            return;
        LowerDisplacement();
        LowerAovs(node->second);
        _out.ok = true;
        return;
    }
    FieldMap const *map = nullptr;
    size_t mapSize = 0;
    if (category == "standard_surface") {
        _out.graph.model = nova::SurfaceModel::StandardSurface;
        _out.defaults = nova::standardSurfaceDefaults(TfStringEndsWith(nd->getName(), "_100"));
        // The Principled Shader's rewrite (principled.h) asks for its sheen
        // and metal with this internal parameter.
        if (node->second.parameters.count(TfToken("nova:houdiniPrincipled")))
            _out.defaults.set(sf::houdiniPrincipled, 1.0f);
        map = kStandardSurface;
        mapSize = std::size(kStandardSurface);
    } else if (category == "open_pbr_surface") {
        _out.graph.model = nova::SurfaceModel::OpenPBR;
        _out.defaults = nova::openPbrDefaults();
        map = kOpenPbr;
        mapSize = std::size(kOpenPbr);
    } else if (category == "surface_unlit") {
        _out.graph.model = nova::SurfaceModel::Unlit;
        _out.defaults = nova::unlitDefaults();
        map = kUnlit;
        mapSize = std::size(kUnlit);
    } else if (category == "UsdPreviewSurface") {
        _out.graph.model = nova::SurfaceModel::UsdPreview;
        _out.defaults = nova::usdPreviewDefaults();
        map = kUsdPreview;
        mapSize = std::size(kUsdPreview);
    } else if (category == "kma_hair" || category == "kma_fur" || category == "kma_fur_2") {
        _out.graph.model = nova::SurfaceModel::Hair;
        _out.defaults = nova::hairDefaults();
        map = kKarmaHair;
        mapSize = std::size(kKarmaHair);
        KarmaHairWarnings(category, node->second);
    } else {
        return;
    }
    // Anisotropy in use (connected, or authored and not 0): the default tangent
    // is Karma's from the primary uv set. Otherwise the tangent is not needed
    // and costs nothing (enabling anisotropy recompiles once).
    bool anisotropic = false;
    for (const char *name : {"specular_anisotropy", "coat_anisotropy", "specular_roughness_anisotropy",
                             "coat_roughness_anisotropy"}) {
        const TfToken token(name);
        const auto value = node->second.parameters.find(token);
        anisotropic |= node->second.inputConnections.count(token) > 0 ||
                       (value != node->second.parameters.end() && std::atof(toText(value->second).c_str()) != 0.0);
    }
    for (mx::InputPtr const &input : nd->getActiveInputs()) {
        FieldMap const *field = nullptr;
        for (size_t i = 0; i < mapSize; ++i)
            if (input->getName() == map[i].input)
                field = &map[i];
        if (!field)
            continue; // e.g. OpenPBR's dispersion scale (dispersion comes after M9)
        const bool connected = node->second.inputConnections.count(TfToken(input->getName())) > 0;
        const bool authored = node->second.parameters.count(TfToken(input->getName())) > 0;
        // Unconnected geometry inputs (normal, tangent) keep the geometric
        // default; the tangent of anisotropic lobes is Karma's.
        if (!connected && !authored && !input->getDefaultGeomPropString().empty()) {
            if (anisotropic && (field->field == sf::tangent || field->field == sf::coatTangent))
                _b.output(field->field, 3, KarmaT());
            continue;
        }
        const Value v = InputOf(node->first, node->second, input);
        if (v.ref != kNoRef)
            _b.output(field->field, field->width, v.ref);
    }
    if (_out.graph.model == nova::SurfaceModel::UsdPreview) {
        // The graph's opacity: "transparent" mode (0) refracts 1 - opacity
        // of the base and cuts out below opacityThreshold; "presence" (1)
        // is a cut-out (above a threshold) or fractional presence.
        auto in = [&](char const *name, int width) {
            mx::InputPtr input = nd->getActiveInput(name);
            const Value v = input ? InputOf(node->first, node->second, input) : Value{};
            return v.ref != kNoRef ? _b.convert(v.ref, width) : Zero(width);
        };
        const Ref opacity = in("opacity", 1), threshold = in("opacityThreshold", 1), mode = in("opacityMode", 1);
        const Ref one = _b.constant(1.0f);
        const Ref presenceMode = _b.binary(Op::Greater, mode, _b.constant(0.5f));
        const Ref hasThreshold = _b.binary(Op::Greater, threshold, Zero(1));
        const Ref cutout = _b.ternary(Op::Select, _b.binary(Op::GreaterEq, opacity, threshold), one, Zero(1));
        const Ref presence = _b.ternary(Op::Select, presenceMode, _b.ternary(Op::Select, hasThreshold, cutout, opacity),
                                        cutout);
        _b.output(sf::opacity, 3, presence);
        const Ref diffuseShare = _b.ternary(Op::Select, presenceMode, one, _b.ternary(Op::Select, hasThreshold, one, opacity));
        _b.output(sf::transmissionWeight, 1, _b.sub(one, diffuseShare));
        // A tangent-space normal (0, 0, 1 by default) through Karma's normal map.
        mx::InputPtr normalInput = nd->getActiveInput("normal");
        const TfToken normalToken("normal");
        const auto mikkt = node->second.parameters.find(TfToken("karma_mikkt"));
        const bool mikktOn = mikkt != node->second.parameters.end() && Enabled(toText(mikkt->second));
        if (mikktOn && normalInput &&
            (node->second.inputConnections.count(normalToken) || node->second.parameters.count(normalToken))) {
            const Ref nn = WorldN();
            _b.output(sf::normal, 3, NgGuard(MikktNormal(in("normal", 3), nn), nn));
        } else if (normalInput && (node->second.inputConnections.count(normalToken) || node->second.parameters.count(normalToken))) {
            const Ref v = in("normal", 3), nn = WorldN(), T = KarmaT(), B = KarmaB();
            const Ref t = _b.normalize(_b.sub(T, _b.mul(nn, _b.dot(nn, T))));
            const Ref bt = _b.normalize(_b.sub(B, _b.mul(nn, _b.dot(nn, B))));
            _b.output(sf::normal, 3,
                      _b.normalize(_b.add(_b.add(_b.mul(t, _b.component(v, 0)), _b.mul(bt, _b.component(v, 1))),
                                          _b.mul(nn, _b.component(v, 2)))));
        }
    }
    if (_out.graph.model == nova::SurfaceModel::OpenPBR) {
        const auto scale = node->second.parameters.find(TfToken("transmission_dispersion_scale"));
        if (scale != node->second.parameters.end() && toText(scale->second) != "0")
            Warn("dispersion", "dispersion comes with spectral rendering (after M9); ignored");
    }
    LowerDisplacement();
    LowerAovs(node->second);
    _out.ok = true;
}

void Translator::LowerDisplacement() {
    // Karma's builders always wire a displacement node, by default with 0:
    // an unconnected zero amount (or scale) does not displace.
    const auto terminal = _src.net.terminals.find(HdMaterialTerminalTokens->displacement);
    if (terminal == _src.net.terminals.end() || ZeroDisplacement(terminal->second.upstreamNode))
        return;
    SdfPath path = terminal->second.upstreamNode;
    TfToken output = terminal->second.upstreamOutputName;
    auto node = _src.net.nodes.find(path);
    // Pass-throughs before the displacement node: a surfacematerial's
    // displacementshader input, dot nodes.
    for (int hops = 0; node != _src.net.nodes.end() && hops < 16; ++hops) {
        const mx::NodeDefPtr nd = ResolveNodeDef(node->second.nodeTypeId);
        const std::string category = nd ? nd->getNodeString() : std::string();
        if (category != "surfacematerial")
            break;
        const auto c = node->second.inputConnections.find(TfToken("displacementshader"));
        if (c == node->second.inputConnections.end() || c->second.empty())
            return;
        path = c->second.front().upstreamNode;
        output = c->second.front().upstreamOutputName;
        node = _src.net.nodes.find(path);
    }
    if (node == _src.net.nodes.end())
        return;
    if (node->second.nodeTypeId == TfToken("UsdPreviewSurface")) {
        // UsdPreviewSurface's displacement: along the normal (object space).
        const mx::NodeDefPtr nd = ResolveNodeDef(node->second.nodeTypeId);
        const mx::InputPtr input = nd ? nd->getActiveInput("displacement") : nullptr;
        if (!input)
            return;
        const Value d = InputOf(path, node->second, input);
        if (d.ref != kNoRef)
            _b.output(sf::kDisplacement, 3, _b.mul(ObjectN(), _b.convert(d.ref, 1)));
        return;
    }
    const Value v = PickOutput(LowerUser(path), output.GetString());
    if (v.closure != Value::Closure::Displacement || v.handle < 0) {
        Warn("displacement", "the displacement shader '" + node->second.nodeTypeId.GetString() +
                                 "' is not supported; the material does not displace");
        return;
    }
    _b.output(sf::kDisplacement, 3, _displacements[size_t(v.handle)]);
}

bool Translator::LowerVolume() {
    namespace vf = nova::volume_field;
    const auto terminal = _src.net.terminals.find(HdMaterialTerminalTokens->volume);
    if (terminal == _src.net.terminals.end())
        return false;
    const auto node = _src.net.nodes.find(terminal->second.upstreamNode);
    if (node == _src.net.nodes.end()) {
        Warn("volume", "the volume terminal's node is missing; volumes render with the default volume");
        return false;
    }
    const Value v = PickOutput(LowerUser(node->first), terminal->second.upstreamOutputName.GetString());
    if (v.closure != Value::Closure::Volume || v.handle < 0) {
        Warn("volume", "the volume shader '" + node->second.nodeTypeId.GetString() +
                           "' is not supported; volumes render with the default volume");
        return false;
    }
    const VolumeShader &s = _volumeShaders[size_t(v.handle)];
    for (auto const &[f, width] : kVolumeFields)
        if (s.field[f] != kNoRef)
            _b.output(uint16_t(vf::kBase + f), width, _b.convert(s.field[f], width));
    return true;
}

// What Karma Hair and Fur do that Nova does not (docs/design/M9.md D6, 2.2):
// the coat (its model is unmeasured), Fur's medulla (Nova renders Fur with
// medulla radius 0, which its help calls equivalent to Karma Hair) and its
// second R lobe. Connected inputs count as used.
void Translator::KarmaHairWarnings(std::string const &category, HdMaterialNode2 const &shader) {
    auto value = [&](char const *name, float fallback) {
        const TfToken token(name);
        if (shader.inputConnections.count(token))
            return 1.0f;
        const auto it = shader.parameters.find(token);
        return it == shader.parameters.end() ? fallback : float(std::atof(toText(it->second).c_str()));
    };
    if (value("coat", 0.0f) != 0.0f)
        Warn(category + ":coat", category + ": coat is not supported yet; rendered without it");
    if (category != "kma_fur" && category != "kma_fur_2")
        return;
    if (value("medulla_radius", 0.5f) != 0.0f)
        Warn("kma_fur:medulla", "kma_fur: the medulla is not supported yet; rendered as Karma Hair (medulla radius 0)");
    const TfToken r2("color_R2");
    const auto c = shader.parameters.find(r2);
    const VtValue color = c != shader.parameters.end() ? VtValue::Cast<GfVec3f>(c->second) : VtValue();
    if (shader.inputConnections.count(r2) ||
        (color.IsHolding<GfVec3f>() && color.UncheckedGet<GfVec3f>() != GfVec3f(0.0f)))
        Warn("kma_fur:R2", "kma_fur: the second R lobe is not supported yet; ignored");
}
void Translator::LowerAovs(HdMaterialNode2 const &shader) {
    // Houdini puts the custom inputs of a Karma AOV node on the surface
    // shader (inputs:aov:<name>), "due to the way Hydra conveys the
    // UsdShade graph" (Karma's help).
    int next = _out.defaults.size;
    for (const sh::IrOutput &o : _out.graph.outputs)
        if (o.field < sf::kBlockSize)
            next = std::max(next, int(o.field) + int(o.width));
    for (auto const &[name, conns] : shader.inputConnections) {
        const std::string &n = name.GetString();
        if (n.rfind("aov:", 0) != 0 || conns.empty())
            continue;
        if (next + 3 > int(sf::kBlockSize)) {
            Warn("aovs", "too many material AOVs; " + n.substr(4) + " and later ones stay 0");
            break;
        }
        const Value v = PickOutput(LowerUser(conns.front().upstreamNode), conns.front().upstreamOutputName.GetString());
        if (v.ref == kNoRef)
            continue;
        _b.output(uint16_t(next), 3, _b.convert(v.ref, 3));
        _out.graph.aovs.emplace_back(n.substr(4), uint16_t(next));
        next += 3;
    }
}

// True if the displacement node at `path` displaces nothing: a MaterialX or
// UsdPreviewSurface displacement whose amount (or scale) is an unconnected 0.
bool Translator::ZeroDisplacement(SdfPath const &path) const {
    const auto it = _src.net.nodes.find(path);
    if (it == _src.net.nodes.end())
        return true;
    HdMaterialNode2 const &node = it->second;
    auto zero = [&](char const *name, bool absentIsZero) {
        const TfToken token(name);
        if (node.inputConnections.count(token))
            return false;
        const auto p = node.parameters.find(token);
        if (p == node.parameters.end())
            return absentIsZero;
        float f[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        parseFloats(p->second.IsHolding<float>() || p->second.IsHolding<double>() ? "float" : "vector3",
                    toText(p->second), f);
        return f[0] == 0.0f && (p->second.IsHolding<float>() || p->second.IsHolding<double>() || (f[1] == 0.0f && f[2] == 0.0f));
    };
    const std::string id = node.nodeTypeId.GetString();
    if (TfStringStartsWith(id, "ND_displacement_"))
        return zero("displacement", true) || zero("scale", false);
    if (id == "UsdPreviewSurface")
        return zero("displacement", true);
    return false;
}

} // namespace

HdNovaTranslatedMaterial HdNovaTranslateMaterial(HdNovaNetworkSource const &src) {
    HdNovaTranslatedMaterial out;
    Translator(src, out).Run();
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
