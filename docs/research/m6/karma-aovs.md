# M6 research: Karma AOVs, LPEs and husk render products (Houdini 22.0.466)

Scope: docs/PLAN.md M6, "AOVs: LPE (OSL-LPE-Syntax), Beauty-Komponenten, Utility (P, N, Z, albedo, crypto-matte), Multi-Part EXR via husk", result "Comp-fertige Ausgabe".
The report answers, from measurements on this machine:
1. what the Karma Render Settings LOP authors for the beauty, every beauty component and every utility AOV;
2. which LPE dialect Karma uses;
3. how husk turns RenderProducts/RenderVars into Hydra AOV bindings and render buffers, and who writes the files;
4. which EXR parts and channels a Karma CPU render of all of them produces.

Sources: the Houdini install (`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`, `$I` = `$HFS/toolkit/include`), the help archives in `$HFS/houdini/help` (extracted with 7-Zip into a temp directory; described, not copied), and my own probes.
Every Houdini process ran isolated: hython through `tests/houdini/run_hython.ps1`, husk and `hoiiotool` through `probes/husk_env.ps1` (own preference directory, no user packages, the installed Nova package skipped, staged Nova on `HOUDINI_PATH`). The user's Houdini session was not touched.
The probe scripts and their outputs are in `probes/` (section 6). They contain no SideFX code; the dumps quote attribute values Houdini authored.

---

## 0. Findings in short

1. **Authoring.** The Karma Render Settings LOP (`karmarendersettings`) and the Karma Standard Render Vars LOP author *identical* RenderVars (diffed, `probes/ksrv_dump.txt`). Every var carries:
   - `KarmaRenderVarAPI` + `HuskRenderVarAPI`;
   - `dataType` always 32-bit (`color4f`, `color3f`, `float`, `int`, `point3f`, `normal3f`, `vector3f`, `float3`);
   - the stored precision only in `driver:parameters:aov:husk:format` (`color3h`, `half3`, `float3`, `float`, ...);
   - `driver:parameters:aov:husk:name` (the AOV/EXR part name), `husk:channel_prefix`, `husk:clearValue = 0.0` (a *float*), `husk:multiSampled` (true for LPE vars, false for raw/primvar vars) and `driver:parameters:aov:karma:filter` (JSON pixel filter).
2. **Beauty components are all LPEs** (`sourceType = "lpe"`); utilities are `raw` sources in Karma's `ray:*` namespace (`ray:hitP`, `ray:hitPz`, `ray:hitN`, `ray:primid`, ...) or `primvar` (`st`). Authored strings: table 1.3. The help pages disagree with the authored strings in several places (1.9); the authored strings are the ones that render.
3. **Karma's LPE dialect** = OSL LPEs with:
   - no `S`/`s` scatter types (all specular is `G`);
   - extra terminating events `B` (background) and `A` (albedo);
   - prefixes `unoccluded;`, `shadow;`, `holdout;`;
   - a 4th slot in full events for BSDF labels (`<...'coat'>`);
   - light tags (`inputs:karma:light:lpetag`) and object tags (`primvars:karma:object:lpetag`) in the 3rd slot.
   - Measured: the components partition the beauty exactly (`C = CL + C<RD>.*L + C<RG>.*L + C<TG>.*L + C<TD>.*L + CV.*L + C.*O`, 0.02 % = half-float noise). The `coat` label works with a MaterialX Standard Surface in Karma CPU.
4. **husk → Hydra.** husk creates one render buffer per *unique* `husk:name` (vars shared by several products are allocated once). The binding's `aovName` is the `husk:name`. `aovSettings` holds `sourceName`, `sourceType`, `dataType`, `sourcePrim` plus every *authored* `driver:parameters:aov:*` attribute under its full name. The descriptor follows simple rules:
   - `HdFormat` follows `dataType`; `husk:format` only selects the file's pixel type;
   - `multiSampled` = `husk:multiSampled` (schema fallback true, false without the schema);
   - the clear value is the authored `husk:clearValue` as a scalar float, else a zero of the data type.
   - LPE, raw and primvar vars arrive the same way; the delegate interprets `sourceType`/`sourceName`.
5. **Files are written by husk**, not by the delegate:
   - raster products, multi-part EXR by default: one part per var, named by `husk:name`, in `orderedVars` order;
   - channel names from `dataType` and `channel_prefix`;
   - `driver:parameters:husk:OpenEXR:multipart = 0` gives one part;
   - several products give several files.
   - Cryptomatte rank layers and their manifest reach husk through `HdRenderBuffer::GetResource()` (`UT_HUSDExtraAOVResource`, `$I/UT/UT_HUSDExtraAOVResource.h`).
6. **Karma is strict:** it rejects ("Unsupported AOV settings for: …") every var without an authored `husk:format`, and Hydra's standard names (`color`, `depth`, `normal`, `primId`, `albedo`, `Peye`) give black. Nova today fills only Hydra names plus the beauty LPEs. On the Karma-authored scene Nova fills `C` and nothing else, and one buffer (`Facingratio_N`) came back with garbage (section 4.4).

## 1. What the Karma LOPs author (hython probes)

Probes: `probe_krs_parms.py` (parameter listing → `krs_parms_aov.txt`), `probe_krs_presets.py` (the 71 "Extra Render Vars" presets → `krs_extra_presets.txt`), `probe_karma_aovs.py` (test scene + every toggle → `karma_aovs_scene.usda`, `render_prims_dump.txt`, `ksrv_dump.txt`), `probe_krs_split.py` (split products).
Table form of the dump: `probes/karma_rendervars_table.txt`.

### 1.1 Prims and schemas

| Prim | Type | API schemas | Notes |
|---|---|---|---|
| `/Render/<node>` | RenderSettings | `KarmaRenderSettingsAPI`, `HuskRenderSettingsAPI`, `HuskRenderProductAPI` | `husk:default_delegate = "BRAY_HdKarma"`, `products` rel, all `karma:global:*`/`karma:object:*` settings |
| `/Render/<node>/Products/renderproduct` | RenderProduct | `HuskRenderProductAPI` | `productName` = Output Picture, `productType = "raster"`, `orderedVars`, `driver:parameters:husk:OpenEXR:compression = "zips"`, `…:dwa_compression = 85`, artist/comment/hostname |
| `/Render/<node>/Vars/<var>` | RenderVar | `KarmaRenderVarAPI`, `HuskRenderVarAPI` | one prim per toggle; the prim name is the parameter name (`hitPz`), the AOV name is `husk:name` (`depth`) |
| `/Render/Products/Vars/*` | RenderVar | (from other LOPs) | the LOP's "Existing Render Vars" pattern (default `/Render/Products/Vars/*`) appends these to `orderedVars` (Cryptomatte LOP and `kma_aov` vars in the probe) |

Default output: only `beauty` (`C`). AOV-related parameter folders of the LOP (`krs_parms_aov.txt`):
- Image Output > AOVs (Render Vars) > Component Level Output: Beauty, Diffuse, Reflections and Refractions, Lights and Emission, Volume, SSS, Albedo, Ambient Occlusion;
- Utility;
- Extra Render Vars (a multiparm `extrarendervars` with `name#`, `format#`, `dataType#`, `sourceName#`, `sourceType#` ∈ {raw, primvar, lpe, intrinsic}, `filter#`, `cryptomatte#`, `cryptomatterank#`, `cryptomattesidecar#`, `utilitypathexpression#`, `split#`).

### 1.2 Attributes common to every LOP-authored var

```
uniform token  dataType                                  # 32-bit USD type
uniform string sourceName / uniform token sourceType     # lpe | raw | primvar | intrinsic
string driver:parameters:aov:husk:name                   # AOV name = EXR part name
token  driver:parameters:aov:husk:format                 # file pixel type (color3h, half3, float3, float, int ...)
string driver:parameters:aov:husk:channel_prefix         # all vars except the beauty "C"
float  driver:parameters:aov:husk:clearValue = 0
bool   driver:parameters:aov:husk:multiSampled           # true for lpe vars, false for raw/primvar/intrinsic
string driver:parameters:aov:karma:filter                # '["ubox",{}]', '["minmax",{"mode":"zmin"}]', ...
string driver:parameters:aov:karma:utilitypathexpression = "CO"   # only on Extra Render Vars
bool   driver:parameters:aov:karma:cryptomatte, int …:cryptomatterank = 6, string …:cryptomattesidecar   # cryptomatte vars (and false on split vars)
```

Schema fallbacks (`$HFS/houdini/dso/usd_plugins/usdHusk/resources/generatedSchema.usda`, `usdKarma/...`):
- `HuskRenderVarAPI`: `husk:format = "float"`, `husk:multiSampled = 1`, `husk:clearValue = 0`, `husk:name = "$OS"`, `channel_prefix = ""`, `channel_lower_rgb = 0`, `bypassSave = 0`, `OpenEXR:compression = "zips"`, `source_colorspace`/`output_colorspace`;
- `HuskRenderProductAPI`: `OpenEXR:multipart = 1`, `image_format`, `makemips`;
- `KarmaRenderVarAPI`: `karma:filter = ""`, `karma:defaultvalue = 0`, `karma:dither = 1`, `karma:cryptomatte = 0`, `cryptomatterank = 6`, `utilitypathexpression = "CO"`.

### 1.3 Beauty and beauty components (Component Level Output toggles)

All `sourceType = "lpe"`, `multiSampled = true`, filter `["ubox",{}]`. `dataType`/`husk:format` = `color4f`/`color4h` for the beauty, `color3f`/`color3h` for the rest. LPE strings exactly as authored:

| Toggle (prim) | husk:name | sourceName |
|---|---|---|
| beauty | `C` | `C.*[LO]` |
| beautyunshadowed | beautyunshadowed | `unoccluded;C.*[LO]` |
| shadow | shadow | `shadow;C.*[LO]` |
| combineddiffuse | combineddiffuse | `C<RD>.*L` |
| directdiffuse | directdiffuse | `C<RD>L` |
| indirectdiffuse | indirectdiffuse | `C<RD>.+L` |
| combineddiffuseunshadowed | combineddiffuseunshadowed | `unoccluded;C<RD>.*L` |
| directdiffuseunshadowed | directdiffuseunshadowed | `unoccluded;C<RD>L` |
| indirectdiffuseunshadowed | indirectdiffuseunshadowed | `unoccluded;C<RD>.+L` |
| combineddiffuseshadow | combineddiffuseshadow | `shadow;C<RD>.*L` |
| directdiffuseshadow | directdiffuseshadow | `shadow;C<RD>L` |
| indirectdiffuseshadow | indirectdiffuseshadow | `shadow;C<RD>.+L` |
| combinedglossyreflection | combinedglossyreflection | `C<RG>.*L` |
| directglossyreflection | directglossyreflection | `C<RG>L` |
| indirectglossyreflection | indirectglossyreflection | `C<RG>.+L` |
| glossytransmission | glossytransmission | `C<TG>.*L` |
| coat ("BSDF Labelled 'coat'") | coat | `C<...'coat'>.*L` |
| visiblelights | visiblelights | `CL` |
| combinedemission | combinedemission | `C.*O` |
| directemission | directemission | `CO` |
| indirectemission | indirectemission | `C.+O` |
| combinedvolume | combinedvolume | `CV.*L` |
| directvolume | directvolume | `CVL` |
| indirectvolume | indirectvolume | `CV.+L` |
| sss ("BSDF Labelled 'sss'", folder SSS) | sss | `C<TD>.*L` |
| albedo (folder Albedo) | albedo | `CDA` |
| ambientocclusion | `AO` | raw `AO`, `multiSampled = false`, prefix `ao` |

Only the beauty ends in `[LO]`; every component ends in `L`, and emission has its own family. The `shadow` vars are the occluded fraction: measured `shadow ≈ 1 − C/unshadowed` (mean 0.1405 vs 0.1387, `probes/karma_partition.txt`).

### 1.4 Utility toggles

| Toggle | husk:name | sourceType | sourceName | dataType | husk:format | prefix | karma:filter |
|---|---|---|---|---|---|---|---|
| hitP "P (World Space)" | `P` | raw | `ray:hitP` | point3f | float3 | P | ubox |
| hitP_camera | `P_camera` | raw | `ray:hitP_camera` | point3f | float3 | P_camera | ubox |
| hitPz "Depth (Camera Space)" | `depth` | raw | `ray:hitPz` | float | float | **z** | `["minmax",{"mode":"zmin"}]` |
| element "Element (Raw Id)" | `element` | raw | `ray:element` | **int** | **float** | id | `["minmax",{"mode":"idcover"}]` |
| primid "Prim Id" | `primid` | raw | `ray:primid` | **int** | **float** | id | `["minmax",{"mode":"idcover"}]` |
| hituv "Texture Coordinates (st)" | `UV` | **primvar** | `st` | float3 | float3 | uv | ubox |
| hitN "N (Smooth Normal)" | `N` | raw | `ray:hitN` | normal3f | half3 | N | ubox |
| hitNg | `Ng` | raw | `ray:hitNg` | normal3f | half3 | Ng | ubox |
| hitN_facingratio | `Facingratio_N` | raw | `ray:hitN_facingratio` | float | half | Facingratio_N | ubox |
| hitNg_facingratio | `Facingratio_Ng` | raw | `ray:hitNg_facingratio` | float | half | Facingratio_Ng | ubox |
| motionvectors | `motionvectors` | raw | `ray:motionvector` | vector3f | half3 | motionvectors | ubox |
| velocity | `velocity` | raw | `ray:velocity` | vector3f | half3 | velocity | ubox |

All utility vars: `multiSampled = false`, `clearValue = 0`. Each has `…filter` and `…precision` parameters on the LOP (e.g. `hitPzprecision` ∈ {half, float}, `hitNprecision` ∈ {half3, float3}); the precision only changes `husk:format`. The LOP also has `hitPzfarclip` ("Camera's Far Clip as Background", off by default, not measured).

### 1.5 Extra Render Vars presets (the Name menu, 71 entries)

Choosing a preset runs the LOP's callback, which fills Source Name/Type, Format and Data Type (`krs_extra_presets.txt`).
- **LPE presets.** The same strings as 1.3, plus:
  - `holdout` = `holdout;C.*[LO]`;
  - `alphatransmission` = `!C<TG>*B`;
  - `albedodiffuse` = `CDA`, `albedoglossyreflection` = `C<RG>A`, `albedoglossytransmission` = `C<TG>A`, `albedotransmitteddiffuse` = `C<TG>*DA`.
  - The preset `sss` has the label "SSS" and `C<TD>.*L`.
- **Raw `ray:*` presets.**
  - Hit data: `ray:hitP`, `ray:hitPz`, `ray:element`, `ray:primid`, `ray:hituv` (vector3f, half3);
  - normals: `ray:hitN`, `ray:hitN_camera`, `ray:hitN_facing`, `ray:hitN_facingratio`, `ray:hitNg`, `ray:hitNg_camera`, `ray:hitNg_facing`, `ray:hitNg_facingratio`;
  - `ray:hitstack` (int), `ray:P` (ray origin), `ray:D`, `ray:time`, `ray:near`, `ray:far`, `ray:mask`, `ray:contrib`, `ray:hitdist`, `ray:dPdz`, `ray:flags`, `ray:motionvector`, `ray:velocity`;
  - holdout filters: `ray:noholdout;hitP` and `ray:holdout;hitP`.
- **Shader export.** `basecolor` = raw `export_basecolor` (color3f). Karma fills it from the Standard Surface base colour.
- **Primvar.**
  - `st` = primvar `st` (vector3f, half3);
  - `CryptoObject` = primvar `__name`, `CryptoMaterial` = primvar `__materialname` (both float3 with `karma:cryptomatte = 1`, rank 6).
- **Intrinsic.** `primarysamples` = `ray:pixelsamples` (int, `["minmax",{"mode":"max"}]`), `cputime` = `stat:cputime`, `indirectraycount` = `stat:indirectraycount` (`["accumulate",{}]`), `oraclevariance`.

Extra render vars also get `driver:parameters:aov:karma:utilitypathexpression = "CO"`. Per the Render Var help, it selects the path vertex whose value a raw/primvar/shader-export AOV records (`C<TG>O` = behind the first refraction).

### 1.6 Split per LPE Tag

Lights tagged `key` and `fill` (attribute `inputs:karma:light:lpetag`), the dome untagged. "Split per LPE Tag" on the beauty and on combineddiffuse adds one var per tag plus one for emission. The tag replaces the terminal `L` by `<L.'tag'>`; the emission var replaces it by `O`:

| prim | husk:name | sourceName |
|---|---|---|
| beauty_key / beauty_fill / beauty_emission | `C_key` / `C_fill` / `C_emission` | `C.*<L.'key'>` / `C.*<L.'fill'>` / `C.*O` (color4f) |
| combineddiffuse_key / _fill / _emission | same names | `C<RD>.*<L.'key'>` / `C<RD>.*<L.'fill'>` / `C<RD>.*O` |

Untagged lights get no split var: in the render, `C − (C_key + C_fill + C_emission)` is the dome's 35 % of the beauty. "Omit LPE Tags" (`omitlpes`, default `Untagged_Lights`) and "LPE Tag AOV Limit" (100) control the expansion.

### 1.7 Karma Cryptomatte LOP and material AOVs

- **Karma Cryptomatte LOP** (upstream of the render settings, Primitives on by default, Materials switched on):
  - authors `/Render/Products/Vars/CryptoPrimitives` (primvar `__name`) and `CryptoMaterials` (primvar `__materialname`);
  - float3/float3, `multiSampled = false`, `karma:cryptomatte = true`, `cryptomatterank = 6`, `cryptomattesidecar = ""`;
  - the render settings pick them up through "Existing Render Vars".
  - "Object name" is the full prim path (manifest in 4.2).
- **Karma AOV VOP** (`kma_aov::2.0`, in a Karma Material Builder, wired to the output's `next` input), AOV name `myaov`, fed by `mtlxposition`. The Material Library authors:
  - on the Material: `vector3f outputs:myaov.connect = </materials/kma_ground/pos.outputs:out>`;
  - on the surface shader: `vector3f inputs:aov:myaov.connect = </materials/kma_ground/pos.outputs:out>` (the help says the custom inputs go on the *shader* "due to the way Hydra conveys the UsdShade graph");
  - a RenderVar `/materials/kma_ground/Vars/myaov`, also present under `/Render/Products/Vars/myaov`. It has **no API schemas**, `sourceType = "raw"`, `sourceName = "myaov"`, `dataType = "vector3f"`, `husk:format = "float3"`, `husk:name`, `channel_prefix`, `karma:filter`.
  - Karma filled it (62 624 pixels: the ground only, i.e. only where the material is bound).

### 1.8 RenderSettings attributes that matter for AOVs

- **Component labels:**
  - `karma:global:exportcomponents = "diffuse reflect coat refract volume sss"`;
  - `diffusecomponents = "diffuse"`, `refractcomponents = "refract"`, `volumecomponents = "volume"`, `ssscomponents = "sss"`.
  - These classify BSDF component labels into LPE scatter/event types; uncategorised labels count as reflection (Karma Render Settings help, "Component Labels").
- **Pixel filtering:**
  - `karma:global:pixelfilter = "gauss"`, `pixelfilterscale = 1` apply to all vars;
  - per-var sample blending comes from `karma:filter`.
- **Clamping:** `karma:global:colorlimit = 20` and `colorlimitindirect = 20` (help: they clamp per-sample contributions to LPE image planes).
- **Other:** `karma:global:pixeloracle` (variance on plane `C`), `karma:global:imagefilter = "[]"` (denoiser/tone map), `husk:orderedImageFilters`.
- **"Split Render Product by Render Var"** (`splitrendervars`) authors one RenderProduct per var (`renderproduct_C`, `renderproduct_directdiffuse`, `renderproduct_depth`, `renderproduct_N`), each with one `orderedVars` entry. As authored, they all keep the same `productName`.

### 1.9 Help text vs authored strings

The help pages (`solaris.zip` `support/lpe.txt` "Standard LPEs" table, `nodes.zip` `lop/karmastandardrendervars.txt`) list differing strings. Measured authored values win:

| AOV | help says | authored (H22.0.466) |
|---|---|---|
| combinedglossyreflection | `C<RG>.*[LO]` | `C<RG>.*L` |
| glossytransmission | `C<TG>.*[LO]` | `C<TG>.*L` |
| coat | `C<...'coat'>.*[LO]` | `C<...'coat'>.*L` |
| sss | `C<TD>.*L` (lpe page) / `C<...'sss'>.*[LO]` (node page) | `C<TD>.*L` |
| combineddiffuseunshadowed | `unoccluded;C<RD>.*[LO]` | `unoccluded;C<RD>.*L` |
| beautyunshadowed (node page) | `unoccluded;C.*[LO][LO]` | `unoccluded;C.*[LO]` |
| albedoglossyreflection (lpe page) | `C<RD>A` | `C<RG>A` |

## 2. Karma's LPE dialect

Sources:
- `solaris.zip`: `support/lpe.txt` (Karma CPU/XPU) and `support/aovs.txt`;
- `render.zip`: `lpe.txt` (Mantra/Karma, with an OSL comparison);
- `nodes.zip`: `lop/lpetag.txt`;
- the authored strings above;
- the probe render.

| Element | Karma |
|---|---|
| Event types | `C` camera, `R` reflection, `T` transmission, `V` volume; terminating: `L` light, `O` emissive object, `B` background (Karma only, not Mantra), `A` albedo (Karma only) |
| Scatter types | `D` diffuse, `G` glossy. No `S`/`s`: specular counts as `G` |
| Full event | `<event scatter 'tag' 'bsdf_label'>`. `.` = any in a slot: `<L.'key'>` (light, any scatter, tag key), `<RG'metal'>` (glossy reflection on an object tagged metal), `<...'coat'>` (any event/scatter/tag, label coat). A label cannot be used in shorthand form (`C'coat'L` is not a label match) |
| Shorthand | `D` = `[<RD><TD>]` (any event with that scatter), `R` = any reflection, etc.; a bare `'tag'` after an event matches the object tag (`C'brick_wall'.*L`) |
| Grouping | `<…>` all of (one event), `[…]` any of, `[^…]` none of |
| Quantifiers | `.` any one event, `*` zero or more, `+` one or more |
| Negation | `!` in front of the whole expression (`!C<TG>*B`) |
| Prefixes (`prefix;expr`) | `unoccluded;` (lighting before shadowing), `shadow;` (the shadow pass of the expression), `holdout;` (lighting on holdout objects) |
| Light tags | `inputs:karma:light:lpetag` (string) on UsdLux lights; Light LOPs and the LPE Tag LOP write it |
| Object tags | `primvars:karma:object:lpetag` (constant string primvar); geometry lights are `L`, not `O` |
| BSDF labels | the label of the BSDF component. Built-in labels: `diffuse reflect coat refract volume sss`. The component-label settings (1.8) decide which labels act as diffuse/refract/volume/sss. Help: custom labels need VEX shaders; XPU knows only `coat` and `sss` |

Measured semantics (probe render, `probes/karma_partition.txt`, `probes/check_partition.py`):
- **The components partition the beauty.** `C.*[LO]` = `CL` + `C<RD>.*L` + `C<RG>.*L` + `C<TG>.*L` + `C<TD>.*L` + `CV.*L` + `C.*O` within 0.02 % of the mean beauty (half-float storage). This means:
  - the first event decides the category;
  - all emission (seen directly or through bounces) goes to `C.*O` only.
- **Splits add up.** `combined* = direct* + indirect*` (≤ 0.01 %). Custom `C<RD>.*<L.'key'>` equals the split var `combineddiffuse_key`, and `C.*<L.'fill'>` equals `C_fill`, exactly.
- **`coat` works with MaterialX.** `C<...'coat'>.*L` is non-zero on the MaterialX Standard Surface sphere with coat 1 (4 860 px) and never exceeds `combinedglossyreflection`. So Karma CPU labels the Standard Surface coat lobe `coat`, contrary to the help's "MaterialX not supported" note for custom labels.
- **Geometry tags work.** `C<RG'metal'>.*[LO]` isolates the sphere with `primvars:karma:object:lpetag = "metal"` (4 871 px).
- **Albedo terminators.** `CDA` (the `albedo` toggle) is the diffuse albedo of the first hit: ≈ 0 on the metal and glass spheres (0.007 and 0.010, edge pixels) while `basecolor` there is (0.92, 0.78, 0.49). `C<RG>A` and `C<TG>A` are the glossy albedos. `C<TG>*DA` looks through glossy transmission: on the glass sphere it shows the ground's albedo (0.24).
- **Background.** `!C<TG>*B` ≈ alpha through glass (mean 0.619 vs `C.A` 0.633; they differ where the glass is).
- **Not observed.** `holdout;…` (no holdouts) and the volume family (no volume in the scene) stayed 0; `visiblelights` is 0 because no light is camera-visible (Karma's default).

Versus OSL:
- the events and grouping are the same;
- Karma drops `S`/`s`;
- Karma adds `B`, `A`, the prefixes and the label slot;
- tags in OSL's custom-label position are Karma's object/light tags.

## 3. From RenderProducts to Hydra: what husk hands a delegate

Evidence:
- `husk -V 9` prints, per render, the parsed settings as JSON. Each var's `AOVName`, `HdFormat`, `HdMultiSampled`, `HdClearValue`, file pixel format and `settings` are the `XUSD_RenderVar` dump (`$I/HUSD/XUSD_RenderSettings.h`: `XUSD_RenderVar::desc()` returns the `HdAovDescriptor`, `dump()` prints it). The dumps are in `probes/husk_karma_dump.json` (full Karma scene) and `probes/husk_products_nova_dump.json` (variants), tabulated in `husk_*_descriptors.txt`.
- The variant scene `probes/husk_products_probe.usda` (two products, 19 vars of every type) ran with Nova and Karma.

### 3.1 Flow

1. husk loads the settings prim, each product and its `orderedVars` ("Loading render var …").
2. **A var without `driver:parameters:aov:husk:name` stops husk:** "Missing driver:parameters:aov:husk:name token in RenderVar …". The API schema itself is optional; the `kma_aov` vars have none.
3. "Adding AOV for `<dataType> <husk:name>`" once per unique name. A var used by two products is added once ("Skipping duplicate AOV for C").
4. husk creates one render-buffer Bprim per AOV (Karma's log: `renderBuffer /_UsdImaging_BRAY_HdKarma_…/aov_C`) and allocates it with the descriptor's format and multiSampled. It binds it as `HdRenderPassAovBinding{aovName = husk:name, renderBuffer, clearValue, aovSettings}` (`$I/pxr/imaging/hd/aov.h:100-135`; Nova resolves `renderBufferId` to the Bprim when the pointer is null).
5. After rendering, husk maps each buffer (`HUSD_RenderBuffer`, `$I/HUSD/HUSD_RenderBuffer.h`), converts it to the file's pixel type and writes every raster product itself with OIIO ("Saved Image: probe_main.exr", "Saved Image: probe_second.exr" in the Nova run; Nova writes no files). Non-raster product types (`karma:deep`, `karma:checkpoint`, ...) are left to the delegate (`solaris.zip` `support/inside_husk.txt`).

### 3.2 The descriptor and the aovSettings

| Field | Rule (measured) |
|---|---|
| binding `aovName` | `driver:parameters:aov:husk:name` (`C`, `directdiffuse`, `depth`, ...), **not** the source name |
| `aovSettings` keys | `sourceName`, `sourceType` (token: `lpe`, `raw`, `primvar`, `intrinsic`), `dataType`, `sourcePrim` (the RenderVar path), plus every *authored* `driver:parameters:aov:*` attribute under its full name (`driver:parameters:aov:husk:format`, `driver:parameters:aov:karma:filter`, `…:karma:cryptomatte`, `…:husk:channel_prefix`, ...). Schema fallbacks do not appear |
| `HdFormat` | from `dataType`. `color4f` → Float32Vec4; `color3f`/`point3f`/`normal3f`/`vector3f`/`float3` → Float32Vec3; `float2` → Float32Vec2; `float` → Float32; `int` and `uint` → Int32; `color3h`/`half3`/`normal3h` → Float16Vec3; `half` → Float16 |
| `husk:format` | does **not** change `HdFormat`. `color3f` + `husk:format = color3h` → buffer Float32Vec3, file half. Absent → the file type follows `dataType` (the schema fallback `float` is not applied) |
| `multiSampled` | `husk:multiSampled` if authored; else true with `HuskRenderVarAPI` applied (schema fallback), false without the schema (`myaov`, `noschema`) |
| clear value | authored `husk:clearValue` → a **scalar float** VtValue, even for Vec3/Vec4 buffers (all Karma-LOP vars: `0`; `clear_one`: `1`). Not authored → a zero of the buffer type (`[0,0,0]`, `0` for int), and *empty* (no clear) for `half` |

So raw, primvar and LPE vars reach the delegate identically, as a named buffer plus `sourceType`/`sourceName` in `aovSettings`. Hydra's `lpe:`/`primvars:` aovName prefixes (`$I/pxr/imaging/hd/tokens.h` `HdAovTokens`) are not used by husk.

### 3.3 Which names each delegate accepts

| Var (product probe) | Nova (today) | Karma CPU |
|---|---|---|
| raw `color`/`depth`/`normal`/`albedo`/`primId`/`instanceId` | filled | black. With `husk:format` authored the var is accepted but empty; without it: "Unsupported AOV settings for: …" |
| raw `Peye`, `ray:hitP` | black (unknown) | `ray:hitP` filled in the Karma scene |
| primvar `st` | black | filled (Karma scene) |
| lpe `C.*[LO]` (beauty) | filled (mapped to color) | filled |
| lpe `C<RD>L` without `husk:format` | black | rejected ("Unsupported AOV settings") |

All 15 vars rejected by Karma in the variant scene lacked `husk:format`; the 4 with it were accepted. The Karma LOPs always author it. Scenes written by Nova's own LOPs must also author it, plus Karma's source names, if Karma is to render them. M4.8 already found this: `houdini/tools/build_nova_hdas.py` authors the HDA's beauty as the LPE `C.*` with `husk:format = color4f`, because Karma renders a raw beauty black.

### 3.4 Files: parts, channels, formats

- **Multi-part by default.** One part per var, in `orderedVars` order; the part name = `husk:name`. The first part's header carries the render metadata (camera matrices, `husk:render_stats`, cryptomatte manifests).
- **Channel names** (Karma and Nova files, `probes/*_exr_layers.txt`):
  - multi-channel vars → `<prefix>.<c>`, where prefix = `husk:channel_prefix`, else `husk:name`, and `c` = `R,G,B,A` for colour/float*/half* types, `x,y,z` for point/normal/vector. Example: `UV` with prefix `uv` → `uv.R,uv.G,uv.B`.
  - `channel_lower_rgb = 1` → `pfx.r,pfx.g,pfx.b`.
  - single-channel vars → `<husk:name>.<prefix>`, with suffix `Z` without a prefix: `depth.z` (prefix `z`), `element.id`, `primid.id` (prefix `id`), `hitdist.hitdist`, `half1.Z`.
  - the var named `C` gets bare `R,G,B,A` (in both files `C` was also the first var, so "named C" vs "first" is not separated).
- **Pixel types.** From `husk:format`: half for `*h`/`half*`, float for `float*`/`*f`, uint32 for `int`.
- **`dataType int` + `husk:format float`** (Karma's `element`/`primid`): Karma's file holds float IDs (with filtered fractions such as 81.0000076 from `idcover`; Karma also logged "Pixel filter 'minmax' - Mode idcover requires PrimId channel"). Nova's Int32 buffer under the same var was written **bit for bit** as float32 (ID 15 → 2.1e-44). So husk copies the bytes rather than converting int → float. Karma either keeps float data in that buffer or reports a float buffer format; either way, a delegate has to honour `husk:format` for int vars itself.
- **Single part.** `driver:parameters:husk:OpenEXR:multipart = 0` on a product gives one part named `C` holding all channels (`R,G,B,A,N.x,N.y,N.z,directdiffuse.R,...`) with mixed half/float channels.
- **Several products, several files.** Two products → two files from one render; shared vars are rendered once.
- **Cryptomatte.** Each cryptomatte var gives 4 parts:
  - `<name>` (float3 preview colours);
  - `<name>00..02` (RGBA = id/coverage pairs; rank 6 → 3 parts);
  - header metadata `cryptomatte/<7-hex>/{name,hash=MurmurHash3_32,conversion=uint32_to_float32,manifest}`.
  - The mechanism (`$I/UT/UT_HUSDExtraAOVResource.h`): the delegate's `HdRenderBuffer::GetResource()` returns a `VtValue(HdAovSettingsMap)` with key `extra_aov_resource` → `std::shared_ptr<void>` to a `UT_HUSDExtraAOVResource`. That resource carries `addPlane(name, HdFormat)` per extra plane, `addMetadata("cryptomatte/…", …)`, and map/unmap callbacks. husk (`HUSD_RenderBuffer::numExtraBuffers/extraBuffer/mergeMetaData`) writes the extra planes and metadata.
  - The header names Karma's implementation as open source (HoudiniUsdBridge, `BRAY_HdAOVBuffer.C`); it was not read here.

## 4. Test render: Karma CPU with every component and utility AOV

### 4.1 Scene and command

- **Scene.** `probes/karma_aovs_scene.usda` (flattened stage, 1758 lines, built by `probe_karma_aovs.py`):
  - ground: Karma Material Builder grey with `kma_aov myaov`;
  - MaterialX Standard Surface spheres: coat (red + coat 1), glass (transmission 1), SSS (subsurface 1), metal (metalness 1, object tag `metal`), a small emitter (emission colour (4, 1.5, 0.3));
  - a cube moving 0.6 units/frame (frames 0-2);
  - lights: sphere light `key` (tag key), distant `fill` (tag fill), dome `env` (untagged);
  - camera shutter −0.25..0.25; 480 × 270, `samplesperpixel` 16.
- **Render settings** `/Render/krs` with:
  - all 41 Component Level and Utility toggles (two "Split per LPE Tag");
  - 18 Extra Render Vars (presets + 3 custom LPEs);
  - the Cryptomatte LOP's 2 vars and the `kma_aov` var.
  - 66 unique vars in all.
- **Command** (in the scene's directory): `husk --renderer BRAY_HdKarma -V 9 -s /Render/krs --make-output-path karma_aovs_scene.usda` (9 s wall). Output `karma_aovs.exr`: 78 parts.

### 4.2 Parts and channels (`hoiiotool --info -v -a`, `probes/exr_layers.py`; full stats in `probes/karma_exr_layers.txt`)

| Parts | name → channels | type | non-zero px (of 129 600; geometry covers 82 626) |
|---|---|---|---|
| 0 | `C` → `R,G,B,A` | half | 82 626 (alpha 0 on the background) |
| 1-3 | `C_emission`, `C_fill`, `C_key` → `<name>.R,G,B,A` | half | 82 626 |
| 4-17 | `beautyunshadowed`, `shadow`, `combineddiffuse`, `combineddiffuse_{emission,fill,key}`, `{direct,indirect}diffuse`, `{combined,direct,indirect}diffuse{unshadowed,shadow}` → `<name>.R,G,B` | half | 17 365-82 626 |
| 18-21 | `combinedglossyreflection`, `directglossyreflection`, `indirectglossyreflection`, `glossytransmission` | half | 82 626 / 80 295 / 54 840 / 5 440 |
| 22 | `visiblelights` | half | 0 (no camera-visible light) |
| 23-25 | `combinedemission`, `directemission`, `indirectemission` | half | 25 400 / 1 155 / 24 668 |
| 26-28 | `combinedvolume`, `directvolume`, `indirectvolume` | half | 0 (no volume) |
| 29-30 | `coat`, `sss` | half | 4 860 / 4 673 |
| 31-32 | `P`, `P_camera` → `.x,.y,.z` | float | 82 626 |
| 33 | `depth` → `depth.z` | float | 82 626 (0..18.04) |
| 34-35 | `element` → `element.id`, `primid` → `primid.id` | float | 21 152 / 20 779 |
| 36 | `UV` → `uv.R,uv.G,uv.B` | float | 82 626 |
| 37-38 | `N`, `Ng` → `.x,.y,.z` | half | 82 626 |
| 39-40 | `Facingratio_N`, `Facingratio_Ng` → `<name>.<name>` | half | 82 626 |
| 41-42 | `motionvectors`, `velocity` → `.x,.y,.z` | half | 649 (the moving cube; max 7.43 and 0.15) |
| 43-44 | `albedo` → `albedo.R,G,B`; `AO` → `ao.R,G,B` | half | 71 956 / 82 134 |
| 45-52 | `CryptoMaterials`, `CryptoMaterials00..02`, `CryptoPrimitives`, `CryptoPrimitives00..02` | float | preview 82 626, 00: 82 626, 01: 3, 02: 0 |
| 53 | `myaov` → `.x,.y,.z` (kma_aov, object-space P) | float | 62 624 (ground only) |
| 54 | `st` → `st.x,y,z` | half | 82 626 |
| 55-58 | `albedodiffuse`, `albedoglossyreflection`, `albedoglossytransmission`, `albedotransmitteddiffuse` | half | 71 956 / 82 626 / 5 441 / 75 309 |
| 59-61 | `basecolor`, `alphatransmission`, `holdout` | half | 82 626 / 82 626 / 0 |
| 62-64 | `hitdist.hitdist` (float), `primarysamples.primarysamples` (uint32, 0-16), `cputime.cputime` (float) | | 82 626 |
| 65-66 | `N_camera`, `Ng_camera` | half | 82 626 |
| 67-74 | `CryptoObject`, `CryptoObject00..02`, `CryptoMaterial`, `CryptoMaterial00..02` | float | as 45-52 |
| 75-77 | `metal_glossy`, `key_diffuse`, `fill_all` (custom LPEs, `husk:format color3f`) | float | 4 871 / 66 458 / 82 171 |

Cryptomatte manifests in part 0's header:
- object names are prim paths (`/World/geo/coat`, ...);
- material names are material paths (`/World/mtl/metal`, `/materials/kma_ground`, ...).

The motion vectors (max 7.43 px) and velocity (0.15) are both about 0.25 of the cube's per-frame motion (≈ 28 px, 0.6 units); their exact convention needs its own probe.

### 4.3 Partition check

See §2; `probes/karma_partition.txt` is the raw output.

### 4.4 The same file rendered by Nova (current M5 build)

`husk --renderer Nova -s /Render/krs` renders all 66 vars without errors, but only `C` has content; every other LPE, `ray:*` and primvar var stays at the clear value. Exception: `Facingratio_N` (dataType float, `husk:format half`, unknown raw source) contained garbage in two runs: about 31 000 non-zero pixels, 354 NaN, values that change between runs and match no other plane. `Facingratio_Ng` (same settings) was 0. This looks like an uncleared or aliased buffer in Nova; it is unexplained and should be investigated in M6.

Related detail: Karma-authored vars arrive with a scalar float clear value (§3.2). Nova's `HdNovaRenderBuffer::Clear` writes a scalar only into channel 0 of pixel 0 and then replicates pixel 0. The other channels of a vector buffer keep whatever pixel 0 held, which is 0 after `Allocate` but stale in later frames.

## 5. Not measured / open

- Volume LPEs (`V`), `holdout;`, `B` with holdouts, and `hitPzfarclip`.
- The exact units of `ray:motionvector`/`ray:velocity` (4.2).
- The Solaris viewport's AOV path (Render Outputs menu): this report covers husk only.
- Deep (`karma:deep`) and checkpoint products.
- `HOUDINI_OIIO_EXR`: the schema doc says multi-part needs it set to 1. In 22.0.466 the default output was multi-part without setting it.
- Whether husk uses the buffer's `GetFormat()` or the descriptor for the int → float copy (3.4). Separating the two needs a delegate that reports a different format from the one it was allocated with.

## 6. Probe files (`docs/research/m6/probes/`)

| File | What |
|---|---|
| `husk_env.ps1` | runs `$HFS/bin` tools (husk, hoiiotool) isolated, with timeout and log |
| `probe_krs_parms.py` → `krs_parms_aov.txt` | AOV-related parameters of `karmarendersettings` |
| `probe_krs_presets.py` → `krs_extra_presets.txt` | the 71 Extra Render Var presets and what they fill in |
| `probe_karma_aovs.py` → `karma_aovs_scene.usda`, `render_prims_dump.txt`, `ksrv_dump.txt` | the test scene with every toggle; dumps of all RenderSettings/Product/Var prims (Karma Render Settings and Karma Standard Render Vars) |
| `table_from_dump.py` → `karma_rendervars_table.txt` | the dump as a table |
| `probe_krs_split.py` | "Split Render Product by Render Var" (output in the scratch run only; summarised in 1.8) |
| `husk_products_probe.usda` | variant scene: dataType/format/schema/clear/multiSampled/prefix variants, two products, single-part EXR |
| `husk_dump_table.py` → `husk_karma_descriptors.txt`, `husk_products_descriptors.txt`, `husk_*_dump.json` | the `husk -V 9` JSON per render var |
| `exr_layers.py` → `karma_exr_layers.txt`, `products_nova_exr_layers.txt`, `products_karma_exr_layers.txt` | EXR parts, channels, pixel types, statistics |
| `check_partition.py` → `karma_partition.txt` | sums of the component AOVs |

Re-run (from the repo root, PowerShell):
```
powershell -NoProfile -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m6\probes\probe_karma_aovs.py -Stage build\vs2022\stage\Release\nova <outdir>
cd <outdir>; powershell -NoProfile -ExecutionPolicy Bypass -File <repo>\docs\research\m6\probes\husk_env.ps1 -Log husk.log -CmdLine "--renderer BRAY_HdKarma -V 9 -s /Render/krs --make-output-path karma_aovs_scene.usda"
$HFS\python313\python.exe -I <repo>\docs\research\m6\probes\exr_layers.py karma_aovs.exr
```
