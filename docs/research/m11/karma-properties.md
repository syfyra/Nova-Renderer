# M11 research: Karma's render properties and what Nova does with them

Goal (user): a scene set up with Karma's nodes (Karma Render Settings, the Karma tab of Render Geometry Settings, the
Karma parameters of Light and Camera LOPs) should render the same in Nova. This report lists every Karma render
property, says what Nova does with it today, marks which ones change the image, and groups the gaps into work
packages. No code was changed.

## 0. Summary

212 properties: the 210 of Karma's registry (`RenderPropertiesKA.json` 209 + `karma:global:engine` from the .ds files) and 2 the Karma Render Settings LOP authors without registering them (marked *). **H** 14 honoured, **F** 18 read as fallback (10 of them only partly), **W** 6 ignored with a warning, **D** 16 ignored on purpose (documented, no warning), **S** 158 silently ignored. Image-affecting gaps (look changes, status S/W/D or partial F): 125 (27 of them the outline feature); noise-only gaps: 28.

Main findings:

1. **Nova ignores most of Karma's properties without a word.** Every Nova render of a section 2 probe that only sets
   an ignored Karma property is bit-identical to its base render (ratio exactly 1, no changed block), so the value
   never reaches the integrator. Karma's image changed in every probe except the four rows marked "not
   discriminating", often by large factors.
2. **A default Karma Render Settings LOP scene already looks different in Nova** (section 3): Karma fakes caustics
   (`causticsenable` off, authored explicitly by the LOP) where Nova casts black shadows and renders true caustics;
   Karma has no internal reflection on glass by default (`internalreflectenable` off) where Nova always has it; Karma
   constrains roughness along paths (`constrainmaxrough` on); Karma's colour limit (10 as the LOP authors it) clamps
   other contributions than Nova's `clampIndirect`; Karma's volume march (`volumesteprate` 0.25) is biased where Nova
   is not (documented, M8 D16). Sample count and adaptive sampling are not carried over (Nova: 64 uniform).
3. **Cheap, exact wins:** the camera's USD exposure controls (`exposure`, `exposure:iso`/`time`/`fStop`/
   `responsivity`) and `karma:camera:tint` are applied exactly by Karma (measured x2.000, x2.000, x0.250 and
   (1, 0.5, 0.25)) and ignored by Nova. Many Karma settings have a Nova twin with the same defaults and rules
   (pixel filter, dicing, texture filter width, seed, limits) and only lack the fallback read.
4. **Per-object ray limits from the Render Geometry Settings LOP are ignored on surfaces** (measured: a diffuse limit
   of 0 on the ground darkens Karma's image by 3.7 %, Nova's not at all) but read on volumes.
5. **Bug:** the Karma Render Settings LOP authors `karma:object:curve_style` and `point_style` as menu-label tokens
   ("Rounded Curves", "Spheres"); Nova's render-settings fallback casts them to float, which fails, so a non-default
   style on the render settings is dropped silently (`curves.cpp:162`, `points.cpp:47-49`). The prim-level int
   primvars work.
6. **Doc/code mismatch:** `docs/design/M9.md` D10 says `sss_traceset` "warns once"; the code has no such warning.
7. **Light controls Karma honours and Nova ignores** (measured): `activeradius` (x0.50), `pointradius` (x0.93),
   `spread` (x1.28), `contribs` (black), `misbias` -1 on a point light (black), UsdLux `shadow:enable` and
   `shadow:color`. Visibility and holdouts (`rendervisibility`, `holdoutmode`) are ignored as well.

## 1. Method

- **Karma's property list.** `$HFS/houdini/RenderPropertiesKA.json` (209 properties, all `"renderer": "karma"`) and the
  parameter files `$HFS/houdini/soho/parameters/BRAY_HdKarma_{Global,Geometry,Light,Camera,Aov,Product,Viewport}.ds`
  and `BRAY_HdKarmaXPU_Viewport.ds` (617 parameters after removing the `_control` menus; names decoded from Houdini's
  `xn__` encoding). The .ds files add `karma:global:engine` and lack `inputs:karma:light:category`,
  `shadowtraceset` and `karma:global:rendercamera`; `RenderProperties.json` is Mantra's and only includes the Karma
  file. An isolated hython (`tests/houdini/run_hython.ps1`, no user packages) created a default Karma Render
  Settings LOP (`karmarenderproperties`), a Render Geometry Settings LOP, a Light and a Camera LOP and dumped what they
  author, with types. This found two properties the LOP authors without registering them
  (`karma:global:pixelfiltersize` 2.0, `karma:global:enable_dof` on) and LOP defaults that differ from the registry
  (section 3). Meanings in the tables are my own short wording.
- **Nova's handling.** Every Karma name and its unprefixed key were searched in `hydra/`, `core/` and
  `houdini/tools/nova_settings.json`, and each hit was read in context. Status codes:
  **H** honoured (Karma's name is read directly; Nova has no own setting for it),
  **F** read as a fallback of a Nova setting (Nova's value wins; "partial" when some paths or value types are
  missed), **W** ignored with a warning, **D** ignored on purpose and documented in `docs/design` without a runtime
  warning, **S** silently ignored.
- **Image column.** **L** changes the converged image (look), **N** changes only noise or convergence (sample counts,
  sampling strategies; unbiased), **A** AOV/output content only, **P** performance, IPR or diagnostics, **-** not
  applicable to Nova.
- **Effort column.** CPU/XPU: **S** up to a day, **M** one to three days, **L** more. "/-" means the work is in the
  Hydra layer (`hydra/`) and reaches both devices at once; integrator work needs the CPU path (`core/integrator`) and
  its twin in `core/gpu/kernels.cu` / `programs.cu`.
- **Measurements** (section 2): isolated husk (`tests/reference/tools/refcommon.IsolatedHoudini`), Karma CPU
  (`BRAY_HdKarma`) and the staged Nova CPU (`build/vs2022/stage/Release/nova`), variant layers over
  `tests/reference/scenes/light_point.usda` (Karma in Path Traced mode, uniform oracle, colour limit off via
  `common.usda`), 160x96, 64 spp. Each renderer is compared with its own base render: mean RGB ratio, and the share of
  4x4 pixel blocks whose luminance moved by more than 15 %. The scripts stayed in the session scratch directory
  (`probe_render.py`, `probe_render2.py`); section 6 describes how to turn them into a test.

## 2. Measurements (Karma vs Nova, each against its own base)

Base agreement: Nova/Karma mean ratio 1.0001 (point light scene) and 1.0001-1.0003 (the rect light variant).

| Variant over `light_point.usda` | Karma | Nova |
|---|---|---|
| camera `exposure` = 1 | x2.000 | x1 (bit-identical) |
| camera `exposure:iso` = 200 | x2.000 | x1 |
| camera `exposure:fStop` = 2 | x0.250 | x1 |
| `karma:camera:tint` = (1, 0.5, 0.25) | x(1.000, 0.500, 0.250) | x1 |
| `karma:camera:window` = (-0.5, 0.5, -0.5, 0.5) | mean x1.08-1.13, 68 % of blocks changed | x1 |
| `karma:global:colorlimit` and `colorlimitindirect` = 0.1 | x(0.986, 0.993, 0.995) | x1 |
| `karma:global:pixelfilter` box, scale 3 | mean x1.000 (filters keep energy; not discriminating) | x1 |
| `primvars:karma:object:rendervisibility` "-primary" on the middle ball | x0.970 (3.5 % of blocks) | x1 |
| same, "-shadow" | x1.011 (1.6 %) | x1 |
| `primvars:karma:object:holdoutmode` = 1 on the right ball | x(0.983, 0.972, 0.945), alpha mean 0.954 | x1, alpha 1 |
| `primvars:karma:object:diffuselimit` = 0 on the ground (settings: 3) | x0.963 (8.8 %) | x1 |
| `primvars:karma:object:cull_backface` = 1 on wall and ground (front faces seen) | x1 | x1 (not discriminating) |
| UsdLux `shadow:enable` = 0 (ShadowAPI) | x1.040 (5.3 %) | x1 |
| UsdLux `shadow:color` = (1, 0, 0) | x(1.040, 1, 1) | x1 |
| `inputs:karma:light:activeradius` = 3 | x(0.501, 0.476, 0.474), 72 % | x1 |
| `inputs:karma:light:pointradius` = 1 | x0.93 | x1 |
| `inputs:karma:light:contribs` = "reflect" (Lambert scene) | x0 (black) | x1 |
| `inputs:karma:light:misbias` = -1 (point light) | x0 (black) | x1 |
| `primvars:karma:object:smoothp` = 0 (dense sphere) | x1 | x1 (not discriminating) |
| normals blocked on the balls, with and without `cuspangle` 0 | x1.000 | x1.000 (not discriminating at this density) |
| `karma:global:override_lighting` = "Emissive Objects" | x0 (no emitters) | x1 |
| `karma:global:disable_lighting` = 1 | x(5.60, 5.75, 5.76) | x1 |
| rect light `inputs:karma:light:spread` = 0.2 | x1.28 (99 %) | x1 |
| rect light `singlesided` = 0 (control, light faces away from nothing) | x1.002 | x1.002 |
| rect light `shaping:cone:angle` = 20 (control) | x0.220 | x0.220 |

The two controls show the harness detects properties both renderers honour; the four "not discriminating" rows need a
scene built for the property (listed in the Verification column of the tables).

## 3. What a default Karma Render Settings LOP authors, and what Nova makes of it

The LOP authors 72 attributes (and the `camera` and `products` relationships) on `/Render/rendersettings`
(KarmaRenderSettingsAPI) even when nothing is edited, plus a
beauty RenderVar (`C.*[LO]`, `color4h`, `driver:parameters:aov:karma:filter = ["ubox",{}]`). Values that matter:

| Authored | Value | Registry default | Nova |
|---|---|---|---|
| `karma:global:samplesperpixel` / `pathtracedsamples` / `convergence_mode` | 9 / 128 / Variance | same | ignored: 64 uniform samples (`nova:samples`) |
| `karma:global:pixeloracle` | variance JSON, threshold 0.01, minrays -1 | "variance" | ignored: uniform (`nova:pixelOracle`) |
| `karma:global:colorlimit`, `colorlimitindirect` | **10**, **10** | 20, 20 | ignored; Nova's `clampIndirect` 10 clamps indirect light only |
| `karma:global:constrainmaxrough` | on | on | ignored (no equivalent) |
| `karma:object:causticsenable` | off (fake caustics) | off | ignored: Nova renders true caustics by default |
| `karma:object:ssslimit` | **0** | 1 | read (fallback while `nova:object:sssLimit` is at its default 1) |
| `karma:object:{diffuse,reflect,refract,volume}limit` | 1, 4, 4, 0 | same | read; equal to Nova's defaults |
| `karma:object:volumesteprate` | 0.25 | 0.25 | ignored on purpose (Nova does not march; Karma's absorbing emitters come out about 10 % bright) |
| `karma:global:pixelfilter`, `pixelfiltersize` | gauss, **2.0** (unregistered) | gauss | ignored; Nova's default (gauss, sigma 0.5 px) equals Karma's registered gauss |
| `karma:object:curve_style`, `point_style` | tokens "Rounded Curves", "Spheres" | 1, 1 | dropped (float cast), Nova's defaults are the same |
| `karma:global:enable_dof` | on (unregistered) | - | ignored |
| `karma:global:light_sampling_mode` | "Light Tree" | Light Tree | read |
| `karma:object:vblur`, `instance_vblur`, `geosamples`, `instance_samples` | No Velocity Blur, 2, 2 | same | read |
| `karma:global:imagefilter` | `[]` | "" | read (no denoiser) |

Not authored, registry default differs from Nova: `karma:object:internalreflectenable` off (Nova always reflects
inside glass).

So with Nova's own LOPs absent, a Karma-LOP scene in Nova differs at defaults in glass (shadows, caustics, internal
reflection, path roughness), in the colour clamp, in absorbing emissive volumes, and in noise and render time.

## 4. Inventory

Tables per group. "Nova today" starts with the status code and gives the evidence (paths relative to the repo; line
numbers as of main `caba75c`). Properties marked * are not in Karma's registry.


### 1. Sampling and convergence (render settings)

None of these change the converged image; they decide how many samples Karma takes and where. They matter for render time and noise parity: a scene set up with the Karma Render Settings LOP renders in Nova with Nova's 64 uniform samples whatever Karma's settings say.

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:global:samplesperpixel` | 9 | Camera samples per pixel (Distributed and Variance modes). | **S** Nova renders `nova:samples` (64); `renderDelegate.cpp:509`. | N | S/- : use when `nova:samples` is at its default (Karma's convergence mode picks this or pathtracedsamples). | husk stats: Nova `samples` vs Karma primary rays |
| `karma:global:pathtracedsamples` | 128 | Samples per pixel in Path Traced mode. | **S** as above | N | S/- with samplesperpixel | as above |
| `karma:global:convergence_mode` | Variance | Path Traced (one indirect ray per bounce), Distributed, or Variance (indirect ray count from a noise estimate). | **S** Nova always path traces. | N | - (only selects which sample count to read) | - |
| `karma:global:pixeloracle` | variance (LOP: JSON with variance 0.01, minrays -1) | Adaptive pixel sampling: uniform, or stop pixels whose variance is low. | **S** `nova:pixelOracle` (uniform), `renderDelegate.cpp:605-612`. | N | S/- : parse the JSON into nova:pixelOracle / oracleThreshold / oracleMinSamples. | parser unit test + sample counts |
| `karma:global:percentofsamples` | 100 | Finish once this share of the samples is done. | **S** - | N | S/- | - |
| `karma:global:randomseed` | 0 | Seed of the sample pattern. | **S** `nova:seed` | N | S/- fallback | - |
| `karma:global:russianroulette_cutoff` | 2 | Depth where throughput-based path termination starts. | **S** fixed `rrStartDepth = 3`, `core/integrator/settings.h:37` | N | S/S | furnace (expectation unchanged) |
| `karma:global:screendoorlimit` | 4 | Transparent layers shaded per sample before stochastic transparency. | **S** Nova's opacity is always stochastic (M3.11). | N | - | - |
| `karma:global:light_sampling_mode` | Light Tree | Light tree or uniform light selection. | **F** read while `nova:lightSamplingMode` is at its default, `renderDelegate.cpp:536-553` | N | done (M9 D18) | - |
| `karma:global:light_sampling_quality` | 1 | Global multiplier on light and shadow samples per shading point. | **S** - | N | M/M (extra NEE samples per vertex) | equal-time noise |
| `karma:object:diffusequality` | 1 | Multiplier on the secondary-ray count of that component (Distributed/Variance). | **S** no ray splitting in Nova's wavefront | N | - | - |
| `karma:object:reflectquality` | 1 | Multiplier on the secondary-ray count of that component (Distributed/Variance). | **S** no ray splitting in Nova's wavefront | N | - | - |
| `karma:object:refractquality` | 1 | Multiplier on the secondary-ray count of that component (Distributed/Variance). | **S** no ray splitting in Nova's wavefront | N | - | - |
| `karma:object:sssquality` | 1 | Multiplier on the secondary-ray count of that component (Distributed/Variance). | **S** no ray splitting in Nova's wavefront | N | - | - |
| `karma:object:varianceaa_minsamples` | 1 | Min secondary rays of per-component variance AA. | **S** - | N | - | - |
| `karma:object:varianceaa_maxsamples` | 9 | Max secondary rays of per-component variance AA. | **S** - | N | - | - |
| `karma:object:varianceaa_thresh` | 0.01 | Noise threshold of per-component variance AA. | **S** - | N | - | - |
| `karma:hydra:variance` | 0.01 | Viewport variance threshold (Karma display option). | **S** - | N | S/- | - |
| `karma:global:guiding_enable` | off | Path guiding of indirect rays from learned radiance. | **F** `nova:guiding` (M11-P9c: Nova's own SD-tree, CPU and XPU; Karma XPU ignores guiding) | N | done | `husk_karma_guiding` |
| `karma:global:guiding_samples` | 0 | Training samples (0: train throughout). | **F** `nova:guidingSamples` | N | done | `husk_karma_guiding` |
| `karma:global:guiding_components` | diffuse volume sss | Components that are guided. | **F** `nova:guidingComponents` | N | done | `husk_karma_guiding` |
| `karma:global:guiding_deterministic` | off | Deterministic guiding at extra cost. | **F** Nova's guiding is always deterministic | N | done | `husk_karma_guiding` |
| `karma:global:guiding_clearbuffers` | off | Discard the training samples from the image. | **F** `nova:guidingClearBuffers` | N | done | `husk_karma_guiding` |


### 2. Pixel filter, image filters, components, deep output

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:global:pixelfilter` | gauss | Reconstruction filter: box, gauss (sigma 0.5 px), blackman, blackmanharris. | **S** `nova:pixelFilter` has the same names and sizes (`renderDelegate.cpp:567-590`) but Karma's value is never read. | L | S/- fallback | edge profile probe (point-sampled vs filtered edge) |
| `karma:global:pixelfilterscale` | 1 | Multiplies the filter's width. | **S** `nova:pixelFilterScale` | L | S/- fallback | as above |
| `karma:global:pixelfiltersize` * | LOP authors 2.0 | Not in the registry; authored by the Karma Render Settings LOP. | **S** - | L? | measure first whether Karma reads it | A/B husk probe |
| `karma:global:imagefilter` | "" (LOP: []) | JSON list of post filters (denoise, OCIO, ...). | **F** (partial) `denoise` entries run the OptiX denoiser while `nova:denoise` is off (`renderDelegate.cpp:309-315`, `denoise.cpp:16-49`); engine oidn warns and uses OptiX; other filters are skipped silently. | L | other filters M/- | - |
| `karma:global:samplefilter` | "" | JSON filters applied to samples before the pixel filter. | **S** - | L | M/M (rare) | - |
| `karma:hydra:denoise` | off | Viewport toolbar denoise toggle. | **S** `nova:viewportDenoise` | P | S/- fallback | viewport check |
| `karma:global:background_iprfilter` | "" | Filters for background-plate previews (holdout slap comp). | **S** - | A | with holdouts (WP-E) | - |
| `karma:global:use_background` | Auto | When the background IPR filter runs. | **S** - | A | with holdouts | - |
| `karma:global:exportcomponents` | diffuse reflect coat refract volume sss | Components computed for per-component exports. | **S** Nova's LPE labels are fixed (`core/integrator/wavefront.h:160`) | A | M/M (custom labels) | - |
| `karma:global:diffusecomponents` | diffuse | Labels that count as diffuse bounces (limits, scopes). | **S** fixed classes | L | M/M, only for custom labels | - |
| `karma:global:refractcomponents` | refract | Labels that count as refraction bounces. | **S** fixed classes | L | as above | - |
| `karma:global:ssscomponents` | sss | Labels that count as SSS bounces. | **S** fixed classes | L | as above | - |
| `karma:global:volumecomponents` | volume | Labels that count as volume bounces. | **S** fixed classes | L | as above | - |
| `karma:global:globalaovmaterial` | "" | A material that writes AOVs for every object. | **S** - | A | M/M | - |
| `karma:global:dcm_usehitdist` | off | Deep Z as hit distance instead of depth. | **S** no deep output | A | with DCM | - |
| `karma:global:dcm_excludeholdouts` | off | Holdouts write no deep samples. | **S** no deep output | A | with DCM | - |
| `karma:product:dcmcompression` | 5 | Deep camera map compression. | **S** no deep output | A | L/L (deep output) | - |
| `karma:product:dcmofsize` | 3 | Deep opacity channel count. | **S** no deep output | A | with DCM | - |
| `karma:product:dcmzbias` | 0.001 | Deep sample merge distance. | **S** no deep output | A | with DCM | - |


### 3. AOV (render var) options

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:plane:cryptomatte` | off | Turns the var into a cryptomatte. | **H** `hydra/aov.cpp:107-127` (with or without `karma:`) | A | done | - |
| `karma:plane:cryptomatterank` | 6 | IDs kept per pixel. | **H** `aov.cpp:123-125` | A | done | - |
| `karma:plane:cryptomattesidecar` | "" | Manifest file instead of header metadata. | **H** `aov.cpp:126` | A | done | - |
| `karma:plane:filter` | "" (LOP: ubox on the beauty) | Per-var sample blending: average, sum, min, max, closest, farthest, statistics. | **S** Nova always averages | A | M/M (accumulation modes in the CPU and GPU film) | depth/id AOV per mode |
| `karma:plane:defaultvalue` | 0 | Clear value of the plane. | **S** the binding's clearValue is honoured (`renderBuffer.cpp:216`) | A | S/- | - |
| `karma:plane:dither` | 1 | Dithering for 8/16-bit integer outputs. | **S** - | A | S/- | - |
| `karma:plane:utilitypathexpression` | CO | LPE-style choice of the path vertex that writes utility, primvar and shader AOVs (e.g. behind glass). | **S** Nova writes first hits | A | M/M | AOV probe with C<TG>O |


### 4. Shading, clamps, textures

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:global:colorlimit` | 20 (LOP authors 10) | Clamp on sample contributions. Measured: direct light from lights is not clamped (0.1 removed only 1.4 % of a point-lit scene); indirect light and camera-visible bright texels are. | **S** `nova:clampIndirect` (10, indirect only; `wavefront.cpp:1994-1995`) | L | S-M/S-M: measure exactly what Karma clamps, then map onto clampIndirect | A/B husk probe (measured x0.986-0.995 at 0.1) |
| `karma:global:colorlimitindirect` | 20 (LOP 10) | Separate indirect clamp when not linked. | **S** as above | L | with colorlimit | - |
| `karma:global:colorlimitindlinked` | on | Indirect uses the main colour limit. | **S** as above | L | with colorlimit | - |
| `karma:global:constrainmaxrough` | on | GGX roughness is clamped to the largest roughness met earlier on the path. | **S** - | L | M/M (path state + lobe roughness floor) | glossy seen via a rough mirror, A/B |
| `karma:global:raybias` | 0.001 | Minimum distance of secondary rays. | **S** scene intersection tolerance | L | S/S (tiny or coplanar geometry only) | - |
| `karma:global:autoraybias` | on | Automatic bias. | **S** - | P | - | - |
| `karma:global:mtlx_imagewidth` | 1 | Global multiplier on the texture filter width. | **S** `nova:textureFilterWidth` 0.25 equals Karma's width 1 (M3.9) | L | S/- (Nova width = 0.25 x this) | mtl_textured at width 2 |
| `karma:global:mtlx_imageblur` | 0 | Extra texture blur added to every lookup. | **S** `nova:textureBlur` | L | S/- | as above |
| `karma:global:abortmissingtexture` | off | Fail the render on a missing texture. | **S** `nova:abortOnMissingTexture` | P | S/- | husk exit code |
| `karma:global:autoheadlight` | off | Headlight when the scene has no lights (deprecated). | **S** husk `--headlight` serves both renderers | L | S/- | - |
| `karma:global:bgoccludeshadow` | off | Background holdouts can shadow each other. | **S** no holdouts | L | with WP-E | - |
| `karma:object:bumpshadowterminator` | 0 | Softens the shadow terminator of bump and normal maps. | **S** - | L | S-M/S-M (measure the blend) | bumped sphere A/B |
| `karma:object:smoothp` | on | Shadow rays leave from a smoothed position on low-poly meshes. | **S** - | L | M/M | 12-segment sphere A/B (dense reference sphere: no change in either renderer) |
| `karma:object:cuspangle` | 60 | Normals of meshes without normals: edges sharper than this stay hard. | **S** polygon meshes without normals render faceted (`mesh.cpp:545`) | L | M/- (cusped vertex normals in Hydra) | low-poly mesh without normals (dense-sphere probe did not separate them) |
| `karma:object:lpetag` | "" | Object tag for light path expressions. | **H** `primvarUtil.cpp:93` (meshes, curves, points, spheres, volumes) | A | done | - |
| `karma:object:lightsubset` | Outside | Which lights refractive BSDFs sample directly. | **S** weight-1 LPE sums skip into-solid directions (`lpe_path.h:368-372`); the beauty samples all (the solid blocks them) | N | S/S (L only for open refractive meshes) | - |
| `karma:object:internalreflectenable` | off | Reflection lobe on the inside of glossy transmissive surfaces. | **S** Nova always reflects internally | L | M/M (Karma's default differs from Nova's) | glass sphere A/B |
| `karma:object:dielectricpriority` | 0 | Priority of overlapping refractive volumes (liquid in glass). | **S** no medium stack (`docs/research/m8/volume-algorithms.md`) | L | L/L | glass + water probe |
| `karma:object:thinwallatrefractlimit` | on | Thin-walled behaviour past the refraction limit. | **S** `nova:object:thinWallAtRefractLimit` has the same default (`mesh.cpp:852`); Karma's is not read | L | S/- fallback (only matters when off) | - |


### 5. Ray limits

The Karma Render Settings LOP authors the limits on the settings prim (Nova picks them up), the Render Geometry Settings LOP as constant primvars `primvars:karma:object:*` (Nova ignores them on surfaces).

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:object:diffuselimit` | 1 | Bounces of that ray type; fractions are dithered per sample. | **F** (partial) render settings while Nova's limit is at its default (`renderDelegate.cpp:520-530`); the per-object primvar is ignored on meshes, curves, points and spheres (`mesh.cpp:841-851`, `curves.cpp:167-175`, `points.cpp:52-60`) | L | S/- : Karma's primvar as fallback of `nova:object:*Limit` | A/B husk probe (measured: diffuselimit 0 on the ground: Karma x0.963, Nova x1) |
| `karma:object:reflectlimit` | 4 | Bounces of that ray type; fractions are dithered per sample. | **F** (partial) render settings while Nova's limit is at its default (`renderDelegate.cpp:520-530`); the per-object primvar is ignored on meshes, curves, points and spheres (`mesh.cpp:841-851`, `curves.cpp:167-175`, `points.cpp:52-60`) | L | S/- : Karma's primvar as fallback of `nova:object:*Limit` | A/B husk probe |
| `karma:object:refractlimit` | 4 | Bounces of that ray type; fractions are dithered per sample. | **F** (partial) render settings while Nova's limit is at its default (`renderDelegate.cpp:520-530`); the per-object primvar is ignored on meshes, curves, points and spheres (`mesh.cpp:841-851`, `curves.cpp:167-175`, `points.cpp:52-60`) | L | S/- : Karma's primvar as fallback of `nova:object:*Limit` | A/B husk probe |
| `karma:object:ssslimit` | 1 (LOP authors 0) | Bounces of that ray type; fractions are dithered per sample. | **F** (partial) render settings while Nova's limit is at its default (`renderDelegate.cpp:520-530`); the per-object primvar is ignored on meshes, curves, points and spheres (`mesh.cpp:841-851`, `curves.cpp:167-175`, `points.cpp:52-60`) | L | S/- : Karma's primvar as fallback of `nova:object:*Limit` | A/B husk probe |
| `karma:object:volumelimit` | 0 | Volume scattering events. | **F** (partial) render settings and volume prims (`volume.cpp:80-84`); ignored on surfaces | L | S/- on surfaces | - |
| `karma:object:limitcolor` | (0,0,0) | Colour returned where a limit cuts the listed components. | **D** deferred, `docs/design/M4.md:97` | L | M/M | mirror corridor A/B |
| `karma:object:limitcomponents` | reflect refract | Components that return the limit colour. | **D** deferred with limitcolor | L | with limitcolor | - |


### 6. Caustics and fake caustics

Karma's default (`causticsenable` off, which the Karma Render Settings LOP authors explicitly) lets shadow and diffuse rays pass refractive objects with a BSDF-derived factor; Nova's default renders true caustics. Karma's factor is (1-F)^2 per front face and (1-F) per back face (Schlick, roughness ignored; `docs/research/m4/nodes-and-limits.md` B1-B2).

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:object:causticsenable` | off | Off: fake caustics (tinted transparent shadows); on: true caustics. | **S** `nova:object:fakeCaustics` (default off = true caustics) is never fed by Karma's (`mesh.cpp:857-860`, `renderDelegate.cpp:533`) | L | S/- mapping (fakeCaustics = not causticsenable, prim then settings) + M/M for Karma's per-face factor | B1/B2 Karma numbers as a deterministic husk test |
| `karma:object:causticsroughnessclamp` | 0.2 | Minimum roughness on true-caustic paths. | **D** deferred, `M4.md:97` | L | M/M | caustic focus probe |
| `karma:object:fakecausticsbsdfenable` | on | Shadow factor from the BSDF, else from the colour alone. | **D** deferred, `M4.md:97` | L | S-M/S-M | B1 table |
| `karma:object:fakecausticscolor` | (1,1,1) | Tint per crossing. | **D** deferred | L | S/S | B1 table |
| `karma:object:fakecausticsopacity` | (1,1,1) | Lerp of the factor towards 1 per crossing. | **D** deferred | L | S/S | B1 table |
| `karma:object:fakecausticsfresneldarken` | 0 | Angle-dependent darkening of the fake shadow. | **D** deferred | L | S-M/S-M | B1 table |


### 7. Visibility, holdouts, trace sets

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:object:rendervisibility` | * | Per-ray-type visibility (primary, shadow, diffuse, reflect, refract, volume, sss; '-' excludes). | **S** - | L | M/M: per-mesh mask tested against `RayClass` (`core/bsdf/closure.h:36`) in the Embree filter and OptiX `__anyhit__filter` | A/B husk probe (measured: '-primary' x0.970, '-shadow' x1.011 in Karma; Nova x1) |
| `karma:object:holdoutmode` | None | Matte: cut-out whose light and alpha go to holdout LPEs; Background: pre-lit plate in indirect bounces. | **S** holdout LPE AOVs warn and stay black (`renderDelegate.cpp:215-216`) | L | L/L | A/B husk probe (measured: alpha mean 0.954 in Karma, 1 in Nova) |
| `karma:object:cull_backface` | Off | Skip back (or front) facing primitives. | **S** - | L | M/M (hit filters) | back-facing card A/B (this report's probe faced the camera: no change in either) |
| `karma:object:stencilmap` | "" | Texture cut-out by st without running the material. | **S** - | L | M/M (lookup in the hit filters) | A/B |
| `karma:object:stencilthresh` | 0 | Texel values at or below it cut. | **S** - | L | with stencilmap | - |
| `karma:object:custom_tracesets` | [] | Named trace sets (texture baking). | **S** - | - | - | - |
| `karma:object:sss_traceset` | "" | Random-walk SSS also enters objects with the same set name. | **D** `docs/design/M9.md` D10 says it warns once; no such warning exists in the code | L | M/M (or add the promised warning, S) | two-object SSS probe |
| `karma:object:lightcategories` | [] | Legacy light categories (object side). | **W** `mesh.cpp:306-318` (UsdLux light linking is read instead) | L | - | - |
| `karma:object:tracesets` | [] | Legacy trace sets. | **W** `mesh.cpp:306-318` | L | - | - |
| `karma:object:visibilitymask` | 32767 | Internal visibility bits. | **S** - | - | - | - |
| `karma:object:hdrprimid` | -1 | Internal prim id. | **S** - | - | - | - |


### 8. Curves and points

On the render settings the Karma LOP authors the styles as tokens ("Rounded Curves", "Spheres"); the Render Geometry Settings LOP authors int primvars.

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:object:curve_style` | Rounded Curves | Flat ribbons, round tubes, oriented ribbons, USD default. | **F** (partial) prim primvar (int) after `nova:object:curveStyle` (`curves.cpp:152-165`); the settings token is dropped by the float cast at `curves.cpp:162` | L | S/- bug: parse the menu labels (as `HdNovaVelocityBlurMode` does) | Flat Ribbons on the settings, husk |
| `karma:object:point_style` | Spheres | Discs facing the ray, spheres, discs facing N. | **F** (partial) prim primvar after `nova:object:pointStyle` (`points.cpp:37-50`); settings token dropped likewise | L | S/- bug | Discs on the settings |
| `karma:object:width_scale` | 1 | Multiplies curve and point widths. | **F** `curves.cpp:279`, `points.cpp:98` (float: settings work) | L | done | - |
| `karma:object:curve_basis` | Geometry Default | Force linear, Bezier, B-spline or Catmull-Rom. | **S** - | L | S/- (re-read the topology with the forced basis) | A/B |


### 9. Dicing and displacement

Nova has its own primvars with Karma's defaults and rules (`nova:dicingQuality`, `nova:dicingDepth`, `nova:displacement`; `docs/research/m4/displacement.md` asked whether to read Karma's as fallbacks).

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:object:dicingquality` | 0.5 | Micropolygon size target (1: about one per pixel). | **S** `nova:dicingQuality` (`mesh.cpp:865-877`) | L | S/- fallback | disp_textured at q 1 |
| `karma:global:dicingqualityscale` | 1 | Global multiplier on dicing quality. | **S** `nova:dicingQualityScale` | L | S/- fallback | - |
| `karma:global:offscreenquality` | 0.1 | Dicing quality outside the camera's view. | **S** `nova:offscreenQuality` | L | S/- fallback | - |
| `karma:object:dicingdepthmin` | -1 | At least 2^d rows and columns per face. | **S** `nova:dicingDepth` is one fixed depth | L | S/- (min = max: fixed; else clamp the rate) | displacement.md probes |
| `karma:object:dicingdepthmax` | -1 | At most 2^d rows and columns per face. | **S** as above | L | with dicingdepthmin | - |
| `karma:object:dicingpredisplace` | off | Trial displacement before measuring. | **S** - | L | M/- | - |
| `karma:object:dicingzimportance` | 1 | Below 1 the rate drops with the incidence angle. | **S** - | L | S/- | - |
| `karma:global:dicingcamera` | "" | Camera the dicing rate is measured from. | **S** Nova dices from the first render view (`renderPass.cpp:365-369`) | L | M/- | moving-camera probe |
| `karma:object:truedisplace` | True Displacement | 0 bump only, 1 true, 2 shader off, 3 true plus bump. | **S** `nova:displacement` (bool) covers mode 2 only | L | S/- for 2; M/M for 0 and 3 | displacement.md sph_bump probe |
| `karma:global:ipr_continuous_dicing` | off | Re-dice on IPR camera moves. | **S** Nova never re-dices on camera moves (= Karma's default) | P | - | - |


### 10. Motion blur

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `disableMotionBlur` | false | UsdRender: no motion blur. | **H** `renderPass.cpp:267`, `renderDelegate.cpp:142` | L | done | - |
| `karma:global:image_blur` | on | Off: camera rays at shutter open (no visible blur, vectors kept). | **S** - | L | S/S | motion_blur scene A/B |
| `karma:object:mblur` | on | Per-object motion blur switch. | **S** - | L | S/- (sample that prim once) | A/B |
| `karma:object:blurstyle` | Rotation Blur | Rotation blur or linear matrix interpolation. | **S** Nova interpolates decomposed SRT (rotation-like) | L | S-M/S-M (linear only) | check_motion.py analytic |
| `karma:object:xformsamples` | 2 | Transform samples when not taken from the stage. | **S** Nova takes the stage's samples (cap `nova:maxMotionSteps`) | L | S/- (only with samplesfromstage off) | - |
| `karma:object:geosamples` | 2 | Deformation samples. | **F** (partial) Acceleration Blur sample count only (`velocitySceneIndex.cpp:108-121`) | L | S/- for samplesfromstage off | - |
| `karma:object:samplesfromstage` | on | Take the sample count from the stage. | **S** Nova always does | L | S/- (only when off) | - |
| `karma:object:vblur` | No Velocity Blur | Velocity or acceleration blur of points. | **F** `velocitySceneIndex.cpp:101-106`, settings `renderDelegate.cpp:652-672` | L | done (M8 D19) | - |
| `karma:object:instance_vblur` | No Velocity Blur | Velocity blur of instances. | **F** as above | L | done | - |
| `karma:object:instance_samples` | 2 | Segments of instance acceleration blur. | **F** as above | L | done | - |
| `karma:object:volumevblurmode` | Lattice Deformation | Volume velocity blur by lattice or lookup offset. | **D** lookup offset only, `docs/design/M8.md` D17 | L | M/M | - |
| `karma:object:volumevblurlatticequality` | 0.1 | Lattice resolution. | **D** as above | L | with lattice mode | - |
| `karma:object:volumevblurscale` | 1 | Scales volume velocities. | **H** `volume.cpp:117-130` | L | done | - |


### 11. Volumes

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:object:volumesteprate` | 0.25 | Ray-march samples per voxel. | **D** Nova does not march (M8 D16); Karma's default leaves absorbing emitters about 10 % bright (`docs/design/M8.md:167`) | L | - (keep unbiased; an emulation would be L/L) | houdini_volume_karma (Karma at step rate 4) |
| `karma:object:volumeshadowsteprate` | 1 | Step-rate factor for shadow rays. | **D** as above | L | - | - |
| `karma:object:volumequality` | 1 | Secondary-ray multiplier for volumes. | **D** as above | N | - | - |
| `karma:object:volumesamplingchannel` | density | Field used for empty-space culling. | **D** majorants come from the material (M8 D7) | P | - | - |
| `karma:object:volumefilter` | box | Voxel reconstruction filter. | **W** warns once, renders trilinear (`volume.cpp:94-109, 223-227`) | L | M/M | - |
| `karma:object:volumefilterwidth` | 1 | Filter width in voxels. | **W** as above | L | with volumefilter | - |
| `karma:object:volumeuniform` | off | Render a surface as a uniform-density volume. | **D** deferred, `docs/design/M8.md:51` | L | L/L (medium stack) | - |
| `karma:object:volumeuniformsamples` | 4 | Samples of uniform volumes. | **D** as above | N | - | - |


### 12. Emission as light, portals

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:object:treat_as_lightsource` | No | Light-sample emissive meshes and volumes (No / Yes / Auto). | **F** (partial) after `nova:object:lightSource` on prims (`primvarUtil.cpp:109-138`, `volume.cpp:132-147`); the render-settings default is not read | L | S/- settings default | husk_emitters |
| `karma:object:lightsource_diffusescale` | 1 | Diffuse multiplier of a treated emitter. | **H** as above | L | done | - |
| `karma:object:lightsource_specularscale` | 1 | Specular multiplier of a treated emitter. | **H** as above | L | done | - |
| `karma:object:lightsource_samplingquality` | 1 | Selection weight of a treated emitter. | **H** meshes; volumes ignore it | N | S/S volumes | - |
| `karma:object:isportal` | off | The object becomes a portal for dome lights. | **S** - | L | L/L | - |
| `karma:object:portaldomelights` | * | Dome lights the portal serves. | **S** - | L | with portals | - |


### 13. Lights (KarmaLightAPI, `inputs:karma:light:*`)

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `inputs:karma:light:renderlightgeo` | off | Camera rays see the light shape. | **H** `light.cpp:145` | L | done | - |
| `inputs:karma:light:lightgeocastsshadow` | off | The visible light shape casts shadows. | **S** - | L | M/M | A/B husk probe |
| `inputs:karma:light:singlesided` | on | Rect and disk lights emit from one side. | **H** `light.cpp:146` | L | done | A/B husk probe (both x1.002) |
| `inputs:karma:light:recttextureflip` | Flip X | Orientation of rect light textures. | **H** `light.cpp:147` | L | done | - |
| `inputs:karma:light:lpetag` | "" | Light tag in LPEs. | **H** `light.cpp:148` | A | done | - |
| `inputs:karma:light:samplingquality` | 1 | Per-light sample multiplier. | **F** after `inputs:nova:light:samplingQuality`, `light.cpp:150-151` (0: no light, Karma-measured) | N | done | - |
| `inputs:karma:light:samplingmode` | Auto | Keep the light out of the light tree. | **F** `light.cpp:155-165` | N | done | - |
| `inputs:karma:light:equiangular` | off | Equiangular MIS in volumes. | **F** `light.cpp:153-154` | N | done | - |
| `inputs:karma:light:misbias` | 0 | Shifts MIS towards BSDF (-1) or light (+1) samples. | **S** - | L | S/S (noise only for area lights; at -1 delta lights go dark) | A/B husk probe (measured: point light at -1 renders black in Karma) |
| `inputs:karma:light:portalmisbias` | 0 | Portal vs environment sampling balance. | **S** - | N | with portals | - |
| `inputs:karma:light:hdri_max_isize` | 2048 | Resolution cap of the dome importance map. | **S** - | N | S/S | - |
| `inputs:karma:light:activeradius` | -1 | The light has no effect beyond this distance. | **S** - | L | S/S (cut in NEE, at BSDF hits and in light-tree bounds) | A/B husk probe (measured: 3 units -> Karma x0.50, Nova x1) |
| `inputs:karma:light:pointradius` | 0 | Softens the 1/d^2 peak of point lights near surfaces. | **S** - | L | S/S after measuring Karma's falloff curve | A/B husk probe (measured: 1 -> Karma x0.93, Nova x1) |
| `inputs:karma:light:contribs` | * | Components the light reaches (diffuse, reflect, coat, sss, volume). | **S** only UsdLux diffuse/specular multipliers (`light.cpp:121-122`) | L | M/M (component mask vs lobe class) | A/B husk probe (measured: 'reflect' on a Lambert scene -> Karma black, Nova x1) |
| `inputs:karma:light:contributescaustics` | on | The light takes part in true caustics. | **S** - | L | S/S with WP-C | - |
| `inputs:karma:light:illumbackground` | off | The light also lights background holdouts. | **S** - | L | with WP-E | - |
| `inputs:karma:light:shader` | "" | VEX light shader. | **S** - | L | L/L (CVEX bridge for lights) | - |
| `inputs:karma:light:spread` | 1 | Emission spread of rect and disk lights (1 diffuse, 0 parallel). | **S** - | L | M/M (emission profile + sampling) | A/B husk probe (measured: 0.2 -> Karma x1.28, Nova x1) |
| `inputs:karma:light:category` | "" | Legacy light category. | **W** `light.cpp:166-175` | L | - | - |
| `inputs:karma:light:shadowtraceset` | "" | Legacy shadow trace set. | **W** `light.cpp:166-175` | L | - | - |


### 14. Camera (KarmaCameraAPI and render settings)

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:camera:tint` | (1,1,1) | Colour multiplier on camera rays. | **S** - | L | S/S | A/B husk probe (measured: Karma exactly (1, 0.5, 0.25), Nova x1) |
| `karma:camera:window` | (-1,1,-1,1) | 2D pan and zoom window on the image plane. | **S** - | L | S/- (projection in `renderPass.cpp` `_ComputeView`) | A/B husk probe (measured: 68 % of blocks change in Karma, none in Nova) |
| `karma:camera:use_lensshader` | off | Enable the lens shader. | **S** - | L | L/L | - |
| `karma:camera:lensshadervop` | "" | VOP of the lens shader. | **S** - | L | with lens shaders | - |
| `karma:camera:lensshader` | (expression) | Lens shader string. | **S** - | L | with lens shaders | - |
| `karma:camera:material:binding` | "" | Lens material. | **S** - | L | with lens shaders | - |
| `disableDepthOfField` | false | UsdRender: no depth of field. | **H** `renderPass.cpp:142` | L | done | - |
| `karma:global:enable_dof` * | LOP authors on | Not in the registry; authored by the Karma Render Settings LOP. | **S** - | L | S/- (honour off) | fStop scene A/B |


### 15. Look-dev overrides (headlight, AO)

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:global:override_lighting` | Off | Emissive Objects only, Headlight, or Dome Light instead of the stage's lights. | **S** - | L | S-M/S-M | A/B husk probe (measured: Emissive Objects -> Karma black) |
| `karma:global:disable_lighting` | off | Shade with the display colour only. | **S** - | L | M/M | A/B husk probe (measured: Karma x5.6) |
| `karma:global:head_ao_distance` | 1e38 | Occlusion distance in headlight AO. | **S** - | L | with headlight mode | - |
| `karma:global:head_ao_samples` | 1 | AO samples per shade in headlight mode. | **S** - | N | with headlight mode | - |
| `karma:global:head_depthcue_alpha` | 1 | Depth-cue fog alpha (lighting off). | **S** - | L | with headlight mode | - |
| `karma:global:head_depthcue_color` | (0,0,0) | Depth-cue fog colour. | **S** - | L | with headlight mode | - |
| `karma:global:head_depthcue_z` | (0,0) | Depth-cue fog near/far. | **S** - | L | with headlight mode | - |


### 16. Outlines (toon lines)

All silently ignored. They change the image only when `karma:global:outline_enable` is on (default off). Effort for the feature: L/L (stencil rays around primary and secondary hits).

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:global:outline_enable` | off | Turn edge detection on. | **S** - | L | with outlines | - |
| `karma:global:outline_mode` | off | Fixed or random stencil pattern. | **S** - | L | with outlines | - |
| `karma:global:outline_radius` | 1 | Search radius (line thickness). | **S** - | L | with outlines | - |
| `karma:global:outline_primary` | on | Lines on primary rays. | **S** - | L | with outlines | - |
| `karma:global:outline_primary_samples` | 4 | Stencil samples per primary ray. | **S** - | L | with outlines | - |
| `karma:global:outline_primary_stencils` | 1 | Stencil doublings (primary). | **S** - | L | with outlines | - |
| `karma:global:outline_secondary` | on | Lines in reflections and refractions. | **S** - | L | with outlines | - |
| `karma:global:outline_secondary_samples` | 4 | Stencil samples per secondary ray. | **S** - | L | with outlines | - |
| `karma:global:outline_secondary_stencils` | 1 | Stencil doublings (secondary). | **S** - | L | with outlines | - |
| `karma:global:outline_intensity` | 1 | Line intensity. | **S** - | L | with outlines | - |
| `karma:global:outline_fitmin` | 0 | Remap minimum. | **S** - | L | with outlines | - |
| `karma:global:outline_fitmax` | 1 | Remap maximum. | **S** - | L | with outlines | - |
| `karma:global:outline_apply` | on | Composite lines into the beauty. | **S** - | L | with outlines | - |
| `karma:global:outline_depthblend` | 0 | Blend of constant vs depth-varying width. | **S** - | L | with outlines | - |
| `karma:global:outline_color` | (1,0,1) | Default line colour. | **S** - | L | with outlines | - |
| `karma:global:outline_linesaovs` | off | Line types to separate AOVs. | **S** - | L | with outlines | - |
| `karma:global:outline_depththreshold` | 1 | Global depth threshold scale. | **S** - | L | with outlines | - |
| `karma:global:outline_debug` | off | Debug AOVs. | **S** - | L | with outlines | - |
| `karma:object:outline_enable` | on | Lines on this object. | **S** - | L | with outlines | - |
| `karma:object:outline_radius` | 0.002 | Object line thickness. | **S** - | L | with outlines | - |
| `karma:object:outline_contour` | on | Contour lines. | **S** - | L | with outlines | - |
| `karma:object:outline_idthreshold` | 0.001 | Id difference threshold. | **S** - | L | with outlines | - |
| `karma:object:outline_crease` | on | Crease lines. | **S** - | L | with outlines | - |
| `karma:object:outline_creasethreshold` | 0.1 | Normal change threshold. | **S** - | L | with outlines | - |
| `karma:object:outline_depth` | on | Depth lines. | **S** - | L | with outlines | - |
| `karma:object:outline_depththreshold` | 0.5 | Depth change threshold. | **S** - | L | with outlines | - |
| `karma:object:outline_secondary` | on | Lines in indirect views of the object. | **S** - | L | with outlines | - |


### 17. Engine, IPR, buckets, caches

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:global:engine` | cpu (XPU viewport: xpu) | Karma CPU or XPU. | **S** Nova picks its device by renderer (Nova / Nova XPU) and `nova:device` | P | - | - |
| `karma:global:abortnogpudevice` | off | Fail when no GPU works. | **S** Nova XPU falls back to the CPU | P | - | - |
| `karma:global:imagemode` | Progressive | Progressive or bucket rendering. | **S** - | P | - | - |
| `karma:global:progressivepasses` | 0 | Progressive passes before buckets. | **S** - | P | - | - |
| `karma:global:bucketsize` | 32 | Bucket edge in pixels. | **S** - | P | - | - |
| `karma:global:bucketorder` | Middle | Bucket order. | **S** - | P | - | - |
| `karma:global:cacheratio` | 0.25 | Share of memory for the texture/geometry cache. | **S** - | P | - | - |
| `karma:global:cachesize` | 1000 | Fixed cache size (MB). | **S** - | P | - | - |
| `karma:global:usecacheratio` | on | Ratio or fixed cache size. | **S** - | P | - | - |
| `karma:global:ipr_proxydepth` | 2 | Downsampled first IPR passes. | **S** `nova:iprProxyDepth` (same default) | P | S/- fallback | - |
| `karma:global:ipr_reserve_threads` | 1 | Threads left to Houdini in IPR. | **S** - | P | S/- | - |
| `karma:global:ipr_proxy_for_imagefilter` | 2 | Proxy passes before the IPR denoiser. | **S** `nova:denoiseStartPasses` | P | - | - |
| `karma:global:overrideobject` | "" | Object defaults on the render settings override prim values instead of being fallbacks. | **S** - | L | M/- (only once Nova reads object defaults from the settings) | - |
| `karma:global:rendercamera` | "" | Internal render camera. | **S** the UsdRender camera is used | - | - | - |


### 18. Texture baking

| Property | Default | Meaning | Nova today | Image | Gap / effort (CPU/XPU) | Verification |
|---|---|---|---|---|---|---|
| `karma:object:baker_identifier_traceset` | "" | Trace set of the high-res object. | **S** no baker | - | - | - |
| `karma:object:baker_visibility_traceset` | * | Objects visible to bake rays. | **S** no baker | - | - | - |
| `karma:object:baker_visibility_cull_backface` | Off | Culling of bake rays. | **S** no baker | - | - | - |


### 19. Generic USD attributes Karma reads (not counted above)

| Attribute | Karma | Nova today | Image | Gap / effort |
|---|---|---|---|---|
| RenderSettings `camera`, `resolution`, `pixelAspectRatio`, `aspectRatioConformPolicy`, `dataWindowNDC`, `instantaneousShutter` | read | honoured (`renderPass.cpp:133-216`, Karma's conform rule; husk applies the data window) | L | done |
| `includedPurposes`, `materialBindingPurposes` | read | purposes filtered by husk/UsdImaging; Nova binds `full` (`renderDelegate.cpp:866`) | L | done |
| `renderingColorSpace` | not measured | not read | L? | measure first |
| RenderVar `sourceName`, `sourceType`, `dataType`, `driver:parameters:aov:{name, format, clearValue}` | read | honoured (`aov.cpp:90-178`, `renderBuffer.cpp:216`) | A | done |
| `driver:parameters:aov:multiSampled` | read | ignored (Nova always filters) | A | S/- |
| UsdGeomCamera `exposure`, `exposure:iso`, `exposure:time`, `exposure:fStop`, `exposure:responsivity` | applied (measured x2, x2, x0.25) | ignored | L | S/S, WP-B |
| `fStop`, `focusDistance`, `focalLength`, apertures, `projection`, `shutter:open`/`close` | read | honoured (`renderPass.cpp:139-146, 266-270`) | L | done |
| `clippingRange` | not measured | not read | L? | measure first |
| UsdLux `intensity`, `exposure`, `color`, colour temperature, `diffuse`, `specular`, `normalize`, sizes, `treatAsPoint`, `shaping:cone:*`, `shaping:focus*`, `texture:file`, link collections | read | honoured (`light.cpp:114-179`; cone measured identical) | L | done |
| UsdLux `shadow:enable`, `shadow:color` | applied (measured) | ignored | L | S-M/S-M, WP-D |
| UsdLux `shadow:distance`, `shadow:falloff`, `shadow:falloffGamma` | not measured | ignored | L | with WP-D |
| UsdLux `shaping:ies:file`, `ies:angleScale`, `ies:normalize` | IES profiles (not measured here) | ignored | L | M/M, WP-D |
| `light:filters` (Karma Blocker Light Filter LOP, light filter shaders) | supported | no light filter prims (`renderDelegate.cpp:56-65`) | L | L/L, WP-G |
| dome `texture:format` other than latlong | supported | warns, renders latlong (`light.cpp:184-189`) | L | M/M |

## 5. Gaps by priority (work packages)

Order: cheap and visible first. Effort is per device (CPU/XPU); "/-" is Hydra-only work.

**WP-A. Read Karma's names as fallbacks (Hydra only; about 3 days; both devices at once).**
The pattern exists (`nova_settings.json` "Unset: Karma's ..."; `renderDelegate.cpp:520-553`). Items, each S/-:
1. Per-object `primvars:karma:object:{diffuse,reflect,refract,sss,volume}limit` on meshes, curves, points and spheres
   (measured gap), and `karma:object:thinwallatrefractlimit` (settings and prim).
2. `karma:object:causticsenable` (prim, then settings) as `nova:object:fakeCaustics = !causticsenable`. This alone
   turns on Nova's fake caustics for every Karma-LOP scene; the per-face factor still differs (WP-C).
3. `karma:global:pixelfilter`, `pixelfilterscale` (same names and sizes as Nova's); find out first whether Karma reads
   the LOP's unregistered `pixelfiltersize`.
4. Sampling: `samplesperpixel` or `pathtracedsamples` (by `convergence_mode`) as `nova:samples`; the `pixeloracle`
   JSON as `nova:pixelOracle`/`oracleThreshold`/`oracleMinSamples`; `randomseed` as `nova:seed`.
5. Dicing: `dicingquality`, `dicingqualityscale`, `offscreenquality`, `dicingdepthmin`/`max`, `truedisplace` 2
   (shader off) as `nova:displacement` false.
6. Textures: `mtlx_imagewidth` (Nova width = 0.25 x it) and `mtlx_imageblur`; `abortmissingtexture`.
7. Fix the token bug of `curve_style`/`point_style` on the render settings (parse the menu labels like
   `HdNovaVelocityBlurMode`); read `treat_as_lightsource` and `mblur` from the render settings as object defaults.
8. `karma:global:enable_dof` off, `karma:global:ipr_proxydepth`, `karma:hydra:denoise`.
Caveat for all: Hydra hands Nova its descriptor defaults for unset settings, so "Nova's own value wins" is really
"Nova's value wins unless it equals its default". An explicit Nova value equal to the default loses to Karma's.

**WP-B. Camera and film (about 1-2 days).**
UsdGeomCamera exposure (`HdCamera`'s linear exposure scale) and `karma:camera:tint` as a camera-ray weight (S/S; the
expected values are exact: x2, x0.25, per-channel tint); `karma:camera:window` in `_ComputeView` (S/-);
`karma:global:image_blur` off = camera rays at shutter open while motion vectors keep the motion (S/S).

**WP-C. Glass defaults and clamps (about 1-1.5 weeks).**
Karma's fake-caustic factor ((1-F)^2 per front face, (1-F) per back face, Schlick, roughness ignored) as an option or
as the Karma-compatible default of the WP-A mapping (S-M/S-M), the four `fakecaustics*` knobs (S-M/S-M),
`causticsroughnessclamp` and the lights' `contributescaustics` (M/M), `internalreflectenable` with Karma's default off
(M/M), `constrainmaxrough` (M/M), `limitcolor`/`limitcomponents` (M/M), and the colour limit: first measure which
contributions Karma clamps (camera-visible emission, indirect, NEE of emissive geometry), then map
`colorlimit`/`colorlimitindirect`/`colorlimitindlinked` onto Nova's clamp (S-M/S-M). These are the defaults a
Karma-LOP scene differs in today (section 3).

**WP-D. Light controls (about 1-1.5 weeks).**
`activeradius` (S/S), `pointradius` (S/S once Karma's falloff curve is measured), UsdLux `shadow:enable` and
`shadow:color` (S-M/S-M; the shadow-ray pass multiplies by the colour instead of blocking) and the other `shadow:*`
after measuring, `contribs` as a component mask against the lobe class (M/M), `spread` for rect and disk lights
(M/M, emission profile and its sampling), `lightgeocastsshadow` (M/M), `misbias` (S/S; only -1 changes the look, for
delta lights), IES profiles (M/M).

**WP-E. Visibility, culling, holdouts (about 2 weeks).**
`rendervisibility` as a per-mesh mask tested against `RayClass` in the Embree filter and the OptiX any-hit program
(M/M; first-hit AOVs follow the camera bit), `cull_backface` (M/M, same filters), `stencilmap`/`stencilthresh`
(M/M, a texture lookup in the filters), `holdoutmode` Matte (alpha 0, holdout LPE planes instead of today's black
with a warning) and Background (pre-lit plate in bounces, with `illumbackground`, `bgoccludeshadow`,
`background_iprfilter`) (L/L), `sss_traceset` (M/M, or add the warning M9 D10 promises, S).

**WP-F. Geometry and motion details (about 1 week).**
`cuspangle` normals for polygon meshes without normals (M/-; Nova renders them faceted today), `smoothp` and
`bumpshadowterminator` (M/M, S-M/S-M), `curve_basis` (S/-), `blurstyle` linear (S-M/S-M), `xformsamples`/`geosamples`
with `samplesfromstage` off (S/-), `dicingcamera` (M/-), `dicingpredisplace` (M/-), `dicingzimportance` (S/-),
`truedisplace` 0 and 3 (bump from the displacement program, M/M).

**WP-G. Large features (L each, plan separately).**
Nested dielectrics (`dielectricpriority`), light portals (`isportal`, `portaldomelights`, `portalmisbias`), lens
shaders (`karma:camera:*lensshader*`, lens material), VEX light shaders, light filters, outlines (27 properties),
path guiding (`guiding_*`), uniform volumes (`volumeuniform`), filtered volume lookups (`volumefilter`), Karma CPU's
lattice velocity blur, deep output (DCM), per-AOV sample blending (`karma:plane:filter`) and
`utilitypathexpression`, the look-dev overrides (`override_lighting`, `disable_lighting`, headlight AO and fog),
`light_sampling_quality` as extra light samples, custom component labels.

**Not planned (no image effect or not applicable):** engine, bucket, cache and IPR thread settings, internal ids
(`visibilitymask`, `hdrprimid`, `rendercamera`), texture baking, Karma's ray-splitting controls (`*quality`,
`varianceaa_*`, `screendoorlimit`; only the sample counts of WP-A carry over), the volume march settings (Nova stays
unbiased; documented in M8 D16).

## 6. Verification

- **Harness.** Turn the probe of section 2 into `tests/husk/check_karma_props.py`: variant layers over
  `tests/reference/scenes/light_point.usda` (plus a rect-light, a glass and a curves variant), each rendered with Nova
  and compared with Nova's own base; the expected ratio is Karma's, measured once and stored in the script as
  `check_lights.py` does, so ctest needs no Karma run. Re-measuring Karma uses the same script with
  `--renderer BRAY_HdKarma`. Pass: Nova's ratio within 3 % (or 3 sigma of the block noise) of Karma's.
- **Exact cases.** Camera exposure and tint are deterministic ratios; `activeradius` cuts are geometric. The
  fake-caustic factors have measured Karma tables (B1, B2 in `docs/research/m4/nodes-and-limits.md`), dicing and
  truedisplace probes are in `docs/research/m4/displacement.md`.
- **Reference scenes** for the ones with spatial structure (holdout alpha, `rendervisibility`, `spread`, glass with
  Karma defaults) via `tests/reference/tools/make_references.py`, so `check_references.py` covers them per AOV.
- **XPU.** Every integrator change also on `core/gpu`: CPU-vs-XPU comparison per scene and a `[gpu]` test case; the
  golden set keeps CPU bit-identity for scenes that do not use the property.
- **Viewport.** Settings that the Viewport display options carry (`karma:hydra:denoise`, IPR proxy depth) need
  `tests/houdini/run_viewport_check.ps1` in an own Houdini instance.

## 7. Caveats

- The four "not discriminating" probes (pixel filter, cull_backface, smoothp, cusp angle) did not prove that Karma
  honours those properties in this scene; the tables rely on the code for Nova's side and on Karma's documented
  behaviour for Karma's. Each needs its own scene (Verification column).
- Karma's viewport display options (`BRAY_HdKarma_Viewport.ds`) are only shown for Karma; with Nova the viewport
  shows Nova's options, so those Karma values reach Nova only through a render settings prim on the stage.
- Light filters, IES, `renderingColorSpace` and `clippingRange` were not measured here.
