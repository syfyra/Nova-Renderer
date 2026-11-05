#include "material.h"

#include "registry.h"
#include "translator.h"
#include "principled.h"
#include "vexBridge.h"
#include "vexShaders.h"

#include "../diagnostics.h"
#include "../renderParam.h"

#include "core/shading/compiler.h"

#include <pxr/base/tf/debug.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>

#include <algorithm>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

bool sameInputs(nova::SurfaceInputs const &a, nova::SurfaceInputs const &b) {
    return a.model == b.model && std::memcmp(a.v, b.v, sizeof(a.v)) == 0;
}

uint64_t hashBytes(uint64_t h, void const *p, size_t n) {
    auto const *b = static_cast<unsigned char const *>(p);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// The material's displacement for meshes (M4.6): null when it does not
// displace (no program, or a uniform zero offset for these values).
// The material's tag in light path expressions: karma:lpetag on the
// surface shader (an object's own tag wins, docs/design/M6.md D4).
std::string lpeTagOf(HdNovaNetworkSource const &src) {
    const auto terminal = src.net.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminal == src.net.terminals.end())
        return {};
    const auto node = src.net.nodes.find(terminal->second.upstreamNode);
    if (node == src.net.nodes.end())
        return {};
    const auto p = node->second.parameters.find(TfToken("karma:lpetag"));
    if (p == node->second.parameters.end())
        return {};
    if (p->second.IsHolding<std::string>())
        return p->second.UncheckedGet<std::string>();
    if (p->second.IsHolding<TfToken>())
        return p->second.UncheckedGet<TfToken>().GetString();
    return {};
}

HdNovaDisplacementPtr displacementOf(nova::MaterialDesc const &desc) {
    if (!desc.program || !desc.program->displacement)
        return nullptr;
    auto d = std::make_shared<HdNovaDisplacement>();
    d->shader.program = desc.program->displacement;
    d->shader.program->initUniforms(desc.params.size() >= d->shader.program->paramCount ? desc.params.data() : nullptr,
                                    d->shader.uniforms);
    if (nova::displacementIsZero(d->shader))
        return nullptr;
    for (size_t i = 0; i < d->shader.program->textures.size(); ++i)
        d->shader.samplers.push_back(
            {i < desc.textures.size() ? desc.textures[i] : nova::kNoTexture, d->shader.program->textures[i]});
    // The hash covers what the displacement reads: its structure, the
    // parameter values it reads (a surface-only value edit leaves it) and the
    // textures.
    nova::shading::Program const &p = *d->shader.program;
    uint64_t h = hashBytes(1469598103934665603ull, &p.structureHash, sizeof(uint64_t));
    for (auto const &[offset, width] : p.paramReads)
        for (uint32_t k = 0; k < width; ++k)
            if (offset + k < desc.params.size())
                h = hashBytes(h, &desc.params[offset + k], sizeof(float));
    h = hashBytes(h, desc.textures.data(), desc.textures.size() * sizeof(nova::TextureId));
    d->hash = h | 1; // never 0 (0: none)
    return d;
}

// The names the material's volume shader reads (docs/design/M8.md D5), in
// the order of first use; empty without a volume shader.
std::vector<std::string> volumeFieldsOf(nova::MaterialDesc const &desc) {
    std::vector<std::string> names;
    if (!desc.program || !desc.program->volume)
        return names;
    nova::shading::Program const &v = *desc.program->volume;
    for (nova::shading::PrimvarBinding const &b : v.primvarBindings)
        if (std::find(names.begin(), names.end(), v.primvars[b.name]) == names.end())
            names.push_back(v.primvars[b.name]);
    return names;
}

const TfToken vexContext("vex");

// The VEX shader on the network's surface terminal, if it is one.
HdNovaVexShader surfaceVexShader(HdNovaNetworkSource const &src, SdfPath *node = nullptr) {
    const auto terminal = src.net.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminal == src.net.terminals.end())
        return {};
    if (node)
        *node = terminal->second.upstreamNode;
    const auto info = src.nodeTypeInfo.find(terminal->second.upstreamNode);
    return info != src.nodeTypeInfo.end() ? HdNovaVexShaderOf(info->second) : HdNovaVexShader{};
}

} // namespace

HdNovaMaterial::HdNovaMaterial(SdfPath const &id) : HdMaterial(id) {}

void HdNovaMaterial::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) {
    const HdDirtyBits bits = *dirtyBits;
    *dirtyBits = HdMaterial::Clean;
    SdfPath const &id = GetId();
    if (!(bits & HdMaterial::AllDirty) || id.IsEmpty())
        return;
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    HdNovaMaterialRegistry &registry = param->GetMaterials();

    // The first render context whose network translates wins; the
    // universal network is the last resort, the default material after it.
    const HdSceneIndexBaseRefPtr sceneIndex = sd->GetRenderIndex().GetTerminalSceneIndex();
    TfTokenVector chain = HdNovaMaterialRenderContexts();
    chain.push_back(TfToken()); // universal
    nova::MaterialDesc desc = nova::defaultMaterial();
    bool translated = false;
    HdNovaDisplacementPtr vexDisplacement; // a VEX displace function through the bridge (D8)
    for (TfToken const &context : chain) {
        HdNovaNetworkSource src;
        const bool found = sceneIndex ? HdNovaReadNetwork(sceneIndex, id, context, &src)
                                      : (context == chain.front() && HdNovaReadNetworkFromDelegate(sd, id, &src));
        if (!found)
            continue;
        if (_haveSource && src == _source)
            return; // unchanged, e.g. an edit on a node no terminal uses
        TF_DEBUG(HDNOVA_MATERIAL).Msg("Nova: material %s, %s", id.GetText(), HdNovaDescribeNetwork(src).c_str());
        // VEX shaders (docs/design/M5.md): the Principled Shader natively as a
        // MaterialX network, other VEX code through the CVEX bridge.
        HdNovaNetworkSource rewritten;
        HdNovaNetworkSource const *network = &src;
        HdNovaDisplacementPtr displacement;
        if (context == vexContext) {
            SdfPath vexNode;
            const HdNovaVexShader vex = surfaceVexShader(src, &vexNode);
            std::string displacementError;
            displacement = HdNovaBuildExternalDisplacement(src, &displacementError);
            if (!displacementError.empty())
                registry.Warn(id, "render context 'vex': the displacement does not run (" + displacementError +
                                      "); the surface renders undisplaced");
            if (vex.kind == HdNovaVexKind::PrincipledSurface) {
                std::vector<std::string> warnings;
                HdNovaRewritePrincipled(src, &rewritten, &warnings);
                for (std::string const &w : warnings)
                    registry.Warn(id, w);
                TF_DEBUG(HDNOVA_MATERIAL)
                    .Msg("Nova: material %s, Principled Shader as %s", id.GetText(),
                         HdNovaDescribeNetwork(rewritten).c_str());
                network = &rewritten;
            } else if (vex.kind == HdNovaVexKind::Code) {
                // A VEX Material Builder: the CVEX bridge (D4).
                HdNovaExternalMaterial ext;
                std::string why;
                if (!HdNovaBuildExternal(src, vexNode, vex, &ext, &why)) {
                    registry.Warn(id, "render context 'vex': " + why + "; the next render context is used");
                    continue;
                }
                nova::shading::CompileResult compiled = nova::shading::compile(ext.graph);
                if (!compiled.program) {
                    registry.Warn(id, "render context 'vex': the parameter program does not compile: " +
                                          compiled.error);
                    continue;
                }
                TF_DEBUG(HDNOVA_MATERIAL)
                    .Msg("Nova: material %s runs VEX through the CVEX bridge: %zu parameter(s), %u per hit\n",
                         id.GetText(), ext.binding->shader->params().size(), unsigned(ext.blockSize));
                desc.inputs = nova::SurfaceInputs{};
                desc.inputs.model = nova::SurfaceModel::External;
                desc.inputs.size = ext.blockSize;
                desc.inputs.set(nova::surface_field::opacity, nova::spectrum(1.0f)); // opaque (D9)
                desc.program = registry.Intern(compiled.program);
                desc.params = std::move(ext.params);
                desc.external = std::move(ext.binding);
                vexDisplacement = std::move(displacement);
                _source = std::move(src);
                _haveSource = true;
                translated = true;
                break;
            } else if (vex.kind != HdNovaVexKind::NotVex) {
                registry.Warn(id, "render context 'vex': " + HdNovaDescribeVexShader(vex) +
                                      " does not run in Nova; the next render context is used");
                continue;
            }
        }
        HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(*network);
        for (std::string const &w : tr.warnings)
            registry.Warn(id, w);
        if (!tr.ok) {
            registry.Warn(id, "render context '" + context.GetString() + "': " + tr.error);
            continue;
        }
        nova::shading::CompileOptions options;
        options.volumeParams = &tr.params; // the volume shader with the values folded (docs/design/M8.md D4)
        nova::shading::CompileResult compiled = nova::shading::compile(tr.graph, options);
        if (!compiled.program) {
            registry.Warn(id, "render context '" + context.GetString() + "' does not compile: " + compiled.error);
            continue;
        }
        if (!compiled.warning.empty())
            registry.Warn(id, compiled.warning);
        TF_DEBUG(HDNOVA_MATERIAL)
            .Msg("Nova: material %s uses context '%s': %zu instruction(s), %zu prologue, %u row(s), %zu param(s)\n",
                 id.GetText(), context.GetText(), compiled.program->code.size(), compiled.program->prologue.size(),
                 compiled.program->rowCount, tr.params.size());
        if (compiled.program->volume) {
            std::vector<std::string> reads;
            for (nova::shading::PrimvarBinding const &b : compiled.program->volume->primvarBindings)
                reads.push_back(compiled.program->volume->primvars[b.name]);
            TF_DEBUG(HDNOVA_MATERIAL)
                .Msg("Nova: material %s has a volume shader: %zu instruction(s), %zu prologue, reads %s\n",
                     id.GetText(), compiled.program->volume->code.size(), compiled.program->volume->prologue.size(),
                     TfStringJoin(reads, ", ").c_str());
        }
        desc.inputs = tr.defaults;
        desc.program = registry.Intern(compiled.program);
        desc.params = std::move(tr.params);
        if (!tr.textures.empty()) {
            // Preparing a texture may convert it; the render must not sample
            // while the registry changes.
            param->StopRenderForEdit();
            for (HdNovaTextureRequest const &request : tr.textures)
                desc.textures.push_back(param->GetSurfaceTextures().Acquire(request));
        }
        vexDisplacement = std::move(displacement);
        _source = std::move(src);
        _haveSource = true;
        translated = true;
        break;
    }
    if (!translated)
        _haveSource = false; // nothing usable: the default material
    else
        desc.lpeTag = lpeTagOf(_source);

    const uint64_t vexDisplacementHash = vexDisplacement ? vexDisplacement->hash : 0;
    if (_hasApplied && desc.program == _applied.program && desc.params == _applied.params &&
        desc.textures == _applied.textures && sameInputs(desc.inputs, _applied.inputs) && !desc.external &&
        desc.lpeTag == _applied.lpeTag &&
        !_applied.external && vexDisplacementHash == _appliedVexDisplacement)
        return; // e.g. a recook that produced the same material
    registry.SetDisplacement(id, vexDisplacement ? vexDisplacement : displacementOf(desc));
    registry.SetVolumeFields(id, volumeFieldsOf(desc));
    _appliedVexDisplacement = vexDisplacementHash;
    nova::Scene &scene = param->AcquireSceneForEdit();
    desc.cryptoId = param->GetCryptoNames().Note("__materialname", id.GetString()); // cryptomatte (D10)
    scene.updateMaterial(registry.Acquire(id, scene), desc);
    _applied = std::move(desc);
    _hasApplied = true;
}

void HdNovaMaterial::Finalize(HdRenderParam *renderParam) {
    // Meshes are not re-synced when their material goes away: they keep the
    // id, which now renders the default material.
    if (!_hasApplied || GetId().IsEmpty())
        return;
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    param->GetMaterials().SetDisplacement(GetId(), nullptr);
    param->GetMaterials().SetVolumeFields(GetId(), {});
    nova::Scene &scene = param->AcquireSceneForEdit();
    scene.updateMaterial(param->GetMaterials().Acquire(GetId(), scene), nova::defaultMaterial());
    _hasApplied = false;
}

PXR_NAMESPACE_CLOSE_SCOPE
