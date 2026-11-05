# M6 research: Cryptomatte in Solaris, Karma and husk (Houdini 22.0.466)

Scope: docs/PLAN.md M6, "Utility (... crypto-matte)" and "Multi-Part EXR via husk".
The report answers four questions from measurements on this machine:
1. What Solaris authors in USD when a user asks Karma for cryptomattes.
2. What Karma writes into the EXR: parts, channels, header metadata, hashes.
3. How the metadata travels from a Hydra delegate to husk's EXR writer, and whether a third-party delegate can use the same route.
4. Which names are hashed, and how per-pixel coverage is accumulated.

Sources: the Houdini install (`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`, headers in `$HFS/toolkit/include`, help zips in `$HFS/houdini/help`), hython exports, husk renders (Karma CPU, Nova, and a probe delegate built for this report) and `hoiiotool --info -v -a`.
Every Houdini process ran isolated (own preferences, no user packages, installed Nova skipped); the user's session was not touched.
Probe scripts and scenes are in `probes/crypto_*` (list and commands in section 8). They contain no SideFX code.

---

## 0. Findings in short

1. **Authoring.** The Karma Cryptomatte LOP (`karmacryptomatte`) creates one RenderVar per ID type under `/Render/Products/Vars` and, by default, appends it to the existing product's `orderedVars` (same EXR as the beauty). Each var is a primvar AOV with three Karma driver parameters:
   - `sourceType = "primvar"`, `sourceName` = `__name` (prims), `__materialname` (materials), `cryptomatte:kind:<VarName>` (kind) or the user primvar name;
   - `dataType = "float3"`, `driver:parameters:aov:husk:format = "float3"`, `driver:parameters:aov:husk:multiSampled = 0`;
   - `driver:parameters:aov:karma:cryptomatte = 1`, `driver:parameters:aov:karma:cryptomatterank = 6`, `driver:parameters:aov:karma:cryptomattesidecar = ""`, `driver:parameters:aov:karma:filter = '["ubox",{}]'`.
   The Render Var LOP has the same three `karma:cryptomatte*` fields ("Enable Cryptomatte", "Cryptomatte Max Overlap", "Cryptomatte Manifest File"). The Karma Render Settings LOP has no cryptomatte parameters of its own.
2. **EXR layout** (multi-part, husk default). Per crypto var: one part `<Var>` (channels `<Var>.R/G/B`, float, a hash-colour preview) followed by `ceil(rank/2)` parts `<Var>00`, `<Var>01`, ... (channels `<Var>NN.R/G/B/A`, float) holding (id, coverage, id, coverage). Rank 6 gives 3 layers, rank 3 gives 2, rank 1 gives 1. With `--exrmode 0` all channels go into one part.
3. **Metadata** sits in the header of the product's first part: `cryptomatte/<key>/name`, `/hash` = `MurmurHash3_32`, `/conversion` = `uint32_to_float32`, and either `/manifest` (embedded JSON) or `/manif_file` (sidecar JSON next to the product).
4. **Hashing verified** on every manifest entry and pixel id of 7 analysed Karma renders (up to 238 names per crypto): id = MurmurHash3_x86_32(UTF-8 name, seed 0); if the exponent bits are 0 or 255, bit 23 is flipped. The manifest stores these fixed-up bits as 8 lowercase hex digits; the pixels store the same bits as float. `<key>` = the first 7 hex digits of MurmurHash3(layer name), with no fixup. Two names needed the fixup, and Karma applied it in both.
5. **Delegate → husk:** generic. A delegate's `HdRenderBuffer::GetResource()` returns an `HdAovSettingsMap` holding `"extra_aov_resource"` → `std::shared_ptr<void>` to a `UT_HUSDExtraAOVResource` (`$I/UT/UT_HUSDExtraAOVResource.h`, Apache-2.0, header-only). husk then:
   - writes every extra plane as its own part;
   - merges every metadata pair into the header (any key, not only `cryptomatte/...`);
   - writes the sidecar itself when both `manif_file` and `manifest` are given.
   A probe delegate (`probes/crypto_probe_delegate`) built against Houdini's USD reproduced Karma's layout exactly. husk itself computes nothing for cryptomatte: Nova today gets four all-zero `float3` parts and no metadata.
6. **Names** (prim/material/primvar/kind):
   - prims: the USD path of the gprim;
   - point-instancer instances: the prototype prim path, shared by all instances;
   - native instances: the first instance's path plus the prototype-relative path, shared by all instances;
   - materials: the bound material path (per face with GeomSubsets), `defaultshader` when unbound;
   - primvars: the string value, or an int as its decimal string; constant, uniform and per-instance values all work;
   - kind: a constant string primvar the LOP authors (`default` or the kind prim's path).
   A delegate gets the Karma names from Hydra 2's `HdPrimOriginSchema` and `HdInstancerTopologySchema::instanceLocations`, not from `GetScenePrimPath`.
7. **Coverage:**
   - Per camera sample, each hit id gets weight 1 (opaque) or its opacity share along the camera path (a 50 % card over a sphere gives 0.5 / 0.5).
   - The pixel value is the plain average over the pixel's samples ("ubox"); filter importance sampling places the samples, so it also follows the pixel filter.
   - The coverage sum equals the beauty alpha (max diff 2.2e-4, the half-float precision of the alpha).
   - Pairs are sorted by coverage, descending, and cut at `rank` without renormalization. Empty slots are (0, 0).
   - The preview part is Σ coverage · (h/2³², (h≪8 mod 2³²)/2³², (h≪16 mod 2³²)/2³²) over **all** ids of the pixel, including those past the rank.

## 1. Authored USD (hython, `probes/crypto_export.py`)

The Karma Cryptomatte LOP in its default mode "Add to Existing Render Product", downstream of a Karma Render Settings LOP, with all four ID types on (primvar `asset_name`). The layer is abridged: per-attribute `customData` is removed.

```
over "Render" { over "karmarendersettings1" { over "Products" { def RenderProduct "renderproduct" {
    rel orderedVars = [</Render/karmarendersettings1/Vars/beauty>, </Render/Products/Vars/CryptoPrimitives>,
        </Render/Products/Vars/CryptoMaterials>, </Render/Products/Vars/CryptoKind_component>,
        </Render/Products/Vars/CryptoPrimvar_asset_name>]
    string driver:parameters:husk:OpenEXR:compression = "zips" ... } } }
  def Scope "Products" { def Scope "Vars" {
    def RenderVar "CryptoPrimitives" (prepend apiSchemas = ["KarmaRenderVarAPI", "HuskRenderVarAPI"]) {
        token dataType = "float3"
        float driver:parameters:aov:husk:clearValue = 0
        token driver:parameters:aov:husk:format = "float3"
        bool driver:parameters:aov:husk:multiSampled = 0
        string driver:parameters:aov:husk:name = "CryptoPrimitives"
        bool driver:parameters:aov:karma:cryptomatte = 1
        int driver:parameters:aov:karma:cryptomatterank = 6
        string driver:parameters:aov:karma:cryptomattesidecar = ""
        string driver:parameters:aov:karma:filter = '["ubox",{}]'
        string sourceName = "__name"
        token sourceType = "primvar" }
    def RenderVar "CryptoMaterials"          { ... sourceName = "__materialname" ... }
    def RenderVar "CryptoKind_component"     { ... sourceName = "cryptomatte:kind:CryptoKind_component" ... }
    def RenderVar "CryptoPrimvar_asset_name" { ... sourceName = "asset_name" ... } } } }
over "World" {
    string primvars:cryptomatte:kind:CryptoKind_component = "default" (interpolation = "constant")
    over "geo" { over "assetC" {   # kind = "component"
        string primvars:cryptomatte:kind:CryptoKind_component = "/World/geo/assetC" (interpolation = "constant") } } }
```

- **LOP parameters** (Max Overlap, Multi Sampled, Sidecar File, Sample Blending) map 1:1 to the attributes above.
  - Sample Blending offers `ubox` (default), `accumulate` and several `minmax` modes.
  - Mode 1, "Create Render Product", authors a separate `RenderProduct /Render/Products/<node>` with `productName` = Output Picture and `orderedVars` = the crypto vars, and appends it to the settings' `products`. Its EXR carries the crypto metadata in its own first part.
  - Mode 2 creates one product per ID (from the help; not rendered).
- **Kind IDs** are ordinary primvar cryptomattes. The LOP writes an inherited constant string primvar on the root prims (`"default"`) and on each matching kind prim (its path).
- **The Render Var LOP** with "Enable Cryptomatte" authors the same `driver:parameters:aov:karma:cryptomatte`, `...cryptomatterank` and `...cryptomattesidecar` attributes. The comment in `UT_HUSDExtraAOVResource.h` still shows the older spelling `driver:parameters:aov:cryptomatte`, without `karma:`.
- **Help** (`nodes.zip:lop/karmacryptomatte.txt`): "Third-party renderers are responsible for implementing Cryptomatte support in their Hydra render delegates." Primvar IDs must be integer or string with constant or uniform interpolation. Per-instance (instancer, `vertex`) values also worked (section 5).

## 2. What Karma writes (husk `--renderer BRAY_HdKarma`, `hoiiotool --info -v -a`)

The scene has two spheres, a cube under a `component` Xform, a point instancer with two prototypes, two materials and a string primvar. It renders at 160×90 and 9 spp. The product holds the beauty and four crypto vars at rank 6, which makes 17 parts:

```
part 0  "C"                      R,G,B,A (half)
part 1  "CryptoPrimitives"       CryptoPrimitives.R/G/B (float)           preview
part 2  "CryptoPrimitives00"     CryptoPrimitives00.R/G/B/A (float)       (id0, cov0, id1, cov1)
part 3  "CryptoPrimitives01"     ...                                       (id2, cov2, id3, cov3)
part 4  "CryptoPrimitives02"     ...                                       (id4, cov4, id5, cov5)
part 5  "CryptoMaterials" ... part 16 "CryptoPrimvar_asset_name02"
header of part 0:
  cryptomatte/b5b7a6b/name: "CryptoPrimitives"
  cryptomatte/b5b7a6b/hash: "MurmurHash3_32"
  cryptomatte/b5b7a6b/conversion: "uint32_to_float32"
  cryptomatte/b5b7a6b/manifest: "{"/World/geo/assetC/boxC":"cd188f4c","/World/inst/protos/pebble":"ac608955",
      "/World/geo/ballA":"aabbd798","/World/inst/protos/rock":"8082046e","/World/geo/ballB":"1f492b24"}"
  cryptomatte/f93da7f/name: "CryptoMaterials"      manifest {"/World/mtl/red":"f13a94bf","/World/mtl/blue":"9c9743fa"}
  cryptomatte/cd8c1c1/name: "CryptoKind_component" manifest {"/World/geo/assetC":"d45b5e00","default":"42c9679f"}
  cryptomatte/a6c3e25/name: "CryptoPrimvar_asset_name" manifest {"alpha":"a9c0133d","beta":"789069a9"}
```

- **Sidecar.** With `cryptomattesidecar = "crypto_primvar_manifest.json"`, `/manifest` is replaced by `cryptomatte/a6c3e25/manif_file: "crypto_primvar_manifest.json"`. The JSON (the same `{name: hex}` map) lands next to the product, also when `husk -o` redirects the product into another directory.
- **Embedded manifests** list only the ids that occur.
- **Other header keys** are Karma's `husk:render_stats` JSON. Its `aov_buffers` list contains `"cryptomatte": true, "source": "primvar:__name"`.
- **multiSampled** (`husk:multiSampled` 0 vs 1) made no difference to Karma's crypto output (the files were identical in every statistic).

## 3. Hash check (`probes/crypto_analyze.py`)

The checks cover every manifest entry, every pixel id and every metadata key, in 7 analysed Karma renders with 4–5 crypto vars each and up to 238 manifest entries.

| check | result |
|---|---|
| key = first 7 hex of mmh3(layer name) | all match (`CryptoPrimitives` → b5b7a6bc → `b5b7a6b`; `CryptoMaterials` → `f93da7f`; `CryptoPrimvar_piece` → `01f46c3`) |
| manifest hex = mmh3(name) with the exponent fixup | all match. `/World/inst/protos/rock`: raw `8002046e` (exponent 0) → manifest `8082046e`; `c284`: → `809f0832`. Without the fixup these two fail. |
| pixel id bits = manifest value | every non-zero-coverage id is in the manifest (0 unknown ids) |
| int primvar | hashed as its decimal string: `{"42":"bc58a436","-3":"1826e92f","7":"23ea8628"}` |
| preview RGB | = Σᵢ covᵢ · (hᵢ/2³², (hᵢ≪8 mod 2³²)/2³², (hᵢ≪16 mod 2³²)/2³²), max error 0 for pixels below the rank limit; hᵢ = the stored (fixed-up) bits |

## 4. Delegate → husk interface

**What the delegate receives.** The probe delegate prints `HdRenderPassAovBinding::aovSettings` for each binding. For a crypto var it gets:
- every RenderVar attribute verbatim: `dataType`, `sourceName`, `sourceType` and `sourcePrim` (the RenderVar path);
- all `driver:parameters:aov:*` keys, including `driver:parameters:aov:karma:cryptomatte` (bool), `...cryptomatterank` (int), `...cryptomattesidecar` (string) and `...karma:filter` (string).

`binding.aovName` is the `husk:name` (`CryptoPrimitives`). The buffer is allocated as `HdFormatFloat32Vec3` (from `husk:format float3`); an authored `multiSampled = 1` arrived as `Allocate(..., multiSampled = true)`. `GetDefaultAovDescriptor` is called for each name, but husk allocates the buffer even when Nova returns an invalid descriptor.

**What husk asks back** (`$I/UT/UT_HUSDExtraAOVResource.h`; `$I/HUSD/HUSD_RenderBuffer.h` declares `mergeMetaData`, `numExtraBuffers`, `extraBuffer` and `extraName`). The probe measured this call sequence:
1. After the render converged, once per image write, husk calls `GetResource(true)` on **every** bound buffer, the beauty included.
2. If the returned `VtValue` holds an `HdAovSettingsMap` with key `extra_aov_resource` → `VtValue(std::shared_ptr<void>)`, husk casts it to `UT_HUSDExtraAOVResource`. It calls `myMap(i)`, writes plane i as part `myNames[i]` with format `myFormats[i]` (an `HdFormat` stored as int), then calls `myUnmap(i)`.
   - The extra parts follow the parent AOV's own part.
   - Plane memory has the same bottom-up row order as `Map()`; husk flips it like the main buffers.
3. `myMetadata` (map<string,string>) is merged into the header of the product's first part.
   - Keys are not filtered: `probe:extra` appeared too, and a beauty buffer returning only metadata (no planes) works.
   - When two buffers send the same key, the later buffer's value wins.
4. If the metadata has both `cryptomatte/<k>/manif_file` and `.../manifest`, husk writes the manifest string to `<product dir>/<manif_file>` and keeps only `manif_file` in the header.

The probe's output matched Karma's layout part for part. It wrote 16 parts, the four keys per crypto and its own id/coverage pattern unchanged.
- **ABI.** The struct is header-only: `std::function`, `std::vector`, `std::map` and `SYS_Inline.h`. The probe linked only `pxr_*` libraries and used the same MSVC `/MD` runtime as Houdini.
- **Karma's implementation** is named in the header comment (HoudiniUsdBridge `BRAY_HdAOVBuffer.C`); it was not read for this report.

**Other metadata routes** (measured, not usable for runtime data):
- RenderProduct attributes `driver:parameters:<anything>:<name>` (also `...OpenEXR:<name>`, `...husk:OpenEXR:<name>`, `...husk:artist`/`comment`) appear in the header as `<name>` (`probes/crypto_product_meta.usda`). They are authored in USD, so they cannot carry a manifest computed at render time.
- Scalar entries of `HdRenderDelegate::GetRenderStats()` appear as header keys. With Nova: `rendererName`, `samples`, `rays`, `raysPerSecond`, `percentDone`.

**The render-delegate settings** carry no output path (`products` holds paths only; `batchCommandLine` contains the `-o` argument). With the `manif_file` route above the delegate never needs the path.

## 5. What is hashed (Karma, measured)

| source | id name | notes |
|---|---|---|
| `__name` | USD path of the gprim (`/World/geo/ballA`) | a GeomSubset does not split it (`/World/facemesh` for both faces) |
| point-instancer instance | prototype prim path (`/World/inst/protos/pebble`) | shared by all instances. The Hydra rprim is `/World/inst/protos/pebble/ForInstancer<hash>`; `HdPrimOriginSchema` scenePath = `/World/inst/protos/pebble` |
| native instance (`instanceable` reference) | `/World/refA/ball` for **both** refA and refB | Hydra rprim `/UsdNiPropagatedPrototypes/.../UsdNiPrototype/ball`; primOrigin scenePath = `ball` (relative); instancer `instanceLocations` = [`/World/refA`, `/World/refB`] → first location + relative path. `GetScenePrimPath` returns the Hydra path with an empty instancer context, so it is useless here. |
| `__materialname` | bound material path (`/World/mtl/red`) | per face through GeomSubsets; unbound geometry → `defaultshader`; instanceable references share the material id (help page) |
| primvar (string/int) | value; int as decimal string | constant and uniform (per face: `faceA`/`faceB`) on the gprim; per-instance on a PointInstancer (`string[] primvars:asset_name (interpolation = "vertex")` → `inst0..inst3`, 400 unique cloud ids); prims without the primvar get no coverage |
| kind | the LOP-authored constant primvar | `default` everywhere except under kind prims |

## 6. Coverage accumulation (measured on Karma output)

- **Sample counts.** Coverage values are multiples of 1/9 at 9 spp in 99.3 % of the covered pixels. The rest are 1/8, 1/4, 1/5 and 2/5: pixels where the variance oracle took a different number of samples. Coverage = (Σ over the pixel's camera samples of the sample weight of the id) / (that pixel's sample count).
- **Pixel filter.** It is followed through filter importance sampling, not through weights:
  - with the default `gauss` the values stay k/N;
  - with `pixelfilter = "box"` the edge widths change (868 → 497 fractional pixels);
  - in both cases the coverage sum equals the beauty alpha.
- **Opacity.** Standard Surface `opacity = 0.5` on a card over a sphere gives exactly 0.5 / 0.5 in every sample, so the weight is opacity times the remaining transmittance along the camera path, not stochastic.
- **Merging by id.** Hits with the same id merge (Kind: card + sphere both `default` → one pair, 1.0).
- **Sorting and truncation.** Pairs are sorted by coverage, descending; the order among equal values was not determined. Ids beyond `rank` are dropped: pixel (131,12) of the cloud keeps 6 × 1/9 while its alpha is 8/9, with no renormalization. Rank 1 still writes one RGBA layer, whose second pair stays (0, 0).
- **Empty pixels and slots** are (0, 0); there is no background id.

## 7. What Nova has to provide (derived from sections 2–6)

1. **Detect crypto bindings.**
   - Recognize them by `aovSettings["driver:parameters:aov:karma:cryptomatte"]`; also accept the unprefixed `driver:parameters:aov:cryptomatte`.
   - Read the rank (default 6) and the sidecar name from the same namespace.
   - The source is `sourceType == primvar` with `sourceName` `__name`, `__materialname` or a primvar name.
   - The RenderVar's own buffer (float3) receives the preview.
2. **Name per hit.**
   - Prim: `HdPrimOriginSchema` scenePath; for a relative path (native instancing), prefix the instancer's first `instanceLocations` entry. The proto suffix `ForInstancer<hash>` is never used.
   - Material: the bound material path, with GeomSubset per face; `defaultshader` when unbound.
   - Primvar: rprim constant/uniform, else instancer per-instance, else instancer constant; ints as decimal strings.
   - Hash names once at sync into a per-prim (per-face/per-instance) uint32 table: mmh3 plus the fixup.
3. **Accumulate per pixel** (first-hit data, like the other utility AOVs):
   - collect a small id → weight map over the pixel's camera samples, with the opacity share along the camera path;
   - divide by the sample count;
   - sort descending and cut at the rank.
   Karma notes that crypto buffers need not exist until husk writes (`solaris.zip:support/inside_husk.txt`); the probe confirms husk fetches them only at write time.
4. **Return the planes** through `GetResource()` on the crypto buffer:
   - `ceil(rank/2)` planes `<aovName>NN` in `HdFormatFloat32Vec4`, bottom-up rows;
   - metadata `cryptomatte/<7hex>/name|hash|conversion|manifest`, plus `manif_file` when a sidecar is set; husk then writes the file.
5. **nova_cli** writes EXRs itself (`tools/nova_cli/main.cpp` `writeExr`). It must append the same parts after each crypto var and put the metadata into part 0, ideally by reading the same `UT_HUSDExtraAOVResource` from the buffers.

## 8. Probe files and how to reproduce

Probe files (in `probes/`):
- `crypto_parms.py`: hython; lists the cryptomatte parameters and menus of the LOPs.
- `crypto_scene.usda`: the base scene.
- `crypto_export.py <out>`: hython; builds the LOP setups and exports the LOP layers and the flattened `crypto_render*.usda`.
- `crypto_variants.py <out>`: writes `crypto_var_{multi,box,single,rank1,faces,opacity}.usda` over `crypto_render.usda`: per-instance and native instances, a 400-id cloud, int and uniform primvars, GeomSubsets, an unbound sphere and a 50 % card.
- `crypto_product_meta.usda`: the RenderProduct `driver:parameters` test.
- `crypto_husk.py --renderer R --usd F --out D [--stage S] -- <husk args>`: isolated husk plus `hoiiotool --info -v -a`.
- `crypto_analyze.py file.exr [--pixels x,y ...]`: decodes ranks and checks the hashes, manifest, sorting, alpha and preview.
- `crypto_helpgrep.py`: searches the help zips.
- `crypto_probe_delegate/`: the CMake project of the `HdCryptoProbeRendererPlugin` delegate. It logs AOV settings, rprim names (primOrigin, instanceLocations, primvars) and `GetResource` calls. Env switches: `CRYPTO_PROBE_SIDECAR=1` and `CRYPTO_PROBE_BEAUTY_META=1`.

Commands:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m6\probes\crypto_export.py -Stage <stage> <out>
$HFS\python313\python.exe docs\research\m6\probes\crypto_variants.py <out>
$HFS\python313\python.exe docs\research\m6\probes\crypto_husk.py --usd <out>\crypto_var_multi.usda --out <dir> -- -o <dir>/karma.exr
$HFS\python313\python.exe docs\research\m6\probes\crypto_analyze.py <dir>\karma.exr --pixels 131,12 80,53
cmake -S docs\research\m6\probes\crypto_probe_delegate -B <build> -G "Visual Studio 17 2022" -A x64; cmake --build <build> --config Release
$HFS\python313\python.exe docs\research\m6\probes\crypto_husk.py --renderer HdCryptoProbeRendererPlugin --stage <build>\pkg --usd ... --out ...
```

## 9. Not measured

- Karma XPU output: the help says it is supported.
- Deep or other `karma:filter` blending modes on crypto vars.
- `utilitypathexpression` (crypto through refraction).
- husk snapshots/checkpoints: whether `GetResource` is then called repeatedly.
- The Solaris viewport (IPR) display of extra planes.
- The order of pairs with equal coverage.
