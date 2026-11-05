# M7 research: adaptive sampling (Karma's pixel oracles, production practice, a design for Nova)

Scope: docs/PLAN.md M7, "OptiX-Denoiser (Viewport-IPR + Final), adaptives Sampling, IPR-Performance". This report covers **adaptive sampling** only (the denoiser has its own report). It answers:
1. what Karma CPU (Houdini 22.0.466) offers, what the Karma Render Settings LOP authors, what the help says about the algorithm, and how Karma's variance oracle behaves on Nova's reference scenes;
2. how production renderers do it (error metric, pixel filters, denoisers, AOVs, determinism);
3. how Nova's film, passes, waves and sampler work today, and a concrete design for Nova with a test plan.

Sources and tags. Every claim carries one of:
- **(measured)**: my probes on this machine;
- **(help)**: `$HFS/houdini/help/*.zip`, read through `probes/as_helpgrep.py` and described in my own words;
- **(code)**: Nova's source or Houdini's Python/.ds files;
- **(lit.)**: renderer source code, docs or papers, with the URL in section 7. The literature was checked by a sub-agent (WebFetch); claims it could not verify are marked "unverified".

Isolation. hython ran through `tests/houdini/run_hython.ps1`; husk, hoiiotool and nova_cli ran through `tests/reference/tools/refcommon.py IsolatedHoudini` (own preference directory, no user packages, the installed Nova skipped). nova_cli is the existing M6 build, copied to a scratch directory and run unchanged; nothing was rebuilt. The user's Houdini session was not touched. The machine was shared with other measurements, so wall times are indicative; sample and ray counts are exact.

---

## 0. Findings in short

1. **Karma's knobs (measured, help).** The Karma Render Settings LOP authors `karma:global:pixeloracle` as one JSON string. The default is `["variance", {"minrays": -1, "plane": "C", "variance": 0.01, "ociomode": 0, ...}]`.
   - The convergence mode default is `Variance` (UI label "Automatic"). Its primary samples are `samplesperpixel = 9`, and per-lobe secondary samples are adapted by `karma:object:varianceaa_{thresh 0.01, minsamples 1, maxsamples 9}`.
   - In `Path Traced` mode (the one comparable to Nova; the reference scenes use it) the maximum is `pathtracedsamples` (128) and the oracle decides per pixel.
   - `minrays -1` means automatic (help: derived from the maximum). The smallest count measured on fully covered pixels was 32 at a maximum of 1024, i.e. sqrt(max).
   - Per the help, the oracle measures variance across neighbouring pixels within the current bucket, after a gamma-2.0 transform by default (an OCIO transform is optional). Several planes can be measured together.
2. **Karma's per-pixel sample AOV is not a sample count (measured).** `primarysamples` (intrinsic `ray:pixelsamples`, max blending) is 0 wherever the camera ray misses geometry. Under the variance oracle its values exceed the rays per pixel implied by Karma's own ray counts by 1.6-2x (cornell_rect, threshold 0.01: plane mean 817, actual 496 primary rays per pixel). Karma's `cputime` AOV is constant per 32x32 bucket.
3. **Karma's oracle saves rays, rarely time, on Nova's reference scenes (measured).** This is at 128x128 to 160x96 with max 1024 and equal relMSE against the 4096-16384-spp references:
   - mtl_glass (glass spheres in front of the dome): 4-10x fewer primary rays but only 1.2-1.8x less render time, because the stopped pixels (the dome) are cheap;
   - cornell_rect: 0.84-0.96x time;
   - limits_caustics: 0.58-0.86x time;
   - dome_sky: 0.97-0.99x time (only the sky pixels stop).
4. **Nova can be emulated exactly (code, measured).** Nova's samples are a pure function of (pixel, sample index, seed, stream), and a render with N spp holds the first N samples of any longer render. A ladder of renders at 2^k spp therefore reproduces bit for bit what an adaptive Nova that stops pixels at power-of-two checkpoints would output. `prototypes/as_adaptive_emulator.py` replays stopping rules on such ladders.
5. **Equal-error savings for Nova (measured by emulation, min 16, max 1024, cost-weighted by measured per-pixel costs).**
   - Per-pixel rules: mtl_glass 2.6-4.0x, dome_sky 1.1-1.3x, cornell_rect 0.85-1.0x (uniform noise; nothing to gain).
   - Whole 8x8 tiles keep about 2/3 of the gain; whole 32x32 tiles (Nova's current tile) lose almost all of it.
   - So convergence must be **per pixel**, and the wavefront must compact active pixels instead of skipping tiles.
6. **Which half to compare (measured).** With Nova's shuffled, Owen-scrambled Sobol sampler only the **prefix split** gives halves that are (0,m,2)-nets: samples [0, c/2) against [c/2, c) at power-of-two checkpoints c, in 100 % of pixels.
   - Even/odd indices and Cycles' class-A split are never nets; their error estimates overestimate the true error 4-36x, growing with n, on smooth or edge integrands.
   - The prefix estimate stays within 1.3-2.4x at every n.
   - A ~sqrt(2) checkpoint schedule (16, 24, 32, 48, ...) gained only 0-7 % over doubling.
7. **Adaptive sampling is biased, slightly (measured).** Stopping depends on the samples that are kept, so the 8-seed mean of the adaptive images is about 0.05-0.18 % darker than the truth on cornell_rect.
   - On mtl_glass it is within ±0.03 %, except -0.1 % at the most aggressive threshold.
   - The worst 8x8 block is 4-7 % dark at threshold 0.04 (pixels stopping at 8-32 samples), against a 2 % spread for uniform sampling; at 0.01 it is back at the control level.
   - Pooling the error over 3x3 pixels changes the sign but not the size.
   - All production renderers accept this; Kirk & Arvo (1991) describe the effect (lit.).
8. **Recommendation.**
   - **Error estimate:** per pixel, from the prefix half at power-of-two checkpoints after a minimum, in the Cycles/Dammertz form |I - H| / sqrt(I) (linear above 1), with 3x3 pooling and one-pixel x/y dilation (rule `pool3nd`). It is the most robust in the measurements: no worse than 0.93x on uniform noise, 3.6x on mtl_glass.
   - **Normalization:** one per-pixel sample count `n_p` normalizes beauty, alpha, LPEs, cryptomatte weights and AO; first-hit AOVs are unaffected.
   - **Wavefront:** work items are chunks of the sorted active-pixel list (several samples per pixel per wave when few remain), with decisions only at global checkpoints after a pass barrier. That keeps renders deterministic and the beauty bit-identical, per pixel, to a uniform render at `n_p`.
   - **Settings:** `nova:pixelOracle` (`uniform`|`variance`), `nova:oracleMinSamples` (-1 auto), `nova:oracleThreshold`, `nova:oraclePlanes` (default `C`), with `nova:samples` as the maximum. Plus a per-pixel sample-count AOV (Karma's `ray:pixelsamples` preset).

## 1. Karma CPU (Houdini 22.0.466)

### 1.1 Parameters of the Karma Render Settings LOP

Probe: `probes/as_probe_krs_sampling.py`, which lists the parameters and dumps the authored attributes (outputs `as_krs_sampling_parms.txt`, `as_krs_sampling_attrs.txt`). Defaults as authored (measured):

| LOP parameter (label) | Attribute | Default | Notes |
|---|---|---|---|
| `engine` (Engine Settings) | (UI only) | `cpu` | switching to `xpu` changed no authored attribute |
| `samplesperpixel` (Primary Samples) | `karma:global:samplesperpixel` | 9 | Automatic mode's camera samples |
| `pathtracedsamples` (Path Traced Samples) | `karma:global:pathtracedsamples` | 128 | Path Traced mode's (and XPU's) samples = the oracle's maximum |
| `convergence_mode` (Advanced > Sampling) | `karma:global:convergence_mode` | `Variance` | menu: `Path Traced`, `Variance` (label "Automatic"); the schema also allows `Distributed` |
| `oracle` (Pixel Oracle) | `karma:global:pixeloracle` (JSON) | `variance` | menu `variance`, `uniform` |
| `oracle_minsamples` (Minimum Samples) | JSON `minrays` | -1 | -1 = automatic |
| `oracle_plane` (Planes) | JSON `plane` | `C` | space-separated AOV list |
| `oracle_variance` (Variance Threshold) | JSON `variance` | 0.01 | |
| `oracle_ociomode` / `_ociodisplay` / `_ocioview` / `_ociocolorspace` | JSON `ociomode` etc. | 0 / `sRGB - Display` / "" / `default` | 0 = disabled |
| `varianceaa_thresh` (Noise Level) | `karma:object:varianceaa_thresh` | 0.01 | Automatic mode's secondary rays |
| `varianceaa_minsamples` / `_maxsamples` | `karma:object:varianceaa_{min,max}samples` | 1 / 9 | multiplied by the per-lobe quality |
| `diffusequality` ... `sssquality` | `karma:object:*quality` | 1 | |
| `bucketsize` / `bucketorder` / `imagemode` | `karma:global:...` | 32 / `Middle` / `Progressive` | |
| (not on the LOP) | `karma:global:percentofsamples` | 100 | schema only, not authored |

Authored oracle string (measured): `["variance", {"minrays": -1, "plane": "C", "variance": 0.01, "ociomode": 0, "ociodisplay": "sRGB - Display", "ocioview": "", "ociocolorspace": "default"}]`.
The LOP builds it in `husd/karmautils.py _make_oracle` from the seven parameters (code). Setting minimum 4, threshold 0.002 and planes `C foo` produced the matching JSON (measured).
`husk --filters` lists the oracles `uniform` and `variance` and the pixel filters `accumulate`, `minmax`, `point`, `stats`, `ubox` (measured, `probes/as_results/as_husk_filters.txt`). husk's `--convergence-mode` accepts `pathtraced`, `distributed` (deprecated) and `variance` (help).

### 1.2 What the help says (paraphrased)

- **Two levels.** Primary samples are camera rays; secondary samples are the light and indirect rays at each hit (help, KUG Sampling).
  - The oracle only changes primary samples.
  - In Automatic mode Karma also varies the number of secondary rays per lobe, between the min and max secondary samples times the lobe's quality, until a per-lobe noise level is reached.
  - In Path Traced mode there is one light sample (times light quality) and one randomly chosen indirect ray per hit. That is Nova's model.
- **The variance oracle** sends a few primary samples, then compares the pixel against its neighbours and stops a pixel once the variance of its previous samples together with the adjacent pixels falls below the threshold (help, KUG Sampling).
  - The estimate is taken across the pixels of the current bucket. Very small buckets give poor estimates, which shows as black pixels that stopped too early (help, LOP page, Bucket Size).
  - By default the variance is measured after a gamma-2.0 transform. An OCIO display/view or an explicit colour space can be used instead (help, Karma filters page).
  - `minrays -1` derives the minimum from the maximum primary sample count (help, filters page).
  - Since Houdini 21 several planes can be listed. Their summed pixel values are measured, so holdouts or shadow AOVs are not undersampled (help, LOP page and 21 release notes).
  - Karma XPU has had the variance oracle since 20.5 (help, release notes).
  - The help recommends the uniform oracle for scenes dominated by strong depth of field or motion blur, where every pixel ends up at the maximum anyway and measuring only costs time (help, KUG Sampling and Resolving Noise).
- **Automatic mode's secondary threshold** should equal the oracle's (help, LOP page). A lower oracle threshold makes the secondary sampling stop earlier per hit while the oracle fires more camera rays.
- **Sample blending (per AOV, help and `BRAY_HdKarma_Aov.ds`):** `ubox` (average), `accumulate` (sum), `minmax` (min/max/zmin/zmax/idcover), and `stats` with modes `mse`, `var`, `even`, `odd`.
  - `var` is the per-pixel sample variance; `mse` is the variance of the mean (measured: `mse` ≈ `var` / samples).
  - `even`/`odd` average half of the samples.
  - The 20.5 notes add `mse`/`variance` to XPU for RenderMan's denoiser.
- **Debug AOVs (help, KUG Resolving Noise):** `primarysamples` shows how the oracle limits camera samples; `indirectraycount` shows whether max secondary samples are reached; `cputime` shows where time goes. Their authoring (M6 dump):
  - `primarysamples` = intrinsic `ray:pixelsamples`, int, `["minmax",{"mode":"max"}]`;
  - `cputime` = intrinsic `stat:cputime`, `["accumulate",{}]`.
- `percentofsamples` ends the render when that share of the primary samples has been rendered (help, Render Settings LOP).

### 1.3 Measured behaviour

Probe `probes/as_karma_adaptive.py`. It adds a layer over a reference scene with `convergence_mode = "Path Traced"` and an oracle string, a product with `C` and `primarysamples` (optionally `stats` planes), and renders with `husk -p <max> --lock-random 5`. Error: relMSE = mean over pixels and RGB of (x - r)^2 / (r^2 + 0.01) against `tests/reference/images/<scene>.exr`, minus the reference's own variance (its `.sigma.exr` squared). Time is Karma's own render wall clock (`husk:render_stats.system_time.wallclock`; 8 threads per `render_stats.threads`). Tables: `probes/as_results/as_karma_tables.txt`.

| scene | run | rays/px | render s | relMSE | uniform rays/px at equal error | saving rays / time |
|---|---|---|---|---|---|---|
| cornell_rect 128² | uniform 512 / 1024 | 512 / 1024 | 5.28 / 10.32 | 3.28e-4 / 1.58e-4 | | |
| | variance 0.01 | 496 | 5.71 | 3.72e-4 | 459 | 0.92x / 0.84x |
| | variance 0.005 | 887 | 9.14 | 1.78e-4 | 901 | 1.02x / 0.96x |
| | variance 0.0025 / 0.001 | 1012 / 1022 | 10.4 | 1.59e-4 | ~1005 | ~1.0x / 0.93x |
| mtl_glass 160x80 | uniform 256 / 1024 | 256 / 1024 | 1.87 / 5.26 | 2.33e-3 / 5.67e-4 | | |
| | variance 0.02 | 78 | 2.33 | 7.29e-4 | 801 | 10.2x / 1.84x |
| | variance 0.01 | 145 | 3.16 | 6.35e-4 | 919 | 6.3x / 1.48x |
| | variance 0.005 | 240 | 4.18 | 5.91e-4 | 987 | 4.1x / 1.17x |
| limits_caustics 160x96 | uniform 256 / 1024 | 256 / 1024 | 2.74 / 9.76 | 1.99e-4 / 3.99e-5 | | |
| | variance 0.02 / 0.01 / 0.005 | 74 / 224 / 487 | 1.55 / 4.17 / 6.63 | 8.25e-4 / 2.66e-4 / 7.95e-5 | 77 / 203 / 566 | 1.04x / 0.69x, 0.90x / 0.58x, 1.16x / 0.86x |
| dome_sky 144x96 | uniform 1024 | 1024 | 4.42 | 2.37e-3 | | |
| | variance 0.02 / 0.01 / 0.005 | 735 / 746 / 758 | ~4.2 | 2.42e-3 / 2.38e-3 / 2.37e-3 | ~980 | 1.3x / ~0.98x |

Equal error is read off a log-log fit of the uniform runs. Time savings use the same fit over render seconds; at these image sizes the fixed part of Karma's render time (time to first pixel ~0.5 s) is a large share, which penalises the short adaptive runs.

What the oracle did (measured, from the `primarysamples` histograms in `as_karma_*.json`):
- **dome_sky:** the 28 % sky pixels stopped near the minimum; 68-70 % of the geometry pixels ran to 1024 at every threshold.
- **mtl_glass:** at 0.01, 4.7 % of the pixels reached the maximum.
- **cornell_rect:** 19 % at 0.01, 68 % at 0.005 and 97 % at 0.0025. The thresholds that matter on this scene lie between 0.01 and 0.0025.
- **The smallest count on fully covered pixels** was exactly 32 with max 1024 (cornell: 9 pixels; limits_caustics: 30-5192 pixels at 32). That is consistent with an automatic minimum of sqrt(max).

The `primarysamples` plane (measured):
- It is 0 on pixels whose camera samples all miss geometry: 28 % of dome_sky, 67 % of mtl_glass, also under the uniform oracle. Edge pixels hold the number of samples that hit.
- Under the variance oracle its mean exceeds the true rays per pixel. cornell_rect at 0.01: plane mean 817 against `ray_counts.primary` / pixels = 496; mtl_glass at 0.02, geometry pixels only: about 380 against about 175.
- So it is not the number of traced camera samples (with the uniform oracle it matches exactly). It may record a scheduled sample index; I could not find out.
- `stats` planes behave as documented (`C_mse` mean = `C_var` mean / spp within 1 %).
- `cputime` is constant over each 32x32 bucket (`probes/as_karma_cost.py`), so it cannot serve as a per-pixel cost map.

## 2. Production practice

### 2.1 Cycles (lit., read from source: blender/cycles `kernel/film/adaptive_sampling.h`, `light_passes.h`, `kernel/sample/pattern.h`, `scene/integrator.cpp`, `integrator/adaptive_sampling.cpp`, `render_scheduler.cpp`, `kernel/film/read.h`)

- **Half buffer.** Each sample whose index is in "class A" (parity of the popcount of the odd index bits, after Christensen et al. 2018 §10.2.1) adds twice its RGB to an auxiliary float4 pass, so that pass estimates the image from half of the samples. The source comment says this also works with the shuffled Owen-scrambled Sobol sampler.
- **Error per pixel** at a check:
  - e = Σ_rgb |I - A| / (1e-4 + n(Σ_rgb I)), with n(s) = sqrt(s) below 1 and s above, so relative error for bright pixels;
  - I and A are scaled by exposure / per-pixel sample count;
  - the code cites §2.1 of Dammertz et al. 2010, and a TODO asks which colour space it should use;
  - a pixel has converged when e < 5 x threshold. The factor 5 is "arbitrary", to keep old thresholds meaningful.
- **Converged flag.** It lives in the aux pass's w channel. Then two 1D passes (x, then y) mark the neighbours of every unconverged pixel unconverged too, a one-pixel dilation. A `reset` flag lets converged pixels be re-tested.
- **Defaults.** On; threshold 0.01. Min samples 0 = automatic: max(4, ceil(16 / threshold^0.3)), which gives 64 at 0.01. Threshold 0 = automatic: max(0.001, 1 / samples).
- **Schedule.** Checks run every `adaptive_step` = 16 samples after the minimum (a power of two, tested with a bit mask). The scheduler cuts sample batches at those points.
- **Interactive (non-background) renders.** The threshold starts higher and is halved each time everything has converged, down to the target, re-testing all pixels. This gives a progressive noise floor in the viewport.
- **Skipping converged pixels on the GPU.** `integrator_init_from_camera` returns early for converged pixels, so no path state is created. Active path states are compacted when they become sparse, and new work is scheduled when too few paths keep the device busy (`path_trace_work_gpu.cpp`).
- **Normalization.** With adaptive sampling a sample-count pass exists, and every pass is scaled by 1/count per pixel when read: combined colour and alpha, and the cryptomatte coverage weights (ids are not rescaled). Cycles uses filter importance sampling, so samples never cross pixels.
- **Not adapted on alpha.** The error uses RGB only.

### 2.2 Arnold (lit.)

- Pixels whose samples vary more get more camera samples: between `AA_samples` and `AA_samples_max` (options `enable_adaptive_sampling`, `AA_samples_max`, `AA_adaptive_threshold`).
- It is active only when max > AA and AA >= 2.
- Default threshold 0.015 per the text; the same page captions 0.05 as the default, so the page contradicts itself.
- The metric itself is undocumented.
- Since 7.1.4 only the RGBA beauty drives it (no RGBA AOV: disabled with a warning).
- Arnold splats with filters wider than a pixel, so custom filters must weight samples by their inverse density (`AiAOVSampleIteratorGetInvDensity`, AOV `AA_inv_density`). This is the pixel-filter cost that filter importance sampling avoids.
- GPU guidance: AA 3-4, max 30-50, threshold 0.015-0.02.

### 2.3 RenderMan (lit.)

- Adaptive sampling is on by default and recommended.
- `pixelvariance` defaults to 0.015 (0 turns it off); `minsamples` defaults to sqrt(maxsamples); `maxsamples` 0 means 64.
- Metrics: `variance` (default), `contrast`, and legacy `-v22` variants. All divide by the pixel's sample count.
- The error is weighted perceptually by the slope of an sRGB/2.2 gamma curve, with special cases for very dark and saturated pixels (`darkfalloff` only matters for the legacy metric).
- `hider:adaptall` (off by default) makes all AOVs drive it; a per-AOV `relativepixelvariance` overrides that.
- XPU gained the same controls in RenderMan 26.
- Neighbourhood and check frequency are undocumented.

### 2.4 Others (lit.)

- **Mantra:** the predecessor of Karma's Automatic mode, with per-lobe noise levels and min/max ray samples (Houdini help, `render/quality`, `props/mantra`).
- **MoonRay** (DreamWorks; Jeffery 2019) extends Dammertz's hierarchical stopping condition to avoid premature stops, and measures on the beauty only.
- **Dammertz et al. (WSCG 2010):** a second frame buffer with half the samples; an image-block hierarchy refined until the |I - A| / sqrt(I) criterion holds; up to 4x fewer samples at equal RMSE.
- **Kirk & Arvo (1991):** adaptive anti-aliasing that decides from the samples it keeps is biased, most at high contrast; they give a costlier unbiased alternative.
- **Zwicker et al. 2015** surveys adaptive sampling and reconstruction.

### 2.5 Interactions

| Topic | Practice (lit., help) | Consequence for Nova |
|---|---|---|
| **Pixel filter** | Filter importance sampling (Cycles, Karma's radial filters with `ubox`, Nova: `core/film/filter.h`): a pixel is the plain average of its own samples, so per-pixel counts only change the divisor. Splatting filters (Arnold) mix neighbours with different densities and need inverse-density weights. | Nova keeps FIS; normalization is `sum / n_p`. |
| **Denoiser** | Cycles: adaptive sampling plus denoising is the default pairing (2.83 notes: after denoising "almost indistinguishable"). OIDN is trained on 1 spp to nearly converged images and says nothing about uniform noise. No official OptiX, Arnold or RenderMan statement was found. A patent proposes allocating samples by the predicted post-denoise error. | Adaptive sampling evens out the perceptual noise level, which suits a denoiser. Guides must be normalized per pixel if they are ever accumulated per sample (Nova's are first-hit today, see 3.1). |
| **AOVs** | Cycles, Arnold, MoonRay: beauty only. Karma: a list of planes, summed. RenderMan: beauty, or all AOVs with `adaptall`. | Default beauty plus alpha, optionally named LPE planes (each with its own half snapshot). |
| **Alpha / coverage** | Cycles ignores alpha; Arnold uses RGBA. The Karma help suggests checking alpha and cryptomatte for undersampled edges. | Include alpha in the error (an edge against a similar colour is invisible in RGB but not in alpha or crypto). |
| **Determinism** | Cycles checks at fixed sample counts (multiples of 16); Karma's estimate depends on the bucket. | Decide only at global checkpoints after a pass barrier (section 5.6). |
| **Sampler** | Cycles' class split for the half buffer; Christensen et al. split progressive sequences into stratified classes. | Nova's index shuffle makes the prefix split the stratified one (4.2). |

## 3. Nova today (read from code)

### 3.1 Film and normalization

`core/film/film.h` (code). Per pixel, rows bottom-up:
- **Accumulated sums:** `colorSum` (Spectrum) and `alphaSum`.
- **First-hit AOVs**, written once by the first pass through the pixel centre: `depth`, `normal`, `albedo` (the denoiser guide), `primId`, `instanceId`, and the utility planes except AO.
- **LPE sums:** `lpeSum` (output-major) and `lpeLit` (the lit part for `shadow;`).
- **Cryptomatte:** `crypto` planes of 8 (id, summed weight) slots.
- **Counter:** one `uint32_t samples` for the whole film.
- Base memory is about 52 bytes per pixel before LPE, utility and crypto planes.

`hydra/renderBuffer.cpp WriteFromFilm` (code) multiplies by `inv = 1 / film.samples`:
- the beauty (RGB and alpha), every LPE output (with the beauty's alpha), the cryptomatte weights (`w * inv`) and AO;
- `shadow;` outputs are the ratio `1 - lit / all` and need no count;
- first-hit kinds are copied as they are.

It returns early while `film.samples == 0`.

### 3.2 Passes, tiles and waves

- **Passes.** `Renderer::renderPass` (`core/integrator/renderer.cpp`) runs one sample for every pixel of the region: sample index = `film.samples`, then `++film.samples`. `converged()` is `samples >= maxSamples`.
- **Tiles.** The region is cut into 32x32 tiles at `setRegion`. `tbb::parallel_for` runs one `renderWave` per tile in a task arena, and the pass ends at the parallel_for barrier.
- **Waves.** `renderWave` (`core/integrator/wavefront.{h,cpp}`) traces one path per tile pixel (one wave of up to 1024 paths) through Generate, Intersect, Shade (A lights, B material sort, C closures) and Shadow per bounce. Path slots map to film pixels (`q.pixel[slot]`).
  - At the end the slot radiances are added to `colorSum` in slot order (non-finite samples dropped).
  - Alpha, crypto ids and the LPE deposits are added to the film during shading (each tile belongs to one thread, so there are no races).
  - With `sampleIndex == 0` the first-hit AOVs are rendered first.
- **Per-pass sample index.** Every sampler call uses one per-wave index, `ShadeContext::sampleIndex` (wavefront.cpp lines 536, 894, 1066, 1136, 1363, 1370).
- **Parallelism on small images (measured).** At most one task per tile per pass. mtl_glass (160x80) has 15 tiles, and glass pixels cost about 8x as much as sky pixels (4.32 against 0.50 µs per pixel-sample, `prototypes/as_results/crop_cost.txt`). The 1024-spp ladder step took 2.46 s on 16 threads, against about 10.7 s extrapolated from one thread (1.34 s for 128 spp): only about 4.4x. Fewer active tiles would make this worse.

### 3.3 Sampler

`core/math/sobol.h` (code): Owen-scrambled, shuffled Sobol after Burley 2020.
- A sample is a stateless function of (pixel seed, sample index, stream).
- The per-stream index shuffle maps every aligned block of 2^k indices onto an aligned Sobol block, so every prefix of 2^k samples is a (0,k,2)-net per pixel and stream (tested in `tests/test_sobol.cpp`).
- Consequences for adaptive sampling:
  - stopping a pixel at n samples gives exactly the first n samples, whatever the other pixels do;
  - stopping at a power of two keeps the pixel's sample set a complete net;
  - parity splits are not stratified (4.2).

### 3.4 Host side

- **Render loop.** `HdNovaRenderDelegate::_RenderLoop` (`hydra/renderDelegate.cpp`) calls `renderPass` until `converged()`. After every pass it copies the beauty, and other AOVs on demand (M6 D11), into the render buffers in parallel rows, sets `SetConverged`, and reports `samples` / `target` as `percentDone` and "n/target spp".
- **Settings.** `nova:samples` (default 64) is read in `GetNovaSettings`; descriptors and defaults mirror `houdini/tools/nova_settings.json` (checked by `tests/houdini/test_solaris_ui.py`).
- **Restarts.** Any change of settings restarts the render (`renderPass.cpp`: `restart |= settings != _settings`).
- **AOV kinds** (`hydra/aov.{h,cpp}`) map Karma's `ray:*` raw sources to utility planes. There is no intrinsic `ray:pixelsamples` yet.

## 4. Prototype measurements on Nova

### 4.1 Method: exact emulation from sample ladders

- `prototypes/as_nova_ladder.py` renders each scene with nova_cli at N = 8, 16, ..., 1024 and the sqrt(2) points 12, 24, ..., 768, all with the same seed. Each render is the prefix of the longer ones (3.3).
- `prototypes/as_adaptive_emulator.py` replays a stopping rule: at each checkpoint c >= min it computes the error from I_c and I_{c/2} (the prefix halves: I_c - I_{c/2} = (B - A)/2), marks converged pixels, and outputs I_{n_p} per pixel. That is exactly the beauty an adaptive Nova with the same rule and checkpoints would produce (code argument; summation order per pixel is the sample order in both cases).
- **Truth:**
  - the Karma reference for cornell_rect and dome_sky, where Nova matches Karma (their reference tests pass without regions);
  - for mtl_glass, two Nova renders at 4096 spp (Karma differs there by +2 % in glass, a documented M3 difference). Their spread is subtracted like the references' sigma.
- **Cost:** samples weighted by a two-class model of Nova's measured cost per pixel-sample (`prototypes/as_nova_crop_cost.py`: crops rendered on one thread), with coverage from Karma's uniform `primarysamples`.
  - mtl_glass: dome pixels 0.50 µs, geometry 1.47 µs (the glass interior alone 4.32 µs);
  - dome_sky: 0.64 / 1.28 µs;
  - cornell_rect: uniform.
- **Saving** = uniform samples at equal relMSE (log-log fit of the ladder, N >= 32) divided by the adaptive cost. **p99** = the same for the 99th percentile of per-pixel relative squared error. It is truth-limited for dome_sky at high counts, so dome_sky's p99 savings above 2x are inflated.

Rules (all with Cycles' normalization and threshold factor 5):
- `cycles`: per pixel plus x/y dilation, as Cycles;
- `pool3nd`: squared error and normaliser averaged over 3x3, plus dilation;
- `tile8` / `tile32`: pooled, but a whole 8x8 / 32x32 tile stops only when all its pixels have converged.

### 4.2 Which half buffer (prototype `as_sobol_split.py`, output `as_results/sobol_split.txt`)

A numpy port of `Sampler::get2D`, checked against the golden values of `tests/test_sobol.cpp`, on 2000 pixels and the pixel-filter stream (measured):

| split of samples [0, n) | half is a (0, log2 n - 1, 2)-net | calibration of (A-B)/2 vs true error, n = 16 ... 256: vertical edge / smooth bump |
|---|---|---|
| prefix [0, n/2) vs [n/2, n) | 100 % | 1.7-1.75x / 2.0-2.4x |
| even vs odd index | 0 % | 5.8-24x / 3.8-36x |
| Cycles class A vs B | 0 % (also 0 % for unshuffled Sobol) | 3.7-15x / 3.7-34x |
| random half | — | 4.9-19x / 3.6-32x |

- With Nova's sampler even/odd splits the filter's x range into halves: the index parity survives the shuffle and sets the top bit of dimension 0. That turns edges into "noise".
- A parity half is a poorly stratified estimator, so its difference measures the error of something much noisier than the QMC mean. The overestimate grows with n, and adaptive sampling would stop far too late on smooth regions.
- The prefix estimate stays calibrated (within 2.4x) at every n.
- For a small bright spot all splits are similar, and the rank correlation between estimate and true error is negative at n = 16 (-0.68): a feature missed by every sample looks converged. That is what minimum samples and pooling are for.

### 4.3 Equal-error savings (emulated, min 16, max 1024; `as_results/emu_*`)

Savings: power-of-two schedule / sqrt(2) schedule.

| scene | rule | t | mean spp | relMSE | saving (relMSE) | saving (p99) |
|---|---|---|---|---|---|---|
| cornell_rect | cycles | 0.01 / 0.005 | 167 / 516 | 9.3e-4 / 2.5e-4 | 0.85 / 0.91, 0.86 / 0.88 | 0.80 / 0.90, 0.86 / 0.92 |
| | pool3nd | 0.01 / 0.005 | 95 / 299 | 1.6e-3 / 4.2e-4 | 0.93 / 0.97, 0.96 / 0.99 | 0.89 / 0.94, 1.05 / 1.14 |
| | tile8 | 0.01 / 0.005 | 171 / 464 | | 0.86 / 0.87, 0.95 / 0.97 | |
| | tile32 | 0.02 / 0.01 | 204 / 368 | | 0.56 / 0.51, 0.83 / 0.85 | |
| mtl_glass | cycles | 0.01 / 0.005 | 116 / 209 | 4.8e-4 / 3.7e-4 | 3.74 / 3.99, 2.57 / 2.84 | 4.57 / 4.76, 3.61 / 4.10 |
| | pool3nd | 0.01 / 0.005 | 103 / 194 | 5.9e-4 / 3.9e-4 | 3.56 / 3.66, 2.78 / 3.06 | 3.50 / 3.41, 4.00 / 4.56 |
| | tile8 | 0.01 / 0.005 | 199 / 322 | | 2.64 / 2.75, 1.94 / 2.05 | |
| | tile32 | 0.01 / 0.005 | 638 / 822 | | 1.24 / 1.24, 1.08 / 1.11 | |
| dome_sky | cycles | 0.01 / 0.005 | 57 / 159 | 3.6e-4 / 9.1e-5 | 1.09 / 1.13, 1.25 / 1.20 | 2.30 / 2.33, 2.64 / 2.74 |
| | pool3nd | 0.01 / 0.005 | 33 / 96 | 6.7e-4 / 1.6e-4 | 1.12 / 1.18, 1.29 / 1.28 | 1.96 / 1.88, 3.10 / 3.29 |
| | tile8 / tile32 | 0.01 | 52 / 152 | | 1.13 / 1.12, 0.95 / 0.97 | |

For scale, uniform relMSE at 64 / 256 / 1024 spp:
- cornell_rect: 2.2e-3 / 4.7e-4 / 9.7e-5;
- mtl_glass: 6.8e-3 / 1.3e-3 / 3.5e-4;
- dome_sky: 3.8e-4 / 7.9e-5 / 1.6e-5.

Nova's dome_sky relMSE is two orders of magnitude below Karma's at equal spp (1024 spp: 1.6e-5 against 2.4e-3), so Karma's dome_sky row in 1.3 is not comparable.

Reading:
- The gain is a property of the scene: large where regions of very different noise coexist (a clean dome behind glass), none for uniform noise. There the doubling overshoot and the noise of a one-sample estimate cost 3-15 %.
- `pool3nd` is the most even rule: least loss on cornell, about the same gain elsewhere. It also spends fewer samples per threshold, so thresholds are not interchangeable between rules.
- Per-pixel granularity matters: 8x8 tiles keep about 70 % of the gain; 32x32 tiles lose almost all of it.
- The sqrt(2) schedule (two snapshots) gains 0-7 %, which does not justify the second snapshot in a first version.
- Dilation matters for the per-pixel rule (`as_results/emu_nodilate_*.txt`). Without it, pixels stop far too early: at t = 0.01 the mean is 36 / 29 spp on cornell / mtl_glass with relMSE 4.5e-3 / 3.6e-3. Lowering the threshold barely helps: cornell's saving falls to 0.63x at 0.005, and mtl_glass's p99 saving stays at 1.0-1.2x.

### 4.4 Bias (emulated, 8 seeds, max 256; `as_results/bias_*.json`)

Global relative bias of the 8-seed mean of the adaptive images (luminance), ± its standard error from the seed spread; the uniform 256-spp control is shown for comparison.

| scene, min | rule | t = 0.04 | t = 0.02 | t = 0.01 | uniform control |
|---|---|---|---|---|---|
| cornell_rect, 8 | cycles | -0.13 % ± 0.04 | -0.18 % ± 0.02 | -0.11 % ± 0.01 | -0.006 % ± 0.009 |
| cornell_rect, 32 | cycles | -0.05 % ± 0.03 | -0.10 % ± 0.02 | -0.09 % ± 0.01 | |
| cornell_rect, 8 / 32 | pool3nd | -0.01 / +0.01 % | -0.09 / -0.05 % | -0.11 / -0.09 % | |
| cornell_rect, 8 | pool3 (own normaliser) | +0.15 % | +0.34 % | +0.18 % | |
| mtl_glass, 8 / 32 | cycles | -0.11 / -0.08 % | -0.01 / -0.01 % | +0.01 / +0.01 % | +0.03 % ± 0.02 |
| mtl_glass, 8 / 32 | pool3nd | -0.08 / -0.05 % | -0.01 / -0.01 % | +0.03 / +0.02 % | |

The most deviating 8x8 block of the cornell seed mean:
- uniform control: +2.1 %;
- adaptive at t = 0.04: -5.9 to -6.6 % (cycles), -4.7 to -5.7 % (pool3nd);
- at 0.02: -3.5 to -3.7 %;
- at 0.01: -2.3 to -2.6 %, back at the control level.

The bias is dark for rules whose numerator uses the pixel's own samples: a pixel whose samples missed a bright path looks converged and stops. A normaliser taken from the pixel's own estimate makes it bright. It shrinks with lower thresholds and higher minimums; it does not vanish with any rule I tried.

### 4.5 Time

The emulator counts samples, not wall time. A real implementation adds:
- the convergence checks: O(pixels) per checkpoint, about log2(max/min) times;
- smaller waves when few pixels remain, unless compacted (5.4);
- worse load balance on small images (3.2).

Karma's own oracle shows how much ray savings can shrink in time (1.3: mtl_glass 6.3x rays, 1.5x time). The gate in section 6 must therefore measure wall time.

## 5. Proposed design for Nova

Numbered like the design docs, as proposals for docs/design/M7.md.

### 5.1 Error estimate

- **Signal:** the beauty RGB plus alpha; optionally further planes (LPE outputs by AOV name, like Karma's `plane`).
- **Checkpoints:** c = 2^k with c >= m (min samples, rounded up to a power of two), and at max.
- **At a checkpoint**, per pixel:
  - I = S_c / c and H = S_{c/2} / (c/2), where S is the RGB sum and S_{c/2} the snapshot kept from the previous checkpoint;
  - d = Σ_rgb |I - H|, L = Σ_rgb I, and the same for alpha;
  - the error e = sqrt(box3(d²)) / (1e-4 + n(box3(L))), n(s) = sqrt(s) below 1 and s above, with alpha's term computed alike and the maximum taken;
  - per extra plane the same with its own snapshot, maximum over planes;
  - not converged if e >= 5 t; then dilate the unconverged set by one pixel in x and y.
- **Why:**
  - the prefix halves are the only stratified halves with Nova's sampler (4.2);
  - power-of-two checkpoints need one snapshot and keep every stopped pixel a complete net;
  - the normalisation is Cycles' (sqrt below 1 is absolute error in a gamma-2 space, as Karma's default `ociomode 0` and RenderMan's perceptual weight);
  - pooling and dilation were the most robust (4.3).
- **Calibration of t is Nova's own:** pool3nd at t = 0.01 gave a mean of 95 / 103 / 33 spp on cornell / mtl_glass / dome_sky (min 16, max 1024), about uniform 88 spp quality on cornell. Choose the default from these tables (proposal: 0.01).
- **Min samples auto (-1):** max(16, sqrt(max)) rounded up to a power of two, i.e. Karma's and RenderMan's sqrt(max) with a floor that keeps the bias at the measured level (4.4).

### 5.2 Per-pixel counts and normalization

New film members:
- `std::vector<uint32_t> count` (samples per pixel);
- `std::vector<Vec4f> half` (RGB and alpha sums at the last checkpoint), plus one Spectrum snapshot per extra oracle plane;
- optionally `std::vector<float> noise` (the last e, for a debug AOV).

That is +20 bytes per pixel (+24 with the noise plane), the same as Cycles' float4 aux pass plus its count pass: about 41 MB at 1920x1080. `film.samples` becomes the count shared by all still-active pixels.

| buffer kind | today | with adaptive sampling |
|---|---|---|
| beauty RGB, alpha | sum / samples | sum / count[p] |
| LPE outputs (incl. `unoccluded;`) | sum / samples | sum / count[p] |
| `shadow;` outputs | 1 - lit / all | unchanged (ratio) |
| cryptomatte weights | w / samples | w / count[p] (ids unchanged) |
| AO utility | sum / samples | sum / count[p] |
| first-hit AOVs (depth, N, albedo, ids, P, motion, primvars ...) | written once | unchanged (first pass renders every pixel) |
| sample count (new) | — | count[p] |
| noise (new, debug) | — | e at the pixel's last check |

`WriteFromFilm` takes the per-pixel reciprocal. Pixels with count 0 (outside the region) keep the clear value.

### 5.3 Pass structure and determinism

- `renderPass` advances every active pixel from count n to n + s; all active pixels share n (lockstep).
- After the pass barrier, if n + s is a checkpoint, the renderer runs the convergence check (parallel rows; neighbours are read after all tiles have reached the checkpoint), snapshots `half` for the pixels that stay active, and rebuilds the active list.
- Decisions therefore depend only on film values at fixed sample counts. Results do not depend on thread count, chunking, IPR pass size or cancellation points.
- Each pixel's samples are still accumulated in sample-index order, so **the beauty and alpha of every pixel are bit-identical to a uniform render with count[p] samples**. This is the emulator's property (4.1), and it makes a strong test oracle.
- LPE deposits of several samples of one pixel inside one wave interleave by bounce. They are deterministic, but can differ from the uniform render in the last bits.

### 5.4 Wavefront: work items instead of tiles

- **Work items.** The active pixels are kept as a list of film indices sorted by 32x32 tile, then row. A work item is a slice of that list with at most W / s pixels, W = 1024 paths. renderWave gets (pixel span, first sample n, samples per pixel s) instead of (TileRect, sampleIndex).
- **Path slots** are pixel-major: slot = i·s + j, with sample index n + j stored per path. The new `WaveQueues::sampleIndex` replaces `ShadeContext::sampleIndex` at the six sampler sites (3.2).
- **Camera cone:** computed per tile, when the tile changes along the span.
- **Pixel ownership.** Each pixel belongs to exactly one work item per pass, so film writes stay race-free.
- **Samples per pass, s:**
  - final renders: up to the next checkpoint once the minimum is reached, so items stay near W paths even with few active pixels;
  - IPR: from a pass-time budget (the current 1 spp per pass is the special case);
  - in both cases at least about 4 work items per thread, splitting items when few remain.
- **First pass.** It still covers every pixel, including the first-hit AOVs.
- **GPU (M10).** The same layout: an active list (or a converged flag), path state per (pixel, sample), and compaction, as Cycles X does (2.1).

### 5.5 Settings (nova:* namespace, `houdini/tools/nova_settings.json`, scopes global + viewport)

| key | label | type / default | Karma counterpart |
|---|---|---|---|
| `nova:pixelOracle` | Pixel Oracle | token `uniform` \| `variance` | `karma:global:pixeloracle` style |
| `nova:samples` (existing) | Samples (the maximum with the variance oracle) | int 64 | `pathtracedsamples` |
| `nova:oracleMinSamples` | Minimum Samples | int -1 (auto, 5.1) | `minrays` |
| `nova:oracleThreshold` | Noise Threshold | float 0.01 | `variance` (different metric, hence not called variance) |
| `nova:oraclePlanes` | Planes | string `C` | `plane` |

- **Open decision for the user:** default oracle and maximum. Karma, Cycles and RenderMan ship with adaptive sampling on (lit., measured for Karma). With Nova's default of 64 samples there is little room to adapt; a default of `variance` makes sense with a larger maximum (e.g. 256).
- **Reference scenes:** `tests/reference/scenes/common.usda` must pin `nova:pixelOracle = "uniform"` as it pins Karma's.

### 5.6 IPR

- **The first m passes are uniform**, i.e. today's behaviour and latency. After that the pass cost falls as pixels converge.
- **Display.**
  - Converged pixels no longer change, so `WriteFromFilm` can skip rows or tiles without active pixels.
  - `percentDone` should combine samples and the converged share, e.g. done / (done + active · (max - n)) in sample units.
  - The annotation can read "n/max spp, x % active".
- **IsConverged** when the active list is empty or n = max.
- **Restarts.** A threshold or minimum change restarts like any setting today (simple, deterministic). A later IPR improvement (M7 "schneller Neustart") can continue the film instead: re-test all pixels with the new threshold, Cycles' `reset` (2.1), or raise the maximum.
- **Optional viewport progression:** start the viewport at 4 t and halve towards t whenever everything has converged (Cycles' interactive scheme).

### 5.7 AOVs and denoiser

- **Sample-count AOV.** A new kind that fills `count[p]`. Accept Karma's preset (`ray:pixelsamples`, intrinsic, int) plus a Nova name (e.g. raw `nova:sampleCount`). Document that Nova counts every sample while Karma counts only those that hit geometry (1.3).
- **Debug noise plane:** `nova:noise`.
- **Oracle planes:** names in `nova:oraclePlanes` resolve to LPE outputs of the bound RenderVars; unknown names warn once.
- **Cryptomatte and alpha** are covered through alpha in the error. Crypto itself is not measured.
- **Denoiser:** no coupling is needed. The guides are first-hit; if the denoiser work makes them per-sample sums, they join the per-pixel normalization. With denoising on, a higher threshold is the usual pairing (Cycles).

## 6. Test and gate plan

1. **Bit-exact prefix property (ctest, core).** Render a small scene adaptively (pool3nd, threshold such that pixels stop at several checkpoints), then uniform at each checkpoint count. For every pixel, beauty and alpha must equal the uniform render at count[p] bit for bit; LPE outputs within 1e-6 relative. Repeat with 1 and N threads and with IPR-sized and batch-sized passes; all results identical.
2. **Threshold 0 = uniform.** All pixels reach max; the film is bit-identical to the uniform render.
3. **Estimator unit tests:** synthetic films with known per-sample variance (constant regions, a noisy rectangle). Pixels inside the noisy region stay active, the dilation ring is one pixel, and constant regions stop at the minimum.
4. **Normalization.**
   - The LPE components still sum to the beauty per pixel (the M6 partition test with adaptive sampling on).
   - Cryptomatte coverage per pixel sums to its sampled share.
   - AO is in [0, 1].
   - The `shadow;` fraction is unchanged.
5. **Bias gate (reference scenes, 8 seeds).** cornell_rect, mtl_glass and dome_sky at t = 0.01 with min auto. Measured expectation: within -0.11 % to +0.03 %.
   - Global relative bias of the seed mean against the reference < 0.3 %.
   - No 8x8 block beyond the uniform control's largest deviation + 2 %.
   - The existing reference tests (two seeds, `nova_imgcmp`) pass with adaptive sampling on at a threshold giving >= 64 mean spp, without new tolerances.
6. **Equal-error time gate (benchmark script, like `tests/reference/tools/bench_lpe.py`).** Wall time of nova_cli at equal relMSE against a uniform ladder fit, min of 3 runs. Thresholds from the emulation, with margin:
   - mtl_glass >= 2.0x (emulated 3.6x in cost);
   - dome_sky >= 1.0x;
   - cornell_rect >= 0.85x (emulated 0.93-0.97x).
   - Record the results in docs/benchmarks.md.
7. **Sample-count AOV (husk).** The Karma preset `primarysamples` rendered by Nova gives integers in {min, 2·min, ..., max}. It equals max everywhere with the uniform oracle.
8. **IPR (delegate test).** Buffers report converged before max when the scene allows; `percentDone` is monotonic; a threshold change restarts and the next render matches a fresh one.
9. **Solaris UI:** the new keys in `nova_settings.json` match the delegate's descriptors (`tests/houdini/test_solaris_ui.py`).

The emulator stays useful during implementation: it predicts the exact image of a rule before the C++ exists, and test 1 checks the C++ against it.

## 7. Sources

Help (described, not copied): `solaris.zip`: `support/filters.txt`, `kug/sampling.txt`, `kug/sampling_cpu.txt`, `kug/sampling_xpu.txt`, `kug/noise.txt`, `kug/_kug_common.txt`, `kug/prims_to_pixels.txt`; `nodes.zip`: `lop/karmarendersettings.txt`, `lop/rendersettings.txt`, `lop/rendergeometrysettings.txt`; `news.zip`: `20_5/karma.txt`, `21/karma.txt`; `ref.zip`: `utils/husk.txt`.
Houdini files read: `houdini/python3.13libs/husd/karmautils.py` (`_make_oracle`); `houdini/soho/parameters/BRAY_HdKarma_Global.ds`, `BRAY_HdKarma_Aov.ds`; `dso/usd_plugins/usdKarma/resources/generatedSchema.usda`.

Literature (lit.):
- Cycles source: https://github.com/blender/cycles (`src/kernel/film/adaptive_sampling.h`, `src/kernel/film/light_passes.h`, `src/kernel/sample/pattern.h`, `src/scene/integrator.cpp`, `src/integrator/adaptive_sampling.cpp`, `src/integrator/render_scheduler.cpp`, `src/kernel/film/read.h`, `src/integrator/path_trace_work_gpu.cpp`); Blender 2.83 release notes https://developer.blender.org/docs/release_notes/2.83/cycles/
- Arnold: https://help.autodesk.com/cloudhelp/ENU/AR-Core/files/ac-render-settings/ac-sampling/arnold_user_guide_ac_render_settings_ac_adaptive_sampling_html.html ; RGBA-only note (Arnold 7.1.4 via C4DtoA 4.5.0): https://help.autodesk.com/cloudhelp/ENU/AR-Cinema4D/files/release-notes/4x/arnold_for_cinema_4d_450_html.html ; GPU guidance: https://help.autodesk.com/cloudhelp/ENU/AR-Maya/files/am-Arnold_for_Maya_User_Guide/arnold_for_maya_am_Getting_Started_with_Arnold_GPU_html.html
- RenderMan 25 Sampling Modes: https://rmanwiki-25.pixar.com/space/REN25/20416460.md ; options (search snippets only): https://rmanwiki.pixar.com/x/xgBeAw ; denoiser AOVs: https://renderman.atlassian.net/wiki/spaces/REN26/pages/19661815/Denoiser+AOVs
- MoonRay adaptive sampling: https://docs.openmoonray.org/user-reference/how-to-guides/adaptive-sampling/ ; Jeffery, "Global Adaptive Sampling Hierarchies in Production Ray Tracing", SIGGRAPH 2019 Talks, DOI 10.1145/3306307.3328205
- Kirk, Arvo, "Unbiased sampling techniques for image synthesis", SIGGRAPH 1991, DOI 10.1145/127719.122735 (https://authors.library.caltech.edu/records/8sntk-73q87)
- Dammertz, Hanika, Keller, Lensch, "A Hierarchical Automatic Stopping Condition for Monte Carlo Global Illumination", WSCG 2010 Full Papers, pp. 159-164 (content via the Cycles citation; paper not fetched)
- Christensen, Kensler, Kilpatrick, "Progressive Multi-Jittered Sample Sequences", CGF 37(4), 2018, DOI 10.1111/cgf.13472 (section 10.2.1 via Cycles; paper not fetched)
- Zwicker et al., "Recent Advances in Adaptive Sampling and Reconstruction for Monte Carlo Rendering", CGF 34(2), 2015
- Burley, "Practical Hash-based Owen Scrambling", JCGT 9(4), 2020, https://jcgt.org/published/0009/04/01/
- OIDN README (training data range): https://github.com/RenderKit/oidn

Unverified: Arnold's metric; RenderMan's neighbourhood and check frequency; Kirk & Arvo's exact fix; any official OptiX statement on adaptive sampling.

## 8. Not measured / open

- Karma XPU's oracle; Automatic mode's secondary sampling; Karma's oracle with depth of field or motion blur (the help recommends the uniform oracle there; Nova's dof and motion_blur scenes were not emulated).
- Whether `primarysamples` records a scheduled sample index (1.3).
- Real wall time of an adaptive Nova: the emulation counts samples with a measured per-pixel cost model; wave-occupancy losses are not in it.
- Images larger than the reference scenes (128-192 px wide): the checkpoint overhead and the cost of the fixed minimum shrink relative to the work, so the savings should not be smaller there, but this is unmeasured.
- Noise metrics in an OCIO display space (Karma's `ociomode` 1/2).
- An unbiased variant (e.g. Russian roulette on continuing a pixel, weighted by the inverse continuation probability) was considered but not prototyped: it trades the ~0.1 % bias for extra variance.

## 9. Files

| file | what |
|---|---|
| `probes/as_helpgrep.py` | lists or prints help pages by pattern (output stays in the scratch directory) |
| `probes/as_probe_krs_sampling.py` → `as_krs_sampling_parms.txt`, `as_krs_sampling_attrs.txt` | Karma Render Settings sampling parameters and the attributes they author |
| `probes/as_karma_adaptive.py` → `as_results/as_karma_<scene>.json` | Karma renders under uniform/variance oracles with `primarysamples`, ray counts, times, errors |
| `probes/as_karma_tables.py` → `as_results/as_karma_tables.txt` | equal-error tables from those JSONs |
| `probes/as_karma_cost.py` | Karma `cputime` map (found to be per bucket) |
| `probes/as_results/as_husk_filters.txt` | `husk --renderer BRAY_HdKarma --filters` |
| `prototypes/as_nova_ladder.py` | Nova sample ladders with nova_cli (prefix property) |
| `prototypes/as_adaptive_emulator.py` → `as_results/emu_*.{txt,json}`, `emu_nodilate_*.txt`, `bias_*.json` | stopping rules replayed on the ladders; savings and bias |
| `prototypes/as_nova_crop_cost.py` → `as_results/crop_cost.txt` | Nova cost per pixel-sample in image regions |
| `prototypes/as_sobol_split.py` → `as_results/sobol_split.txt` | numpy port of Nova's sampler; half-split stratification and calibration |
| `prototypes/as_results/ladder_times_*.json` | nova_cli times of the ladder renders |

Re-run, from the repo root in PowerShell, with `$py = "$HFS\python313\python.exe"`:
```
$py -I docs\research\m7\probes\as_karma_adaptive.py --work <dir> --scene mtl_glass --uniform 64,256,1024 --variance 0.02,0.01,0.005 --max 1024
$py -I docs\research\m7\prototypes\as_nova_ladder.py --cli <copy of nova_cli.exe> --work <dir> --scene mtl_glass --seeds 1 --kmin 3 --kmax 10 --truth 4096 --truth-seeds 1001,1002
$py -I docs\research\m7\prototypes\as_adaptive_emulator.py --ladder <dir>\mtl_glass --scene mtl_glass --truth nova --min 16 --max 1024 --coverage <karma dir>\mtl_glass_uniform_1024.exr --cost-bg 0.50 --cost-geo 1.47 --rules cycles,pool3nd,tile8,tile32
$py -I docs\research\m7\prototypes\as_sobol_split.py --pixels 2000 --kmax 8
powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m7\probes\as_probe_krs_sampling.py <dir>
```
