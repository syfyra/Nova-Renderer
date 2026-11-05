# Hydra materials in Houdini 22 / USD 26.05: what reaches a render delegate, and how Nova should receive them

Everything below was checked in one of these ways:
- reading `$HFS/toolkit/include/pxr` (USD 26.05 headers);
- reading the v26.05 OpenUSD sources (downloaded to `…/hydra-materials/src/`);
- Houdini's own Python and HDA files;
- a probe Hydra delegate I built and ran through `husk` and a standalone UsdImaging driver;
- Karma CPU renders run through `husk`.

"Verified" means I saw the behaviour in a probe log or render. All scratch files are under
`C:/Users/Synti/AppData/Local/Temp/claude/C--Users-Synti-Desktop-renderEngine/ac6add8e-dc52-40d3-838a-501e34988043/scratchpad/m3research/hydra-materials/`, called `<S>` from here on.

---

## 0. Key findings

1. **Nova currently misses every Karma Material Builder material.**
   - `HdNovaRenderDelegate::GetMaterialRenderContexts()` returns `{"nova","mtlx"}` (`hydra/renderDelegate.cpp:377-379`).
   - Houdini authors the Karma Material Builder as `outputs:kma:*`. That builder is the default one, with MaterialX `ND_*` nodes inside.
   - With Nova's current list, such a material falls back to the universal network, i.e. the auto-generated UsdPreviewSurface (seen in husk), or to nothing if there is no preview.
   - Karma's documented priority is `kma > mtlx > vex > universal`. I confirmed it with a Karma render: a material with red `kma`, green `mtlx` and blue universal outputs came out exactly `(1,0,0)`.
2. **Nova does not get materials at all yet.** `material` is not in `SUPPORTED_SPRIM_TYPES`, and the emulation only inserts sprim types the delegate supports (`sceneIndexAdapterSceneDelegate.cpp:268-271`).
3. **Every material edit arrives as AllDirty (0x7c)** under Hydra 2 emulation. That includes a pure value tweak, a texture path change, a rewire, and even an edit on a node no terminal uses. A `DirtyParams`-only fast path never happens; Nova has to diff the network itself.
4. **Material edits never re-sync the meshes bound to them.** This holds even when the edit changes which primvar the material reads (renaming a `geomprop`). Removing a material does not re-sync its meshes either; they keep the stale path.
5. **Hydra no longer tells the delegate which primvars a material needs.** `HdMaterialNetwork::primvars` is always empty. `IsPrimvarFilteringNeeded()` is only read by the legacy `UsdImagingGprimAdapter`, so all authored primvars are delivered anyway. Nova must work out the required primvars itself.
6. **Quirks of `GetMaterialResource` → `HdMaterialNetworkMap` → `HdConvertToHdMaterialNetwork2`:**
   - node paths are material-relative, e.g. `mtlxstandard_surface1` or `NodeGraph1/geomprop1`;
   - terminal `upstreamOutputName` is empty;
   - parameters include `typeName:<p>` and `colorSpace:<p>` pseudo-entries;
   - nodes not connected to a terminal are dropped;
   - Material interface inputs are already flattened into node values;
   - `nodeTypeInfo` is dropped, so VEX `sourceAsset` shaders arrive with an empty identifier.

   Reading `HdMaterialSchema` straight from the terminal scene index avoids all of this. I verified the reader code in §4.2.
7. **Houdini ships MaterialX 1.39.5** (headers, import libs, DLLs). The full data libraries, including SideFX `hmtlx*` node definitions, are in `$HFS/houdini/materialx/libraries`.
   - Loading them takes about 41 ms and yields 898 nodedefs and 282 nodegraphs (measured).
   - Under husk, libHUSD sets `PXR_MTLX_STDLIB_SEARCH_PATHS` to that folder. In `nova_cli` it is not set, so `HdMtlxStdLibraries()` sees a different set of libraries.

---

## 1. Hydra API (USD 26.05, `$HFS/toolkit/include/pxr`)

### 1.1 Material sprim, network structs, conversion

| API | file:line | Notes |
|---|---|---|
| `HdMaterial::DirtyBits` | `imaging/hd/material.h:26-35` | `DirtyParams=1<<2, DirtyResource=1<<3, DirtySurface=1<<4, DirtyDisplacement=1<<5, DirtyVolume=1<<6, AllDirty=0x7c` |
| `HdMaterialRelationship`, `HdMaterialNode`, `HdMaterialNetwork`, `HdMaterialNetworkMap{map, terminals, config}` | `material.h:71-120` | legacy form returned by `GetMaterialResource` |
| `HdMaterialConnection2{upstreamNode, upstreamOutputName}` | `material.h:136-145` | |
| `HdMaterialNode2{nodeTypeId, parameters, inputConnections}` | `material.h:153-158` | `inputConnections` maps each input token to a `vector` (array elements) |
| `HdMaterialNetwork2{nodes, terminals, primvars, config}` | `material.h:165-178` | has `operator==`, useful for exact no-op detection |
| `HdConvertToHdMaterialNetwork2(map, bool* isVolume)` | `material.h:181-184` | Source v26.05 `hd/material.cpp:79-81`: "Assume that the last entry is the terminal"; never sets the terminal output name |
| `HdMaterialNetwork2Interface(materialPath, HdMaterialNetwork2*)` | `hd/materialNetwork2Interface.h:22-33` | `HdMaterialNetworkInterface` view of the struct, for matfilt-style filters and hdMtlx |
| `HdSceneDelegate::GetMaterialResource(id)` / `GetMaterialId(rprimId)` | `hd/sceneDelegate.h:840` / `:835` | |
| `HdRprim::GetMaterialId()` / `SetMaterialId()` | `hd/rprim.h:162` / `:166` | |
| `HdChangeTracker::DirtyMaterialId = 1<<7` | `hd/changeTracker.h:50` | |
| `HdMaterialTerminalTokens`: surface, displacement, volume, light, lightFilter, imageShader, … | `hd/tokens.h:248-255` | |
| `HdTokens->full`, `->preview` | `hd/tokens.h:54`, `:82` | |

### 1.2 Delegate hooks

| Hook | file:line | Behaviour in Hydra 2 (verified) |
|---|---|---|
| `GetMaterialBindingPurpose()` (default `preview`) | `renderDelegate.h:424` | Emulation calls `HdMaterialBindingsSchema::GetMaterialBinding(purpose)`, which falls back to the all-purpose binding (`hd/materialBindingsSchema.cpp:43-51`). husk also passes the render setting `materialBindingPurposes = [full, ""]`. |
| `GetMaterialRenderContexts()` | `renderDelegate.h:437` | Selects one whole network per material: the first matching context, otherwise universal `""` (`hd/materialSchema.cpp`, `GetMaterialNetwork(TfTokenVector)`). No per-terminal merging. |
| `IsPrimvarFilteringNeeded()` | `renderDelegate.h:456` | Only used by `usdImaging/gprimAdapter.cpp:419,432` (legacy delegate). No effect on the scene-index path. |
| `GetShadingSystems()` / `GetShaderSourceTypes()` (deprecated) | `renderDelegate.h:464` / `:473` | Not used by `UsdImagingStageSceneIndex`. Only matters for `HdSiNodeIdentifierResolvingSceneIndex` if you add it yourself. |
| `IsParallelSyncEnabled(primType)` | `renderDelegate.h:551` | Default true only for extComputation (`renderDelegate.cpp:253-259`). Material sprims sync serially unless you opt in (`hd/primTypeIndex.cpp:388-430`). |
| `Update()` | `renderDelegate.h:546` | Called at the start of `SyncAll` under emulation. A hook for observing the terminal scene index directly; optional. |

Sync order (`hd/renderIndex.cpp:1623-1627`): bprims, then sprims (materials included), then tasks, then rprims in parallel (`:1833`). Materials are therefore up to date when meshes sync.

### 1.3 Hydra 2 schemas

- `HdMaterialSchema`:
  - tokens `material`, `""` (universal), `__all`, `terminals` (`hd/materialSchema.h:36-44`);
  - `GetRenderContexts()` (`:155`), `GetMaterialNetwork()` (`:159`);
  - `GetMaterialNetwork(TfToken)` with no fallback (`:165`);
  - `GetMaterialNetwork(TfTokenVector)` with fallback to universal (`:171`).
- `HdMaterialNetworkSchema`: `GetNodes()` (`:78`), `GetTerminals()` (`:83`), `GetInterface()` (`:88`), `GetConfig()` (`:91`).
- `HdMaterialNodeSchema`: `GetParameters`, `GetInputConnections`, `GetNodeIdentifier`, `GetRenderContextNodeIdentifiers`, `GetNodeTypeInfo` (`hd/materialNodeSchema.h:86-115`).
- `HdMaterialNodeParameterSchema`: `GetValue`, `GetColorSpace`, `GetTypeName` (`:69-75`).
- `HdMaterialConnectionSchema` (`:71-74`).
- Container typedefs: `hd/schemaTypeDefs.h:33-60`.
- `HdMaterialBindingsSchema`: tokens `""`/`allPurpose` (`:36-39`); `GetMaterialBinding(purpose)` (`:75`).
- UsdImaging's chain already contains `HdsiLocatorCachingSceneIndex` on the `material` locator and `UsdImagingMaterialBindingsResolvingSceneIndex` (v26.05 `usdImaging/sceneIndices.cpp`). No extra Hydra scene index is needed to receive materials.
- Optional `hdsi` scene indices (append from Nova's `HdSceneIndexPlugin`, as `hydra/implicitSurfaceSceneIndexPlugin.cpp` already does):
  - `HdsiMaterialBindingResolvingSceneIndex::New(in, purposePriorityOrder, dstPurpose)` (`hdsi/materialBindingResolvingSceneIndex.h:26-29`): not needed, because of the built-in all-purpose fallback;
  - `HdsiUnboundMaterialPruningSceneIndex::New(in, inputArgs{materialBindingPurposes})` (`hdsi/unboundMaterialPruningSceneIndex.h:47`): skips translating unused materials in large scenes;
  - `HdSiNodeIdentifierResolvingSceneIndex::New(in, sourceType)` (`hdsi/nodeIdentifierResolvingSceneIndex.h:30-34`): resolves `info:<type>:sourceAsset` to an id through Sdr (relevant to M5 with `"vex"`; needs an Sdr parser for VEX, which only Karma's `sdrKarma*` plugins provide);
  - `HdsiMaterialPrimvarTransferSceneIndex::New(in, composeFn)` (`hdsi/materialPrimvarTransferSceneIndex.h:62-64`): primvars authored on the Material prim; not what Houdini does;
  - `HdsiMaterialRenderContextFilteringSceneIndex` (`:54`): not needed.
- Houdini's own scene-index plugins (`$HFS/houdini/dso/usd_plugins/hdHoudini|hdKarma/resources/plugInfo.json`) mostly use `loadWithRenderer: ["Karma CPU", …]`, so Nova gets none of them. Nova's chain matches nova_cli's chain plus husk's display-style override.

### 1.4 How the emulation builds the network (v26.05 sources)

- `HdSceneIndexAdapterSceneDelegate::GetMaterialResource` (`hd/sceneIndexAdapterSceneDelegate.cpp:1360-1381`) calls `_ToMaterialNetworkMap` (`:1286`), which calls `_Walk` (`:1180-1270`).
- `_Walk` takes only `nodeIdentifier` (`:1203`), so `nodeTypeInfo` is lost.
- Parameters come from `_GetHdParamsFromDataSource` (`:1135`), which adds `colorSpace:<p>` and `typeName:<p>` entries.
- Dirty translation (`hd/dirtyBitsTranslator.cpp:905-935`): any locator under `material` sets `DirtyParams|DirtyResource`. A `material/<ctx>` or `material/__all/...` locator sets AllDirty.
- UsdImaging's `materialAdapter.cpp:249-297` dirties both `material/<ctx>/nodes/<prim>` and `material/__all/...`, which is why every edit ends up as 0x7c.
- `UsdImagingDataSourceMaterial` (`usdImaging/dataSourceMaterial.cpp`):
  - one network per render context named by `outputs:<ctx>:*`, plus `__all` (`:673-698`);
  - nodes only if reachable from a terminal (`_WalkGraph :704-749`);
  - names relative to the material (`:723`);
  - parameter values through `GetValueProducingAttributes()`, so interface inputs are flattened (`:253-299`);
  - connections through NodeGraph outputs are resolved to the shader output (`:374`);
  - `inputs:sourceColorSpace` is folded into the color space of `file` (`:279-283`).
- Prim invalidation: `UsdImagingDataSourceMaterialPrim::Invalidate` (`:1103-1130`).

### 1.5 GeomSubsets (per-face materials)

`HdMeshTopology::GetGeomSubsets()` (`hd/meshTopology.h:167`) returns `HdGeomSubset{type=TypeFaceSet, id, materialId, indices}` (`hd/geomSubset.h:23-43`). The indices are authored face indices. Subsets survive the refine-level copy constructor (`hd/meshTopology.cpp:31-42`), and `operator==` includes them (`:132`).

---

## 2. What Houdini 22 authors, and what a delegate receives

### 2.1 Render contexts per builder

The authoritative source is `help/solaris/kug/materials.txt:46-79`, confirmed by Houdini's translator scripts.

| Builder | Material output | Contents |
|---|---|---|
| Karma Material Builder (default) | `outputs:kma:surface`, `kma:displacement` | MaterialX `ND_*`, `kma_*` and UsdPreview nodes mixed. Subnet spare parm `shader_rendercontextname="kma"` (`$HFS/houdini/otls/OPlibRecipe.hda`); `renderContextName()` in `husd/shadertranslator.py:40-96`. |
| USD MaterialX Builder / plain `mtlx*` VOPs | `outputs:mtlx:*` | pure MaterialX (`mtlxshadertranslator.py:1062-1066`) |
| VEX Material Builder | `outputs:vex:*` | `info:implementationSource="sourceAsset"`, `info:vex:sourceAsset=@opdef:...@`; no `info:id` |
| USD Preview Material Builder, and the Material Library option "Auto-generate Preview Surface" | `outputs:surface` (universal) | `UsdPreviewSurface`, `UsdUVTexture`, `UsdPrimvarReader_*` |
| Houdini Viewport Material Output (H21+) | `hview:surface` | viewport-only (`materials.txt:122`); Nova must not list it |

The output name is `JoinIdentifier(ctx, type)` (`shadertranslator.py:2396`), and render masks map to contexts as `{'RIB':'ri','karma':'kma'}` (`:22-25`).

### 2.2 Authoring details that affect the translator

- **Only non-default parameters are authored** (`hou.py:115432`, `VopNode.usdShaderParms` docstring). Unauthored inputs must take the MaterialX nodedef default, with inheritance applied. Example: `ND_standard_surface_surfaceshader` 1.0.1 has `base=1.0`, `base_color=0.8`; the `_100` variant has `0.8` and `1.0`.
- `config:mtlx:version = "1.39"` through `MaterialXConfigAPI` (`mtlxshadertranslator.py:150-154`). It arrives in `HdMaterialNetwork2::config["mtlx:version"]` (verified).
- Legacy renames done by Houdini: `ND_normalmap` → `ND_normalmap_float` (`:256-257`); Disney nodes → `ND_disney_principled` (`:126-133`). `ND_normalmap_vector3` does not exist in 1.39.
- AOVs: a pattern output feeding a `kma_aov` becomes `inputs:aov:<name>` on the terminal surface shader, plus a `RenderVar` under `<material>/Vars` (`:626-684, :678`). Received as input connection `aov:mymask` (verified).
- Karma render properties: Karma Material Properties are authored as inputs on the terminal surface shader, "because Hydra does not forward them" (`:584-623, :599`; `materials.txt:175`). Expect unknown extra inputs on known nodes.
- The shader visualizer writes `ND_surface_unlit` with `inputs:houdini_visualize_signal=true` (`:853-869`).
- `mtlximage` reads `primvars:st` by default (`help/nodes/vop/mtlximage.txt:22`).
- SOP Import maps `uv` → `st` and `Cd` → `displayColor` (`help/solaris/sop_import.txt:121-124`). There is no aliasing: a `geompropvalue` of `Cd` does not find `displayColor` (`help/solaris/support/aovs.txt:98`).
- Karma colour-space rule (`help/solaris/kug/textures.txt:111-121`): MtlX image nodes with a color3/color4 signature are always "auto" (role in the file name, then file metadata, then 8-bit = sRGB, else linear). Vector or float signatures, or `raw`, mean no transform.
  - USD 26.05 does deliver `colorSpace:file` (verified). Decide on purpose whether Nova honours it or matches Karma.
- `mtlxnormalmap`: Karma uses its own tangent frame unless a `karma_mikkt` input is present and `tangentu`/`signs` primvars exist (`help/nodes/vop/mtlxnormalmap.txt:16-24`).

### 2.3 What the delegate actually receives (probe, contexts `{nova,kma,mtlx}`)

Probe scene: `<S>/probe/scenes/materials.usda`. Logs: `out_husk_kma.txt` (husk) and `out_driver_initial.txt` (UsdImaging driver); both identical in structure.

```
=== Material Sync /materials/mtlxmaterial bits=Params|Resource|Surface|Displacement|Volume (0x7c)
  scene-index render contexts: 'mtlx' '__all'
  HdMaterialNetworkMap: terminals = [ 'mtlxdisplacement1' 'mtlxstandard_surface1' ]  config = { mtlx:version=1.39 }
    node 'mtlximage1' (absolute=0) id='ND_image_color3'
      colorSpace:file = srgb_texture  <TfToken>
      file = @./tex/pattern.exr@ (resolved 'C:\...\scenes\tex\pattern.exr')  <SdfAssetPath>
      filtertype = linear  <string>
      typeName:file = asset  <TfToken>           <- pseudo-parameter, filter it out
    node 'NodeGraph1/geomprop1' (absolute=0) id='ND_geompropvalue_float'   <- NodeGraph flattened, relative path
      geomprop = mask  <string>
    node 'mtlxstandard_surface1' id='ND_standard_surface_surfaceshader'
      metalness = 0.25
      specular_roughness = 0.35                  <- came from Material inputs:rough (interface flattened)
  HdMaterialNetwork2: terminals: displacement<-mtlxdisplacement1.  surface<-mtlxstandard_surface1.   <- output name empty
=== Material Sync /materials/kmamaterial ...  contexts 'kma' '' '__all'
    node 'kma_fake' id='kma_rayimport'      in aov:mymask <- kma_fake.out
=== Material Sync /materials/vexmat ...  node 'principledshader1' id=''   (nodeTypeInfo lost)
```

- The unconnected node `unconnected1` is absent.
- With contexts `{nova,mtlx}`, `kmamaterial` yields `UsdPreviewSurface` (`out_husk_novamtlx.txt`) and `vexmat` yields `void` (no network).
- Meshes deliver all authored primvars. Example: `st interp=FaceVarying role='textureCoordinate' indexed=1`; a constant `displayColor` arrives as a `VtVec3fArray` of size 1.
- UDIM: `resolvedPath` is absolute with `<UDIM>` kept, but only if tile 1001 exists; otherwise it is `''` (`out_udim.txt`).
- GeomSubsets: `geomSubset id=/World/two_faces/faceA materialId='/materials/mtlxmaterial' faces=1` (`out_driver_subsets.txt`).
- Binding purposes: all-purpose plus `preview` with purpose `full` gives the all-purpose material; `full` plus all-purpose gives `full`.

### 2.4 IPR edit behaviour (driver plus USD edits; `<S>/probe/scenes/edits*.txt`, `out_driver_edits.txt`, `out_driver_subsets_edit.txt`)

| USD edit | Material bits | Bound mesh re-sync |
|---|---|---|
| shader input value, Material interface input, texture path | 0x7c | none |
| `geomprop` rename (different primvar) | 0x7c | **none** |
| add or remove a connection | 0x7c | none |
| parameter on a node not reachable from a terminal (network identical) | 0x7c | none |
| `material:binding` rebind | – | `Varying\|MaterialId` |
| GeomSubset rebind | – | `Varying\|Points\|Primvar\|Topology\|Normals` (would rebuild the BLAS) |
| remove the bound Material prim | sprim destroyed | **none**; mesh keeps the dead path |

A resync of an existing material prim marks the sprim AllDirty without recreating it (`sceneIndexAdapterSceneDelegate.cpp:300-318`).

### 2.5 Karma CPU calibration (`<S>/probe/karma/unbound.usda`)

Setup: white dome intensity 1, `diffuselimit=0` (direct only), camera looking straight down, 256 spp. Means over 16×16 pixels:

| Case | Karma result |
|---|---|
| std_surface base=1, base_color=displayColor via geomprop, specular=0, C=(0.5,0.25,0.125) | (0.5007, 0.2503, 0.1252): exactly Lambert |
| std_surface with nothing authored | 0.8089 |
| UsdPreviewSurface diffuse=C / default (0.18) | (0.5139, 0.2733, 0.1530) / 0.2062 |
| unbound mesh, displayColor C | (0.2340, 0.1337, 0.0836) = 0.401·C + 0.033 |
| unbound mesh, no displayColor | 0.4341 (same formula with C=1) |
| kma + mtlx + universal material | (1,0,0): kma wins |

Karma's fallback for unbound geometry is neither Lambert(displayColor) nor UsdPreviewSurface(displayColor). Nova's current Lambert with a 0.18 default differs from it.

---

## 3. How other delegates handle MaterialX, and what Houdini ships

- **hdMtlx** (`hdMtlx/hdMtlx.h`):
  - `HdMtlxSearchPaths()` `:37-39`; `HdMtlxStdLibraries()` `:43-45`; `HdMtlxTexturePrimvarData` `:53-59`; `HdMtlxGetNodeDef` `:70-74`; `HdMtlxGetNodeDefName` `:79-80`;
  - `HdMtlxCreateMtlxDocumentFromHdNetwork(net2, terminalNode, terminalPath, materialPath, libraries, &data)` `:99-107`; `…FromHdMaterialNetworkInterface` `:110-117`.
  - Source v26.05: assumes version `"1.38"` when `config:mtlx:version` is missing, then calls `upgradeVersion()` (`hdMtlx.cpp:732-797`). Search paths come from `UsdMtlxSearchPaths()`: `PXR_MTLX_PLUGIN_SEARCH_PATHS`, then `PXR_MTLX_STDLIB_SEARCH_PATHS`, then the build-time directory, then the usdMtlx plugin's `resources/libraries` (`usdMtlx/utils.cpp:172-224`).
  - Problems seen in the probe:
    - an unknown id (`kma_rayimport`, `ND_normalmap_vector3`) and `aov:*` inputs are emitted as `type="surfaceshader"`;
    - everything goes into one nodegraph `"_"`;
    - under husk, `doc->validate()` fails on Houdini's own libraries ("Node input has too many bindings").
  - Search paths differ by host:
    - husk: `$HFS/houdini/materialx/libraries;<build path>;$HFS/bin/usd_plugins/usdMtlx/resources/libraries`;
    - nova_cli: no Houdini folder, so no `hmtlx*`/`kma_camerafalloff` definitions.
  - Conclusion: use hdMtlx only for debug dumps, not as the main translation path.
- **Storm** (`hdSt/materialXFilter.cpp`): builds a MaterialX document with hdMtlx, then runs GLSL codegen. Primvars come from `geompropvalue` `geomprop` and from texcoord nodes through Sdr `primvars` metadata (`:490-530`). Default UV name: Sdr metadata, falling back to `"st"` (`:928-940`).
- **UsdMtlx Sdr parser** (`usdMtlx/parser.cpp:58-73, 474-485`): primary UV = `UsdUtilsGetPrimaryUVSetName()` = `"st"` (overridable with `USDMTLX_PRIMARY_UV_NAME`). The texcoord `index` is ignored. `$geomprop` is used for `geompropvalue`. Generic query: `SdrShaderNode::GetPrimvars()` / `GetAdditionalPrimvarProperties()` (`usd/sdr/shaderNode.h:337, 346`).
- **hdPrman**: MaterialX networks go through matfilt into MaterialXGenOsl, are compiled to `.oso`, and textures are rewritten to Prman texture nodes.
- **hdArnold** (`arnold-usd/libs/common/materials_utils.cpp`, master): `ND_standard_surface_surfaceshader` maps to Arnold's native `standard_surface` (`:891`); any other `ND_*` becomes an Arnold `osl` node with code from `AiMaterialxGetOslShaderCode` (`:606-651, :893-894`). Image files go through an extra OSL texture-resource node (`:702-737`).
- **Cycles** (`cycles/src/hydra/material.cpp`): reads `HdMaterialSchema` from the scene index, with the `cycles` context or universal; its parameter-only fast path requires `!DirtyResource` (`:239-290`). Under emulation `DirtyResource` is always set, so that path is dead.
- **What Houdini ships for MaterialX:**
  - version: MaterialX 1.39.5, namespace `MaterialX_v1_39_5` (`toolkit/include/MaterialXCore/Generated.h`);
  - headers: `toolkit/include/MaterialX{Core,Format,GenShader,GenOsl,GenGlsl,GenMdl,GenMsl,GenSlang,Render*}`;
  - import libs: `custom/houdini/dsolib/{MaterialXCore,MaterialXFormat,…,libpxr_hdMtlx,libpxr_usdMtlx}.lib`; DLLs in `$HFS/bin`;
  - HoudiniConfig has no targets for these; add imported targets as `NovaHoudini.cmake` does for OIIO;
  - data libraries: `$HFS/houdini/materialx/libraries/{stdlib,pbrlib,bxdf(+lama,translation),cmlib,nprlib,lights,houdini,targets}`. The `houdini/` folder contains 91 nodedefs: `hmtlx*`, `kma_camerafalloff`, legacy 1.38 definitions, `thin_film_bsdf`, …;
  - licence: MaterialX "Modified Apache 2.0" (`help/licenses/MaterialX.txt`). Nova only loads files from the user's `$HFS` and redistributes nothing.
- **Library census** (my count over Houdini's libraries):
  - stdlib: 694 nodedefs, 493 with a native implementation (106 categories), 198 nodegraph-implemented (45 categories: `place2d`, `tiledimage`, `ramp4`, `noise` variants, …);
  - pbrlib: 26 native categories (the BSDF nodes);
  - bxdf: `standard_surface`, `open_pbr_surface` (1.1.1), `UsdPreviewSurface` and `gltf_pbr` are all nodegraphs;
  - houdini: 17 nodegraph-implemented; the rest have no implementation.

---

## 4. Recommended design for `hydra/material/`

### 4.1 Delegate changes

```cpp
// renderDelegate.cpp
const TfTokenVector HdNovaRenderDelegate::SUPPORTED_SPRIM_TYPES = {
    HdPrimTypeTokens->camera, HdPrimTypeTokens->material, /* lights … */ };

TfTokenVector HdNovaRenderDelegate::GetMaterialRenderContexts() const {
    // Karma: kma > mtlx > vex > universal (help/solaris/kug/materials.txt:46).
    // Add "vex" only with the CVEX bridge (M5): before that a VEX network would
    // hide the auto-generated universal preview of the same material.
    static const TfTokenVector c{TfToken("nova"), TfToken("kma"), TfToken("mtlx")};
    return c;
}
bool HdNovaRenderDelegate::IsParallelSyncEnabled(TfToken const &t) const {   // once the translator is thread-safe
    return t == HdPrimTypeTokens->material || HdRenderDelegate::IsParallelSyncEnabled(t);
}
// CreateSprim: if (typeId == HdPrimTypeTokens->material) return new HdNovaMaterial(sprimId);
// GetMaterialBindingPurpose() stays HdTokens->full (husk passes materialBindingPurposes=[full,""]).
```

### 4.2 Getting the network: read the terminal scene index first, `GetMaterialResource` as fallback

Why read the scene index directly:
- Nova can implement its own per-material context chain: if `kma` contains an untranslatable terminal, try `mtlx`, then universal;
- terminal output names are kept;
- `colorSpace` and `typeName` arrive as separate fields;
- `nodeTypeInfo` is kept, which M5 needs.

This code compiles and ran in the probe (`<S>/probe/probe.cpp`, `ReadNetworkFromSceneIndex`; output in `out_si_read.txt`):

```cpp
struct HdNovaParamMeta { TfToken colorSpace, typeName; };
struct HdNovaNetworkSource {
    TfToken context;                       // which render context was used
    HdMaterialNetwork2 net;                // node paths relative to the material
    std::map<SdfPath, std::map<TfToken, HdNovaParamMeta>> meta;
    std::map<SdfPath, VtDictionary> nodeTypeInfo;   // implementationSource, vex:sourceAsset, sdrMetadata …
};

bool HdNovaReadNetwork(HdSceneIndexBaseRefPtr const &si, SdfPath const &id, TfToken const &ctx,
                       HdNovaNetworkSource *out) {
    const HdSceneIndexPrim prim = si->GetPrim(id);
    const HdMaterialNetworkSchema ns = HdMaterialSchema::GetFromParent(prim.dataSource).GetMaterialNetwork(ctx);
    if (!ns) return false;
    out->context = ctx;
    const HdMaterialNodeContainerSchema nodes = ns.GetNodes();
    for (TfToken const &name : nodes.GetNames()) {
        const HdMaterialNodeSchema node = nodes.Get(name);
        const SdfPath path(name.GetString());
        HdMaterialNode2 &n2 = out->net.nodes[path];
        if (HdTokenDataSourceHandle ds = node.GetNodeIdentifier()) n2.nodeTypeId = ds->GetTypedValue(0.0f);
        const HdMaterialNodeParameterContainerSchema params = node.GetParameters();
        for (TfToken const &p : params.GetNames()) {
            const HdMaterialNodeParameterSchema ps = params.Get(p);
            if (HdSampledDataSourceHandle v = ps.GetValue()) n2.parameters[p] = v->GetValue(0.0f);
            HdNovaParamMeta m;
            if (HdTokenDataSourceHandle cs = ps.GetColorSpace()) m.colorSpace = cs->GetTypedValue(0.0f);
            if (HdTokenDataSourceHandle tn = ps.GetTypeName()) m.typeName = tn->GetTypedValue(0.0f);
            if (!m.colorSpace.IsEmpty() || !m.typeName.IsEmpty()) out->meta[path][p] = m;
        }
        const HdMaterialConnectionVectorContainerSchema conns = node.GetInputConnections();
        for (TfToken const &in : conns.GetNames()) {
            const HdMaterialConnectionVectorSchema vec = conns.Get(in);
            for (size_t i = 0; i < vec.GetNumElements(); ++i) {
                const HdMaterialConnectionSchema c = vec.GetElement(i);
                HdTokenDataSourceHandle up = c.GetUpstreamNodePath(), o = c.GetUpstreamNodeOutputName();
                if (up) n2.inputConnections[in].push_back(
                            {SdfPath(up->GetTypedValue(0.0f).GetString()), o ? o->GetTypedValue(0.0f) : TfToken()});
            }
        }
        if (HdContainerDataSourceHandle info = node.GetNodeTypeInfo()) {
            VtDictionary d;
            for (TfToken const &k : info->GetNames())
                if (HdSampledDataSourceHandle v = HdSampledDataSource::Cast(info->Get(k))) d[k] = v->GetValue(0.0f);
            out->nodeTypeInfo[path] = d;
        }
    }
    const HdMaterialConnectionContainerSchema terms = ns.GetTerminals();
    for (TfToken const &t : terms.GetNames()) {
        const HdMaterialConnectionSchema c = terms.Get(t);
        HdTokenDataSourceHandle up = c.GetUpstreamNodePath(), o = c.GetUpstreamNodeOutputName();
        if (up && !up->GetTypedValue(0.0f).IsEmpty())   // an empty path disables a terminal
            out->net.terminals[t] = {SdfPath(up->GetTypedValue(0.0f).GetString()), o ? o->GetTypedValue(0.0f) : TfToken()};
    }
    if (const HdSampledDataSourceContainerSchema cfg = ns.GetConfig())
        for (TfToken const &k : cfg.GetNames())
            if (HdSampledDataSourceHandle v = cfg.Get(k)) out->net.config[k] = v->GetValue(0.0f);
    return true;
}
```

Context chain: try each of `{nova, kma, mtlx}`, then `""`. Use a context if `HdNovaReadNetwork` succeeds and translation produces a known terminal.

Fallback when there is no terminal scene index (test rigs or legacy delegates): call `sd->GetMaterialResource(id)`, then `HdConvertToHdMaterialNetwork2()`. Then:
- move `colorSpace:*` and `typeName:*` entries into `meta`;
- fill missing terminal output names from the nodedef's single output.

### 4.3 Translation pipeline

All of this is host-side `hydra/` code; core sees only the IR.

```
HdNovaNetworkSource
  └─ Canonicalize    : drop pseudo-params; resolve each nodeTypeId to a nodedef
  │                    (USD aliases, legacy renames, config mtlx:version); DFS from the terminals
  │                    in sorted input-name order gives canonical node indices, so node names
  │                    don't matter; every unconnected input of a user node becomes a param slot
  │                    (authored value, or nodedef default via getActiveInputs);
  │                    unconnected defaultgeomprop inputs (UV0 → primary UV "st",
  │                    Nworld/Tworld/Pworld …) become explicit built-in reads
  ├─ Expand          : nodedefs Nova has no native op for, but with a NodeGraph implementation,
  │                    are expanded recursively (memoised per nodedef)
  ├─ Unknown nodes   : warn once per (material, nodeTypeId); outputs = nodedef output default,
  │                    or a zero of the consumer's input type. Unknown terminal → next context
  │                    in the chain, else Nova's default material
  ├─ Lower           : nova::shading::Graph (IR), ParamLayout, TextureRequests,
  │                    AttributeNames, structureHash
  └─ (core) compile  : optimise and emit bytecode → ShaderProgram (shared through ProgramCache)
```

- **Native set for M3.** Implement the 106 stdlib leaf categories generically over component counts, plus closures for `standard_surface`, `open_pbr_surface`, `UsdPreviewSurface`, `surface_unlit` and `displacement`. Make the closures native even though MaterialX defines them as graphs: lobe-exact BSDFs and importance sampling require it. Everything else in stdlib/houdini/cmlib then comes for free through nodegraph expansion.
- **USD id aliases:**

  | USD id | MaterialX nodedef |
  |---|---|
  | `UsdPreviewSurface` | `ND_UsdPreviewSurface_surfaceshader` |
  | `UsdUVTexture` | `ND_UsdUVTexture_23` |
  | `UsdPrimvarReader_float/float2/float3/float4/int/string` | `ND_UsdPrimvarReader_float/vector2/vector3/vector4/integer/string` |
  | `UsdTransform2d` | `ND_UsdTransform2d` |

  Input names are identical; check the output names (`rgb`, `r`, `a`, `result`).
- **Structural vs value inputs.** Structural (part of the hash) are:
  - node types and connections, terminals;
  - inputs that are string, enum, int or bool and also `uniform` in the nodedef (`filtertype`, address modes, `geomprop`, `varname`, texcoord `index`);
  - attribute names.

  Value inputs (not in the hash) are:
  - float, vector and colour inputs;
  - **filename** inputs, which become texture slots;
  - the colour space of a texture, which belongs to the texture request.

  Constant folding may fold expanded-graph internals and literals, but never param slots. Then any value edit reuses the program.
- **Primvars a material needs.** Collect them after expansion:
  - `geompropvalue*` → `geomprop`;
  - `texcoord` → the primary UV (`"st"`, honouring `USDMTLX_PRIMARY_UV_NAME`);
  - `UsdPrimvarReader_*` / `ND_UsdPrimvarReader_*` → `varname` (string or token);
  - `UV0` defaults → the primary UV;
  - `geomcolor` → decide a mapping (unverified).

  These names live in the program as attribute slots and are hashed.

MaterialX calls (verified in `<S>/probe/mtlx_test.cpp`: 65 files, 898 nodedefs, 282 nodegraphs in 40.7 ms; flattening `tiledimage` took 4.1 ms on first use):

```cpp
mx::DocumentPtr lib = mx::createDocument();
mx::FileSearchPath search;  for (auto &p : HdNovaMtlxSearchPaths()) search.append(mx::FilePath(p));
mx::loadLibraries({"stdlib","pbrlib","bxdf","cmlib","nprlib","lights","houdini"}, search, lib);
mx::NodeDefPtr nd = lib->getNodeDef("ND_standard_surface_surfaceshader");
for (mx::InputPtr in : nd->getActiveInputs())   // inheritance applied: base=1.0, base_color=0.8
    use(in->getName(), in->getType(), in->getValueString(), in->getDefaultGeomPropString(),
        in->getIsUniform(), in->getAttribute("enum"));
if (mx::InterfaceElementPtr impl = nd->getImplementation())
    if (mx::NodeGraphPtr ng = impl->asA<mx::NodeGraph>()) { /* recurse: ng->getNodes(), input->getInterfaceName(),
                                                               input->getNodeName(), input->getOutputString() */ }
// Search paths: NOVA_MTLX_SEARCH_PATHS, PXR_MTLX_PLUGIN_SEARCH_PATHS, PXR_MTLX_STDLIB_SEARCH_PATHS, then
// <dir of MaterialXCore.dll>/../houdini/materialx/libraries. ArchGetAddressInfo() returns false on Windows here;
// use GetModuleHandleW(L"MaterialXCore.dll") + GetModuleFileNameW (verified: <S>/probe/locate_test.cpp).
```

Build `HdNovaNodeDefDatabase` once with `std::call_once`. It turns the MaterialX library into immutable POD records (inputs, `VtValue` defaults, kind of implementation). It is the only file that includes MaterialX. Expansions are cached in a `std::mutex`-guarded map keyed by nodedef.

### 4.4 Material sprim, program cache, IPR fast path

```cpp
void HdNovaMaterial::Sync(HdSceneDelegate *sd, HdRenderParam *rp, HdDirtyBits *bits) {
    if (!(*bits & AllDirty) || GetId().IsEmpty()) { *bits = Clean; return; }
    *bits = Clean;
    auto &param = *static_cast<HdNovaRenderParam *>(rp);
    HdNovaMaterialRegistry &reg = param.GetMaterials();
    if (_materialId == nova::kInvalidId) _materialId = reg.Acquire(GetId());   // stable; a mesh may have created it first

    // 1. Hydra-side work while the render keeps running
    HdNovaNetworkSource src;
    const bool found = HdNovaReadMaterial(sd, GetId(), reg.Contexts(), &src);   // §4.2 chain
    if (found && src.context == _context && src.net == _lastNet) return;      // e.g. an edit on a disconnected node
    HdNovaTranslated tr = HdNovaTranslate(src, reg.NodeDefs(), GetId());      // pure function
    reg.Warn(GetId(), tr.diagnostics);                                       // HD_NOVA_WARN once per node type

    nova::MaterialDesc desc;
    desc.program = (_program && tr.structureHash == _structureHash)
                       ? _program                                          // value-only edit: no recompile
                       : reg.Programs().GetOrCompile(tr.structureHash, tr.graph); // shared by structure
    desc.attributeIds = reg.Attributes().Ids(tr.attributeNames);
    desc.params   = std::move(tr.params);                                  // float block in layout order
    desc.textures = param.GetTextureCache().Acquire(tr.textures);          // handles, opened lazily
    if (desc.program == _program && desc.params == _params && desc.textures == _textures) { _lastNet = src.net; return; }

    // 2. Commit: stops the render thread and bumps the edit version → progressive restart
    param.AcquireSceneForEdit().updateMaterial(_materialId, desc);
    _program = desc.program; _structureHash = tr.structureHash; _params = desc.params;
    _textures = desc.textures; _lastNet = std::move(src.net); _context = src.context;
}
void HdNovaMaterial::Finalize(HdRenderParam *rp) {   // meshes are not re-synced: keep the id, show the default
    if (_materialId != nova::kInvalidId)
        static_cast<HdNovaRenderParam *>(rp)->GetMaterials().Orphan(GetId());
}
```

- `HdNovaMaterialRegistry` lives in `HdNovaRenderParam` and is mutex-protected, because meshes sync in parallel. It holds:
  - `SdfPath → MaterialId`; `Acquire()` creates a placeholder with the default program;
  - `ProgramCache` (`structureHash → shared_ptr<const ShaderProgram>`, a map of `shared_future` so two threads don't compile the same graph);
  - the attribute-name registry (`TfToken → uint32`).
- Because the program cache is keyed by content, even a Houdini recook that re-syncs the whole material reuses the program.
- Default material: measure with `mat_unbound_default` (§6) whether to match Karma (0.401·C + 0.033) or keep Lambert(displayColor). Document either choice.

### 4.5 Core side (POD, GPU-portable)

`core/scene/material.h`, `core/shading/program.h` (IR, VM and BSDF are other research topics):

```cpp
struct ShaderProgram {                 // immutable, shared, uploadable as-is
    std::vector<uint32_t> code;         // bytecode
    std::vector<float>    constants;    // folded literals only
    uint32_t paramCount, textureSlots, attributeSlots, registerCount;
    uint32_t flags;                     // emissive, opacity, needsTangents, hasDisplacement …
};
struct MaterialDesc { std::shared_ptr<const ShaderProgram> program; std::vector<float> params;
                      std::vector<uint32_t> textures; std::vector<uint32_t> attributeIds; };
struct MaterialRecord { uint32_t program, paramOffset, textureOffset, attributeOffset, flags; }; // kernel view
// Scene: createMaterial/updateMaterial/removeMaterial; commit() packs the params, textures and attribute ids
// of all materials into scene-wide flat buffers.
```

In the shade stage, a hit resolves `mesh → triangle slot → MaterialId → MaterialRecord`. Hits are partitioned by program (counting sort), and the VM runs once per batch. The result is a POD closure per path, used by NEE and BSDF sampling. There are no virtual calls; the same structure maps onto CUDA in M10.

### 4.6 Mesh side

- Material slots (per-face materials from GeomSubsets):

  ```cpp
  std::vector<SdfPath> slots{GetMaterialId()};                      // slot 0 = the prim's binding
  std::vector<uint16_t> faceSlot(_topology.GetNumFaces(), 0);
  for (HdGeomSubset const &s : _topology.GetGeomSubsets())
      if (s.type == HdGeomSubset::TypeFaceSet && !s.materialId.IsEmpty()) {
          const uint16_t slot = uint16_t(slots.size()); slots.push_back(s.materialId);
          for (int f : s.indices) if (f >= 0 && f < int(faceSlot.size())) faceSlot[f] = slot;
      }
  desc.triangleSlot.resize(triangles.size());
  for (size_t t = 0; t < triangles.size(); ++t)
      desc.triangleSlot[t] = faceSlot[HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(primitiveParams[t])];
  scene.setMeshMaterials(_meshId, reg.Acquire(slots), desc.triangleSlot);  // separate from geometry: a rebind keeps the BLAS
  ```

  - Today `HdNovaMesh::Sync` returns early unless geometry, instances, visibility or primId changed (`mesh.cpp:503`). It must also handle `DirtyMaterialId`.
  - A subset rebind arrives as `Points|Primvar|Topology|Normals`. Compare `_topology.GetPxOsdMeshTopology()` and the invisible faces/points, and reuse the existing `_GeometryUnchanged` VtArray-identity check, so that case does not rebuild the BLAS.
- **Primvars.** Material edits never re-sync meshes, so store all numeric primvars (1-4 floats, any interpolation) as attribute channels in `MeshDesc`, expanded on the host the way `cornerColors` is today, each tagged with a global attribute id from the registry. `Scene::commit` resolves which mesh channel feeds each program's attribute slot for every (mesh slot, material) pair. Later (M7), filter to the union of attributes all materials use, and re-dirty specific meshes through an extended `HdNovaResyncSceneIndex` (per-path list, `primvars` locator) when that union grows. The subdivision path (`core/geometry/subdiv.h`) needs generic primvars and base-face ids for this, not only `displayColor`.
- **Tangents.** Compute Karma-style at hit time: dP/d(st) from the hit triangle's `st` corners, orthonormalised against N. This needs no storage. The MikkT path (`tangentu`, `signs`, `karma_mikkt`) comes later.
- **Asset or string primvars** (`geompropvalueuniform` driving texture paths): per-mesh overrides of texture slots; constant interpolation only.

### 4.7 Thread safety

- If `IsParallelSyncEnabled(material)` is turned on, materials translate concurrently:
  - the NodeDef database is immutable after `call_once`;
  - expansion and program caches are locked;
  - the texture cache is already locked;
  - `Scene` edits are already mutex-guarded.
- Do the heavy work (reading, translating, compiling, opening textures) before `AcquireSceneForEdit()`, as `mesh.cpp` already does.

### 4.8 File layout

`hydra/material/`:
- `material.{h,cpp}`: `HdNovaMaterial`;
- `networkSource.{h,cpp}`: §4.2;
- `nodeDefDatabase.{h,cpp}`: MaterialX, host only;
- `translator.{h,cpp}`: pure function, unit-testable;
- `usdAliases.h`: USD id map, and later the Karma/Houdini tables for M4;
- `registry.{h,cpp}`: ids, programs, attributes, warnings.

`core/shading/`:
- `graph.h`, `compile.{h,cpp}`, `program.h`, `vm.h` (`NOVA_HOST_DEVICE`).

`core/scene/material.h`.

CMake: imported targets for `MaterialXCore`/`MaterialXFormat` (and `libpxr_hdMtlx` only if the debug dump is wanted), linked from `hdNova_objects` only.

---

## 5. Pitfalls

1. Missing `kma` in the context list means Karma Material Builder materials show their preview or nothing (§0.1).
2. Context selection is per material, whole network, with a fallback only to universal. A material with `kma:surface` plus `mtlx:displacement` gets only the `kma` network.
3. Under Hydra 2 every edit is AllDirty, so diff the network (exact `==` first, then the structure hash). Otherwise an edit on a disconnected node restarts the render needlessly.
4. Meshes are never re-synced by material edits or material removal, so never bake material-dependent data into meshes at their sync.
5. GeomSubset rebinds dirty `Points/Primvar/Topology/Normals`, so avoid the BLAS rebuild.
6. `GetMaterialResource` quirks: relative paths, empty terminal output names, `typeName:`/`colorSpace:` pseudo-params, no `nodeTypeInfo` (empty id for VEX), disconnected nodes dropped, interface already flattened.
7. `HdMaterialNetwork::primvars` is empty and `IsPrimvarFilteringNeeded` is ignored (§0.5).
8. Defaults come only from the MaterialX nodedef, because Houdini authors non-defaults only. Use `getActiveInputs()` (version inheritance: standard_surface 1.0.1 vs 1.0.0).
9. Legacy ids: hdMtlx renames only `ND_normalmap`, and assumes 1.38 if the config is missing (Houdini writes `"1.39"`).
10. hdMtlx mis-types unknown nodes and `aov:*` inputs. Houdini's libraries fail `validate()`. `HdMtlxStdLibraries()` differs between husk and nova_cli, so load the libraries yourself.
11. Colour space: Karma documents that color3/color4 `mtlximage` is always "auto", while Hydra 26.05 delivers `colorSpace:file`. Pick a policy and test it against Karma.
12. UDIM `resolvedPath` is empty unless tile 1001 exists; warn when it is empty.
13. `__all` and `hview` contexts exist; never request them.
14. VEX materials in M3: their id is empty, and with `"vex"` in the list they lose their universal preview fallback (§4.1).
15. Extra inputs on known nodes (`aov:*`, Karma render properties, `karma_mikkt`, `houdini_visualize_signal`) must be ignored quietly, not treated as errors. `aov:*` is input for M6.
16. `ArchGetAddressInfo` fails on Windows here; use `GetModuleHandleW`.
17. Karma's default material for unbound geometry is not Lambert(displayColor) (§2.5).

## 6. How to test

- **Unit (Catch2, no Hydra).** Feed the translator `HdMaterialNetwork2` copies of the probe logs. Check:
  - pseudo-params are stripped and relative paths work;
  - the empty terminal output name is fixed;
  - standard_surface defaults are base 1.0 / base_color 0.8;
  - the `ND_normalmap` rename;
  - USD aliases;
  - attribute extraction: `st` from texcoord and from an unconnected image `texcoord`, `mask` from `geompropvalue`, `uv` from a primvar reader's `varname`;
  - an unknown `kma_*` node gives a warning and default outputs, and an unknown terminal falls back to the next context.
- **Structure hash.** A value change keeps the hash and changes only the param block. Renaming nodes keeps the hash, so two materials of the same structure share one program. A new connection, a changed `geomprop` or an enum change gives a new hash.
- **Hydra-level IPR test**, a port of `<S>/probe/driver.cpp`: UsdImaging scene indices, `HdNovaRenderDelegate`, USD edits, `ApplyPendingUpdates`, `HdEngine::Execute`. Assert:
  - a value edit hits the program cache and triggers no mesh sync;
  - a `geomprop` rename rebinds the attribute without a mesh sync, and the image changes;
  - a disconnected-node edit leaves the edit version unchanged;
  - a rebind produces only `MaterialId`;
  - a subset rebind does not rebuild the BLAS (debug counter);
  - removing a material makes its meshes use the default.
- **husk/nova_cli against Karma** (`tests/reference`, `nova_imgcmp`). All under the §2.5 setup (white dome intensity 1, direct only, top-down camera):
  - `mat_context_priority`: expect exactly (1,0,0);
  - `mat_lambert_calib`: exactly C;
  - `mat_ss_defaults`: Karma 0.8089;
  - `mat_preview`: Karma (0.5139, 0.2733, 0.1530) and 0.2062;
  - `mat_unbound_default`: Karma 0.401·C + 0.033, or mark pending if Nova keeps Lambert;
  - also: interface-promoted parameter, GeomSubset two-material mesh, preview vs full binding, `st`/UDIM/address-mode textures, colour-space cases.
- **Furnace (plumbing).** standard_surface with base=1, base_color=1, specular=0 under a uniform dome gives 1.0, whether it arrives through `mtlx`, through `kma`, or through a Material interface input.
- **Debug aid.** Add a TF_DEBUG flag `HDNOVA_MATERIAL` that dumps the received network and the chosen context in the probe's format, optionally with the hdMtlx document.

## Files

All under `<S>/`:
- **Probe delegate (husk/driver-loadable):** `probe/probe.cpp`, `probe/driver.cpp`, `probe/CMakeLists.txt`, `probe/build.bat`, `probe/stage/` (HOUDINI_PATH layout), `probe/run_husk.py`.
- **MaterialX and library-location checks:** `probe/mtlx_test.cpp`, `probe/locate_test.cpp`.
- **Scenes and edit scripts:** `probe/scenes/{materials,materials_husk,subsets}.usda`, `edits.txt`, `edits_subsets.txt`.
- **Logs:** `probe/out_husk_kma.txt`, `out_husk_novamtlx.txt`, `out_husk_settings.txt`, `out_driver_initial.txt`, `out_driver_edits.txt`, `out_driver_subsets*.txt`, `out_si_read.txt`, `out_udim.txt`.
- **Karma calibration:** `probe/karma/unbound.usda`, `probe/karma/run_karma.py`, `probe/karma/Render_*.exr`.
- **OpenUSD v26.05, Cycles and Arnold sources:** `src/`. **Extracted Houdini help:** `help/`.