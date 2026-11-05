# Nova M3 texturing report: OIIO TextureSystem, UDIM, tx/mipmaps, filter footprints, color spaces and the later GPU path

Everything marked "measured" was run on this machine (Houdini 22.0.466, OIIO 2.5.18, OCIO 2.5.0, MaterialX 1.39.5) with throwaway programs and husk/Karma runs under `…/scratchpad/m3research/textures/`. Nothing in the project tree was changed.

---

## 0. Decisions in short

1. **One private `TextureSystem` on top of the process-wide shared `ImageCache`:** `TextureSystem::create(false, ImageCache::create(true))`.
   - It inherits husk's `--oiio-max-memory`, `--oiio-max-files` and `--oiio-max-texture-res`, and it shares tiles with Houdini's other OIIO users.
   - It keeps the TextureSystem-level options private. These are `flip_t=1` (gives the MaterialX/USD v-up orientation including UDIM rows), `gray_to_rgb=1` and `stochastic=3`.
2. **Don't use OIIO's `colortransformid` (`TextureOpt`).** In 2.5.18 it is ignored for untiled files and for automip levels, and it quantizes to 8 bit unless `forcefloat` is set (measured, §1.8).
   - Color is instead baked into a **converted texture**: tiled 64², MIP-mapped, half float, scene_linear, made with `ImageBufAlgo::make_texture`.
   - Linear-to-linear gamut changes (e.g. ACEScg) are a **3×3 matrix applied after the lookup**, which is exact under filtering.
3. **The same conversion also covers untiled or unmipped inputs, and `.rat` via `imaketx`.** OIIO cannot read `.rat` (measured).
4. **Filter footprint: ray cones, two floats per path slot** (`coneWidth`, `coneSpread`).
   - At each hit the cone becomes OIIO-style derivatives `(dsdx, dtdx, dsdy, dtdy)`: the anisotropic ellipse of JCGT 2021 eqs. 8–9 is projected through the triangle's uv gradients.
   - The spread widens per bounce like Cycles' `bsdf_widen_dD`.
   - `tex2DGrad` on the GPU takes exactly the same inputs.
5. **Default derivative scale 0.25 × pixel footprint, for Karma parity.** Measured: Karma CPU picks MIP level = log2(texels per pixel) − 2, independent of pixel samples.
6. **Core stays free of OIIO.** `core/texture/` holds the POD sampler descriptors, the ray-cone math and an `ITextureSampler` interface (virtual per batch, like `IAccel`). The OIIO implementation and registry live in `hydra/`.
7. **Missing textures follow MaterialX and Karma.** The node's `default` value is returned, with one error message per file. Missing UDIM tiles and negative u/v also return `default`, silently (all measured on Karma).

---

## 1. OIIO in Houdini 22.0.466

### 1.1 Version, namespace and libraries
- `toolkit/include/OpenImageIO/oiioversion.h:45-47`: 2.5.18; `:121` `namespace OIIO = HOIIO_v2_5;`; `:189` `OIIO_BUILD_CPP 20`.
- Import libraries: `$HFS/custom/houdini/dsolib/OpenImageIO_sidefx.lib`, `OpenImageIO_Util_sidefx.lib` and `OpenColorIO_sidefx.lib`. DLLs are in `$HFS/bin`. These are already wired as `Nova::OIIO` in `cmake/NovaHoudini.cmake:37-52`.
- Houdini's Python `OpenImageIO.cp313-win_amd64.pyd` and `husk.exe` link the same `OpenImageIO_sidefx.dll`, so one shared ImageCache exists per process.
- Readers (`hoiiotool --help`): bmp, cineon, dds, dpx, fits, hdr, heif, ico, iff, jpeg, openexr, png, pnm, psd, rla, sgi, softimage, targa, tiff, zfile. **There is no rat reader.**
- Feature macros, `texture.h:22-29`: `OIIO_TEXTURESYSTEM_SUPPORTS_COLORSPACE`, `…_STOCHASTIC`, `…_GETATTRIBUTETYPE`, `…_CLOSE`.

### 1.2 Creating and destroying
- `static TextureSystem *create(bool shared=true, ImageCache *imagecache=nullptr);` at `texture.h:507`.
- `static void destroy(TextureSystem*, bool teardown_imagecache=false);` at `texture.h:527`.
- With `shared=false` and a non-null `imagecache`, the caller owns the cache and frees it after the TextureSystem (`texture.h:495-503`).
- Verified (`probe2.cpp`, case c): a private TS on the shared IC reports `imagecache()==shared`. The private `flip_t=1` does not leak into the shared TS (shared still 0). `max_memory_MB` read through the TS reflects the shared IC.

What husk sets on the shared IC (husk run with a `--prerender-script` querying `oiio.ImageCache(True)` in-process), against OIIO's defaults (plain Python):

| attribute | OIIO default | husk 22.0.466 |
|---|---|---|
| `max_memory_MB` | 1024 | 31531 (50 % of RAM, `--oiio-max-memory-percent 50`) |
| `max_open_files` | 100 | 4096 |
| `autotile` / `autoscanline` | 0 / 0 | 512 / 1 |
| `automip`, `forcefloat` | 0, 0 | 0, 0 |
| `accept_untiled`, `accept_unmipped`, `deduplicate` | 1, 1, 1 | same |
| `max_mip_res` (an IC attribute, `imagecache.cpp:2276`) | 2^30 | 2^30 (`--oiio-max-texture-res`) |
| `$OCIO` | unset (OIIO falls back to `ocio://default`) | `…/packages/ocio/houdini-config-v3.0.0_aces-v2.0_ocio-v2.5.ocio` |

### 1.3 Attributes (IC: `imagecache.h:101-236`; TS: `texture.h:536-640`)
- `max_open_files` (Default = 100, `imagecache.h:103-107`).
- `max_memory_MB` (Default: 1024.0 MB, `:108-110`).
- `autotile`/`autoscanline` (`:121-149`).
- `automip` (`:150-158`): emulates MIP levels for untiled single-subimage files.
- `accept_untiled` and `accept_unmipped` (`:159-173`).
- `forcefloat` (`:174-180`): converts all cached tiles to float.
- `failure_retries` (`:181`), `deduplicate` (`:190`), `unassociatedalpha` (`:209`), `max_errors_per_file` (`:215`), `trust_file_extensions` (`:222`).
- `colorspace` and `colorconfig` (`:228-233`). The working space defaults to `"scene_linear"` (`imagecache.cpp:1688`).
- TS-only attributes:
  - `gray_to_rgb` (`texture.h:588`): **default 0, so a grey file returns (g, fill, fill)** (measured).
  - `max_mip_res` (`:605`).
  - `flip_t` (`:620`).
  - `stochastic` (`:623`, bits: MIP=1, Aniso=2, Texel=4 per `texture_pvt.h:564-568`).
  - `max_tile_channels` (it reads 6, although the doc says 5).

### 1.4 `TextureOpt` (`texture.h:229-271`) and `TextureOptBatch` (`:309-341`)
- Default constructor (`:229-238`): `interpmode=InterpSmartBicubic`, `anisotropic=32`, `conservative_filter=true`, `swidth=twidth=1`, `rnd=-1`, `colortransformid=0`.
- Fields:
  - `firstchannel`, `subimage`, `subimagename`.
  - `swrap`, `twrap` (`:249`).
  - `mipmode` (`:251`), `interpmode`, `anisotropic`.
  - `sblur`, `tblur` (`:255`): a fraction of the texture width.
  - `swidth`, `twidth` (`:256`): derivative multipliers.
  - `fill`, `missingcolor` (`:258`), `time`.
  - `union{bias; rnd}` (`:260-263`), `samples`.
  - `rwrap`, `rblur`, `rwidth`, `colortransformid` (`:271`).
- `TextureOptBatch`: the per-lane arrays are `sblur, tblur, rblur, swidth, twidth, rwidth, rnd[16]` (`:315-326`); everything else is uniform. `BatchWidth=16` (`:131`).
- **Pitfall: `texture()` writes to the caller's `TextureOpt`.**
  - `options.subimage = s` (`texturesys.cpp:1205`).
  - `options.swrap = texturefile->swrap()` when `WrapDefault`, and `→ WrapPeriodicPow2` (`:1218-1225`).
  - So build one `TextureOpt` per (batch, sampler, thread) and never share it across threads.

### 1.5 Lookup API, derivative convention and MIP selection
- Single-point lookup, `texture.h:938-943`:
  ```cpp
  virtual bool texture(TextureHandle*, Perthread*, TextureOpt&, float s, float t,
                       float dsdx, float dtdx, float dsdy, float dtdy,
                       int nchannels, float* result, float* dresultds=nullptr, float* dresultdt=nullptr) = 0;
  ```
  The filename variant is at `:929`.
- The batched form (`texture.h:1190-1196`; mask, SoA results `float[nch][16]`) is a plain loop in 2.5.18: `// (FIXME) CHEAT! Texture points individually` (`texturesys.cpp:1335`). **It brings no speed-up**, so call the scalar handle form in a loop.
- **Derivative convention, `texturesys.cpp:1774-1779`:** "If the derivs passed in are the full pixel-to-pixel derivatives, then majorlength/minorlength are the (diameter) axes … We do the former!" So pass **full-diameter** footprint vectors.
- **MIP level rule, `texturesys.cpp:1560-1577`:** for each level m, `filtwidth_ras = minorlength*minwh[m]`. The first level with ≤ 1 texel gives the pair (m−1, m) with `blend = clamp(2*filtwidth_ras − 1, 0, 1)`.

Measured on a 1024² texture whose level L holds the constant L (made with `maketx:mipimages`), isotropic derivatives d = w/1024:

| footprint w (texels) | 0.5 | 1 | 1.5 | 2 | 3 | 4 | 8 | 16 | 64 |
|---|---|---|---|---|---|---|---|---|---|
| Default (aniso) and Trilinear | 0 | 0 | 0.5 | 1 | 1.5 | 2 | 3 | 4 | 6 |
| OneLevel | 0 | 0 | 1 | 1 | 2 | 2 | 3 | 4 | 6 |

- Anisotropy, 16 × 1 texel footprint: `anisotropic=1` → level 3.06; 4 → 1.25; 16 or 32 → 0.
- Blur: `sblur=tblur=0.01` → level 3.4; 0.1 → level 6.6.

### 1.6 Perthread data and handles (`texture.h:756-832`)
- `Perthread* get_perthread_info(Perthread* = nullptr)` at `:798`. It is thread-specific, released at thread exit, and **must never be used by two running threads**. Passing it in also does per-thread "housekeeping".
- `create_thread_info()` and `destroy_thread_info()` are the manual alternative.
- `TextureHandle* get_texture_handle(ustring filename, Perthread* = nullptr, const TextureOpt* = nullptr)` at `:818`. The `options` argument is used only to make separate handles per color space.
- `bool good(TextureHandle*)` at `:832`. Measured: a missing file still gets a non-null handle, with `good()==0`.
- `get_texture_info(handle, pt, subimage, "exists" | "udim" | "resolution" | "miplevels" | "channels" | "format" | "averagecolor" | "stat:*" …)` at `:1622-1630`.
- `invalidate(ustring, force)`: with `force=false` the file is invalidated only if it changed on disk (`imagecache.h:873-887`).

### 1.7 UDIM (`texture.h:1747-1803`, `imagecache.cpp:3636-3790`)
- Recognized patterns (`imagecache.cpp:3644-3653`): `<UDIM>`, `%(UDIM)d`, `<u>`, `<v>`, `<uvtile>`, `_u##v##`, `<U>`, `<V>`, `<UVTILE>`. The tiles are inventoried once by a directory regex scan (`:3660-3710`).
- Tile resolution, `texturesys.cpp:733-742`: `utile = max(0, int(s)); vtile = max(0, int(t));`. Then `s -= floorf(s); t -= floorf(t);` (`:1177-1183`). `flip_t` is applied **after** that (`:1248`).
- USD 26.05 keeps `<UDIM>` in the resolved path (`usdImaging/materialParamUtils.cpp:78-110, 312-318`), so the pattern reaches OIIO unchanged.

Measured with tiles 1001 red, 1002 green, 1011 blue, 1012 missing:

| (s,t) | OIIO, no `missingcolor` | with `missingcolor` (1,0,1) | Karma CPU (MaterialX `default` = 0.25) |
|---|---|---|---|
| (0.5,0.5) / (1.5,0.5) / (0.5,1.5) | red / green / blue, ok=1 | same | same |
| (1.5,1.5), the missing tile | ok=0, rgb=0, **no error text** | ok=1, magenta | **default (0.25), no warning** |
| (12.5,0.5), beyond the inventory | ok=0 | magenta | — |
| (−0.5,0.5) | **red (tile 1001): `int()` truncates towards 0** | red | **default** |

- Orientation (tile 1011 with a white top half and a blue bottom half):
  - `flip_t=0`: t=1.25 gives the image's top half.
  - `flip_t=1`: v=1.25 gives the bottom half. That is the USD/MaterialX convention, and Karma renders exactly this.
  - Doing the flip yourself (`t = floor(v) + 1 − frac(v)`) puts integer v into the wrong tile row, so use `flip_t=1`.
- `inventory_udim` lists empty entries for missing tiles (printed as `(null)`); here it returned nu=2, nv=2.

### 1.8 Color transforms in the TextureSystem
- `int get_colortransform_id(ustring from, ustring to)` at `texture.h:843`. The implementation (`texturesys.cpp:575-591`) uses `ColorConfig::default_colorconfig()` (which reads `$OCIO`, `color_ocio.cpp:72-76, 821`). It returns 0 for identity and −1 for an unknown space; the id packs `((from+1)<<16)|(to+1)`.
- The conversion is applied **only in `ImageCacheFile::read_tile` for tiled files**, after the tile is read: `if (id.colortransformid() > 0) ImageBufAlgo::colorconvert(wrapper, …, m_imagecache.colorspace(), …)` (`imagecache.cpp:864-876`).
  - The target is always the IC's `colorspace` attribute; the "to" half of the id is ignored.
  - The conversion runs in the tile's cached type. That type stays uint8, uint16 or half unless `forcefloat` is set (`imagecache.cpp:388-397`).
- **Untiled files (`read_untiled`, `:990-1090`) and automip levels (`read_unmipped`, `:895-985`) are never converted.**

Measured with a constant sRGB code; expected values are 10 → 0.003035 and 128 → 0.215861:

| file | `forcefloat=0` fine / coarse | `forcefloat=1` fine / coarse |
|---|---|---|
| `c10.png` (untiled) | 0.039216 / 0.039216 (**not converted**) | same |
| `c10_tiled.tif` (tiled, unmipped, automip) | 0.003922 / **0.039216** | 0.003035 / **0.039216** |
| `c10_u8.tx` (tiled, mipped, uint8) | **0.003922** (quantized: 1/255) | 0.003035 |
| `c128_u8.tx` | 0.215686 | 0.215856 |

So `colortransformid` is correct only for tiled, MIP-mapped files with `forcefloat=1`. `forcefloat` is an IC-wide setting (4× memory for 8-bit data, and it would change the shared IC). Don't rely on it.

### 1.9 Errors, missing files and thread safety
- `missing_texture()` (`texturesys.cpp:977-1000`) writes `missingcolor` if given, else `fill`. With `missingcolor` set it returns **true and swallows the error**; otherwise it returns false.
- Measured: a missing file gives ok=0, `geterror()=="Could not open file …"`, but `has_error()` reported 0. Read the message with `geterror()` in the same thread right after the failure.
- `getstats(level, icstats)` is at `texture.h:1850`. IC statistics include `stat:cache_memory_used`, `stat:bytes_read`, `stat:fileio_time`, `stat:unique_files` (`imagecache.h:260-316`), and per file `stat:mipsused`, `stat:bytesread`, …
- Threading: lookups from many threads at once are the intended use. The only constraint documented in the header is the Perthread rule. Measured scaling (4k tx, random coordinates, 1-texel footprint): 4.2 / 7.5 / 14.2 / 26.8 M lookups/s on 1 / 2 / 4 / 8 threads.

Single-thread lookup costs, warm cache, random uv on a 4k half tx:

| | 1-texel footprint | 64-texel footprint |
|---|---|---|
| handle + Perthread, Default (aniso, smart bicubic) | 240 ns | 74 ns |
| by filename | 251 ns | 88 ns |
| Trilinear + Bilinear | 184 ns | 47 ns |
| Closest, NoMIP | 151 ns | 152 ns |

### 1.10 Stochastic filtering (OIIO 2.5)
`TextureOpt::rnd ∈ [0,1)` together with the TS attribute `stochastic` picks one MIP level (bit 1) and/or a single anisotropic probe (bit 2) (`texturesys.cpp:1616-1627, 1651, 1870-1908`).

Measured with a 4:1 anisotropic footprint:

| `stochastic` | 6 × 1.5 texels | 24 × 6 texels |
|---|---|---|
| 0 | 734 ns | 522 ns |
| 1 | 476 ns | 264 ns |
| 3 | 372 ns | 207 ns |

The mean over stratified `rnd` matches the deterministic blend (1.5013 vs 1.5000). A path tracer averages this noise away, so use `stochastic=3` with a Sobol stream.

---

## 2. tx and MIP production, and `.rat`

### Tools in `$HFS/bin`
There is no `maketx.exe`. The available tools are `imaketx.exe`, `hoiiotool.exe` (`-otex`, `-oenv`), `iconvert.exe` and `iinfo.exe`.

Measured on a PNG with the constant sRGB code 128:

| command | result |
|---|---|
| `imaketx in.png out.exr` | tiled 64, half, MIP-mapped, **linearized** (0.21582), `oiio:ColorSpace=lin_rec709`, `houdini:SHA1` |
| `imaketx in.png out.tx` | tiled, mipped, **uint8 with sRGB data and no colorspace tag**; `oiio:AverageColor` holds the linear value |
| `imaketx in.png out.rat` | RAT format, 8-bit, "srgb" |
| `hoiiotool out.rat …` | `ERROR: could not find a format reader for "c128.rat"` |
| `iconvert in.rat out.exr` | scanline, half, 0.2196: **gamma 2.2, not sRGB** (0.2159) |
| `imaketx in.rat out.exr` | tiled, mipped, half, exact 0.21582 |

`imaketx` defaults (from `--help`): tiles 64, box filter, `-l 2` ("Three channel 8-bit images are assumed to be in sRGB"), `--ocio`. It ran without a license prompt.

**`.rat` policy:** convert once with `imaketx <src.rat> <cache>.exr` as a subprocess into the Nova texture cache, and log it. If `imaketx` is unavailable, warn once and return the node's `default`. Don't use `iconvert`, because of its gamma 2.2.

### In-process conversion
API (`imagebufalgo.h:2161-2390`):
```cpp
bool make_texture(MakeTextureMode mode, string_view filename, string_view outputfilename,
                  const ImageSpec& config, std::ostream* outstream = nullptr);
```
Useful `config` keys:
- `maketx:incolorspace` and `maketx:outcolorspace`.
- `maketx:fixnan` (`"box3"`), `maketx:highlightcomp`, `maketx:filtername`.
- `maketx:constant_color_detect`, `maketx:opaque_detect`, `maketx:monochrome_detect`.
- `maketx:fileformatname`, `maketx:hash`, `maketx:mipimages`, `maketx:unpremult`.
- `wrapmodes`, which defaults to `"black,black"`.

Measured:
- A 4096² uint8 PNG converts to a half, linear, tiled, mipped EXR in **453 ms** (1 call). Dark code 10 comes out as exactly 0.003035.
- **The output's `oiio:ColorSpace` stays "sRGB" (copied from the input) even though the pixels are linear.** Set `config.attribute("oiio:ColorSpace", "lin_rec709")` explicitly; config metadata overrides the input's.
- maketx-produced textures carry `wrapmodes "black,black"`. With `WrapDefault` they would turn black outside [0,1], so always pass explicit wrap modes.

### automip cost (untiled 4k uint8 PNG, private TS)
- The first level-0 lookup takes 59 ms (whole-image decode, a single tile when `autotile=0`).
- The first level-4 lookup takes 320 ms, single-threaded. `read_unmipped` builds every texel with a 2×2 `get_pixels` call (`imagecache.cpp:948-967`).
- automip raises the cache minimum to 2× the image bytes (`imagecache.cpp:943`), and the color transform is not applied to the generated levels.

### What other renderers do
- **Karma:** "Karma will auto-convert any textures which aren't tiled/mip-mapped … converted files live alongside the original … with the `.rat` extension added" (`HOUDINI_TEXTURE_DISK_CACHE`). Source: help `solaris/kug/textures.txt`, section "Auto-Conversion". XPU does the same (`solaris/karma_xpu.txt:339`).
- **Arnold (Arnold for Houdini docs):** TX files are generated by default next to the source, and regenerated only when the source content or its color space changes.

### Recommended setup for Nova
- Private TS options: `flip_t=1`, `gray_to_rgb=1`, `stochastic=3`.
- Per lookup: `anisotropic=8`, `conservative_filter=true`.
- Memory, open files and max resolution come from the shared IC (husk's options apply).
- Use only converted or already linear, tiled, mipped textures, so `automip` can stay 0.
- An explicit `nova:textureCacheMemory` would set the shared IC's `max_memory_MB`. Use it only when authored, and document that it affects the whole process.

---

## 3. Filter footprints

### 3.1 Options

| method | state per ray | after diffuse/glossy bounces | source |
|---|---|---|---|
| Ray differentials (Igehy 1999) | 12 floats (dP, dD for x and y) | not defined (needs path differentials) | pbrt-v4 `interaction.cpp:41-86` |
| pbrt-v4 | camera rays: full differentials; specular: propagated | `Camera::Approximate_dp_dxy`: as if the camera saw the point directly; scaled by `max(1/8, 1/sqrt(spp))` | `cameras.h:155-183`, `cameras.cpp:154-194` |
| Cycles compact differentials | 2 floats: `dP` (width), `dD` (spread) | `dD = max(prev_dD, sqrt(avg_roughness²))`, MIS-weighted roughness, applied the same way for NEE and BSDF rays | `kernel/util/differential.h:108-167`, `integrator/shade_surface.h:585-590, 436`, `closure/bsdf.h:59-79` |
| Ray cones (Akenine-Möller et al., JCGT 10(1) 2021) | 2 floats (width, spread angle) | spread += curvature term + roughness term | paper eqs. 1-9 and App. A; Falcor `TexLODHelpers.slang:99-114, 255-280` |
| **Karma CPU (measured)** | — | — | see below |

Notes on Cycles: the transfer is `dP + t·dD`. At a hit, `differential_from_compact(Ng, dP)` gives an isotropic footprint (no foreshortening). The code comment says widening "is needed to make image cache memory usage scale with render tile size rather than overall resolution", and that NEE and BSDF sampling must use the same widening, "otherwise … seams".

**Karma CPU, measured.** Orthographic camera; a plane with a 1024² texture whose level L = L; emission shading.
- 16 texels/pixel gives level 2.00 (pixel samples 1, 4, 9, 16 and 64 all give 2.00).
- 64 texels/pixel gives level 4.03; 4 texels/pixel gives level ≈ 0.
- So Karma's footprint is **0.25 × pixel spacing and does not depend on spp**.
- At grazing angles (perspective, 8° tilt) Karma blurs less than an isotropic major-axis footprint would. At z = −0.57 it shows level 0.44, while the isotropic major axis predicts 1.42 and the minor axis 0. So Karma filters anisotropically, with limited anisotropy or a different measure.

### 3.2 Recommendation: ray cones in the SoA wavefront
Add to `WaveQueues` (`core/integrator/wavefront.h:63-83`), indexed by path slot: `std::vector<float> coneWidth, coneSpread;` (8 bytes per path, the same layout on the GPU). Shadow rays reuse their path's values through `ShadowQueue::rays.path`, which matters only for future opacity textures.

Formulas (all quantities in world space; d is the unit ray direction; f is the unit geometric normal of the hit triangle):
- **Camera:** `spread0 = ∠(d(x,y), d(x+1,y))`, averaged over x and y. For a pinhole this is `atan(2·tan(fovY/2)/H)` (Falcor `Camera.cpp:298-303`). `width0` is the origin step: orthographic gives the pixel size; perspective gives `near·spread` because Nova starts rays at the near plane.
- **Transfer:** `W(t) = width + spread·t` (Falcor `propagateDistance`, Cycles `differential_transfer_compact`).
- **Footprint ellipse:** JCGT eqs. 8-9 with the diameter.
  - `h1 = normalize(d − (f·d) f)`, `a1 = h1·W/|f·d|`.
  - `a2 = (f × h1)·W`.
- **uv gradients** (barycentric gradients; robust when the uv triangle is degenerate):
  - `e1 = P1−P0`, `e2 = P2−P0`, `n = e1×e2`.
  - `∇b1 = (e2×n)/|n|²`, `∇b2 = (n×e1)/|n|²`.
  - `∇u = Δu1 ∇b1 + Δu2 ∇b2`, `∇v = Δv1 ∇b1 + Δv2 ∇b2`.
- **Derivatives:** `dsdx = k·∇u·a1`, `dtdx = k·∇v·a1`, `dsdy = k·∇u·a2`, `dtdy = k·∇v·a2`.
  - `k = nova:textureFilterWidth` (default **0.25**, Karma parity) × the per-node `karma_width`.
  - The isotropic equivalent is JCGT eq. 1: λ = ½ log2(t_a/p_a) + log2 W − log2 |n·d|.
- **Scattering:**
  - `spread' = spread + β_c`, with β_c = −2 k_c W/(n·d) (JCGT eq. 5). The triangle curvature is `k_c = mean over edges of (n_j−n_i)·(P_j−P_i)/|P_j−P_i|²` (eqs. 6-7, using vertex normals; 0 for flat meshes).
  - Then, for a non-specular lobe, `spread' = max(spread', α_lobe)` (Cycles rule; α is the microfacet alpha; diffuse counts as 1; specular α=0 keeps the cone).
  - `width' = W(t)`. Keep the spread signed (concave mirrors focus) and use |W| for lookups.
- **Supersampling:** pbrt-v4 shrinks the footprint by `max(1/8, 1/sqrt(spp))`. Karma does not. Nova follows Karma (measured).

Ready-to-adapt code (`core/texture/ray_cone.h`; formulas checked in `conemath.cpp`: at θ = 0, 60° and 85° the major axis is W/cosθ and the minor axis W, exactly):
```cpp
#pragma once
#include "core/math/vec.h"
#include <cmath>
namespace nova {

struct StDerivs { float dsdx, dtdx, dsdy, dtdy; };

NOVA_INLINE float coneWidthAt(float width, float spread, float t) { return width + spread * t; }

// World-space gradients of the texture coordinates over the triangle plane.
NOVA_INLINE bool uvGradients(Vec3f p0, Vec3f p1, Vec3f p2, Vec2f t0, Vec2f t1, Vec2f t2, Vec3f &dudP, Vec3f &dvdP) {
    const Vec3f e1 = p1 - p0, e2 = p2 - p0, n = cross(e1, e2);
    const float nn = dot(n, n);
    if (!(nn > 0.0f)) return false;
    const Vec3f g1 = cross(e2, n) * (1.0f / nn), g2 = cross(n, e1) * (1.0f / nn);
    const float du1 = t1.x - t0.x, dv1 = t1.y - t0.y, du2 = t2.x - t0.x, dv2 = t2.y - t0.y;
    dudP = g1 * du1 + g2 * du2;
    dvdP = g1 * dv1 + g2 * dv2;
    return true;
}

// Cone of diameter w along d hitting the plane with unit normal f: the two
// pixel-to-pixel footprint vectors (OIIO/tex2DGrad take full diameters).
NOVA_INLINE void coneFootprint(Vec3f d, Vec3f f, float w, Vec3f &ax, Vec3f &ay) {
    Vec3f h1 = d - f * dot(f, d);
    const float l = length(h1);
    h1 = l > 1e-6f ? h1 * (1.0f / l) : normalize(cross(f, std::fabs(f.x) < 0.9f ? Vec3f{1, 0, 0} : Vec3f{0, 1, 0}));
    const float cosT = std::fmax(std::fabs(dot(f, d)), 1e-4f); // clamp: grazing hits
    ax = h1 * (w / cosT);
    ay = cross(f, h1) * w;
}

NOVA_INLINE StDerivs stDerivatives(Vec3f dudP, Vec3f dvdP, Vec3f ax, Vec3f ay, float k) {
    return {k * dot(dudP, ax), k * dot(dvdP, ax), k * dot(dudP, ay), k * dot(dvdP, ay)};
}

NOVA_INLINE float triangleCurvature(Vec3f p0, Vec3f p1, Vec3f p2, Vec3f n0, Vec3f n1, Vec3f n2) {
    auto edge = [](Vec3f pa, Vec3f pb, Vec3f na, Vec3f nb) {
        const Vec3f e = pb - pa; const float ee = dot(e, e);
        return ee > 0.0f ? dot(nb - na, e) / ee : 0.0f;
    };
    return (edge(p0, p1, n0, n1) + edge(p1, p2, n1, n2) + edge(p2, p0, n2, n0)) * (1.0f / 3.0f);
}

// dotNd = dot(n, d) < 0 for the side the ray arrives from; alpha: sampled lobe
// roughness (0 specular, 1 diffuse).
NOVA_INLINE float scatterSpread(float spread, float widthAtHit, float curvature, float dotNd, float alpha) {
    const float s = spread - 2.0f * curvature * widthAtHit / std::fmin(dotNd, -1e-4f);
    return alpha > 0.0f ? std::fmax(s, alpha) : s;
}
} // namespace nova
```

Where it plugs into Nova:
- `generateCameraRays` (`wavefront.cpp:137-160`): set `coneWidth`/`coneSpread` from `Camera::rayCone(ndc)`. This is a new helper: three `generateRay` calls through (x, y), (x+1, y), (x, y+1) at the lens center. It does not depend on the sample index, so it can be cached per tile.
- `shadeSurfaces` (`wavefront.cpp:169-300`): after `resolveHit` (`:230`), compute `W` and the st derivatives into the shading SoA. After the BSDF sample (`:287`), update the cone with `scatterSpread`.
- `renderFirstHitAovs` (`:80-130`): the albedo AOV uses the center-ray cone and deterministic filtering (`rnd = −1`).
- `SurfaceHit` and `resolveHit` (`scene.cpp:274-350`) already compute the world-space corners `p0..p2`. They additionally need corner st values: `MeshDesc::cornerSt` (3 per triangle, built like `cornerColors` at `mesh.h:32`; face-varying refinement for subdivision surfaces like `gatherSubdivColor`).
- **VM requirement:** texcoord-typed registers carry `Dual2{val, ddx, ddy}`. Affine ops (MaterialX `tiledimage` tiling and offset, `place2d`, rotate, multiply) propagate derivatives. Non-differentiable ops fall back to the geometric derivatives of the st set.
- **Sobol:** use the reserved per-bounce slot `stream::kSpare = 7` (`core/math/sobol.h:152`) as `kTexture` (1D) for `TextureOpt::rnd`. Decorrelate textures at the same hit with `rnd' = fract(rnd + hash(textureId)·φ)`. Reusing the reserved slot doesn't reshuffle existing noise.

---

## 4. Color management

### 4.1 Houdini's OCIO config
`$HFS/packages/ocio/houdini-config-v3.0.0_aces-v2.0_ocio-v2.5.ocio`; husk sets `$OCIO` to it (measured).
- Roles (`:15-24`): `scene_linear: Linear Rec.709 (sRGB)`, `data: Raw`.
- File rules (`:26-46`):
  - Name patterns `*srgb_tx*`, `*srgb_texture*` → sRGB; `*ACEScg*` → ACEScg; `*lin_rec709*`, `*lin_srgb*` → linear.
  - Rules for files packed in usdz.
  - Extensions jpg, jpeg, tga, png, tif, tiff → sRGB; exr → linear.
  - **Default rule → ACES2065-1.** OIIO skips the default rule: `getColorSpaceFromFilepath` checks `filepathOnlyMatchesDefaultRule`, then `parseColorSpaceFromString` (`color_ocio.cpp:2157-2170`).
- Measured `getColorSpaceFromFilepath`: `.png` → sRGB; `.tx`, `.rat`, `.hdr` → "" (falls through); `_ACEScg.exr` → ACEScg; `foo_srgb_texture.tx` → sRGB; `foo.1001.exr` → linear.
- MaterialX color space names resolve as aliases:
  - `srgb_texture` → sRGB Encoded Rec.709 (`:464-465`).
  - `lin_rec709` (`:700-701`).
  - `acescg` (case-insensitive) → ACEScg.
  - `g22_rec709` (`:505`).
  - `raw` → Raw (`:719-720`).

### 4.2 What Karma does
Help `solaris/kug/textures.txt`, section "Color spaces":
- A color space input of "raw", or a vector type, means no transform.
- In "Auto" mode Karma checks, in order: role strings in the file name, then the file's metadata, then "Assume sRGB for 8-bit images, linear other formats/depths".
- "MtlX Image nodes with a color3 or color4 signature are always in 'auto' mode." To force linear, use a float or vector signature.

Measured on Karma:
- A PNG with RGBA (255,255,255,128) renders color 0.503, i.e. **premultiplied in linear space**. OIIO's default `unassociatedalpha=0` gives the same after decoding (the PNG reads as 0.737, which decodes to 0.502). Keep that default.
- A missing texture returns `default` and logs `Unable to load texture './does_not_exist.exr'`, counted in husk's error summary; rendering continues. `karma:global:abortmissingtexture` (default off) cancels instead.

### 4.3 What reaches the delegate
- USD 26.05 adds `colorSpace:<input>` parameters to `HdMaterialNetwork2` nodes when the attribute has `colorSpace` metadata (`usdImaging/materialParamUtils.cpp:320-326`; read back in `hd/materialNetwork2Interface.cpp:176-181`; `HdMaterialNetworkInterface::NodeParamData::colorSpace` in `hd/materialNetworkInterface.h:69-73`).
- Houdini's `mtlximage` VOP has a `filecolorspace` parameter ("File Color Space"; strings in `$HFS/houdini/config/Dialogs/VOP/mtlximage`).
- Its USD encoding is **not verified**; it is presumably `colorSpace` metadata on `inputs:file`. Check it with a LOP test via the fxhoudini MCP before relying on it.

### 4.4 Deciding the source color space (host side, per texture)
Precedence:
1. An explicit source:
   - the `colorSpace:file` parameter;
   - `UsdUVTexture.sourceColorSpace` raw or sRGB;
   - a MaterialX image of non-color type (float, vector*), which means raw.
2. OCIO file rules, ignoring the default rule.
3. The file's `oiio:ColorSpace` metadata.
4. 8-bit data means sRGB; anything else is linear.

Rule 4 is the Karma rule. It is needed for `.tx` files from `imaketx`, which are 8-bit sRGB without metadata (measured). The existing `sourceColorSpace()` in `hydra/textureCache.cpp:50-68` uses the extension instead; extend it with the bit-depth rule and share it as `hydra/colorSpace.{h,cpp}`.

### 4.5 Applying the conversion
- **Linear to linear:** extract a 3×3 matrix from the OCIO processor and verify linearity (`probe5.cpp`). Matrices commute with filtering, so applying them after the lookup is exact. Measured:
  - ACEScg → scene_linear: `[1.7051 −0.6218 −0.0833; −0.1303 1.1408 −0.0105; −0.0240 −0.1290 1.1530]`.
  - ACES2065-1, Linear P3-D65 and Linear Rec.2020 are matrices as well.
  - sRGB, g22 and ACEScct are detected as non-linear (`isColorSpaceLinear=0`, and the linearity check fails).
- **Non-linear encodings:** convert the file to half float in scene_linear (§2). Filtering in encoded space and then decoding is biased:
  - A 50 % black/white sRGB checker at a coarse footprint gives srgb→lin(0.5) = 0.214 instead of 0.5 (−57 %).
  - Bilinear interpolation between texels shows the same, smaller bias.
  - The half-float conversion costs 2× the memory of uint8 data. The only correct alternative, `forcefloat` + `colortransformid`, costs 4×.
- **Raw data** (normal maps, roughness): no transform, never converted for color. Untiled raw files are still converted for tiling and MIPs, with in = out color space. After filtering normals, renormalize them (the MaterialX normalmap node does).

---

## 5. GPU path (M10)
- **Karma XPU** (`solaris/karma_xpu.txt:332-349`):
  - Shader textures are loaded on demand in tiles (e.g. 128²), with eviction when the GPU is full.
  - Lighting textures are not tiled (capped by `KARMA_XPU_MAX_LIGHTING_TEXTURE_RES`, default 8192).
  - UDIMs are limited to a 10×10 range.
  - OptiX uses hardware linear filtering. Hardware bilinear weights have only 8 fractional bits, hence artifacts with normal maps; the workarounds are cubic filtering or `KARMA_XPU_OPTIX_HQ_LINEAR_FILTERING`.
- **NVIDIA OptiX Toolkit DemandLoading** (BSD) provides exactly this. Its `template <class Sample> Sample tex2DGrad(const DeviceContext&, unsigned textureId, float x, float y, float2 ddx, float2 ddy, bool* isResident, float2 texelJitter)` (`DemandLoading/include/OptiXToolkit/DemandLoading/Texture2D.h:274-275`) takes the same (s, t, ddx=(dsdx,dtdx), ddy=(dsdy,dtdy)) as the OIIO call above. It handles UDIM coordinates (`separateUdimCoord`, `:38`), returns `isResident=false` for tiles not yet loaded, and offers texel jitter (stochastic filtering).
- **CUDA constraints:**
  - No 3-channel arrays, so pad to RGBA half (4/3 memory).
  - `cudaTextureDesc::sRGB` decodes only uint8 sRGB before filtering.
  - BC6H/BC7 compression is optional later.

What this means for the CPU design now:
1. Kernels refer to textures only by a `uint32_t` id plus a POD sampler descriptor; no OIIO types in `core/`.
2. The lookup inputs are (s, t, two full-diameter derivative vectors, rnd), and blur is expressed as derivative padding, so both backends implement it.
3. Color is baked into the texture files, plus an optional 3×3 matrix after the lookup. No OCIO in the lookup path.
4. Nova defines the UDIM semantics itself (tile = 1001 + ⌊u⌋ + 10⌊v⌋, with 0 ≤ u < 10 and v ≥ 0; otherwise `default`) instead of inheriting OIIO's `int()` behavior.
5. Every lookup returns a per-lane status (ok, or missing/non-resident), and the fallback path is shared.
6. At most 4 channels per lookup.
7. Lookups happen only in the Shade stage. Opacity textures inside Embree any-hit callbacks are a separate, later case.
8. The converted EXR files are what a GPU demand loader reads directly, so CPU and GPU see the same data. Only the filter kernels differ (OIIO aniso/bicubic vs. hardware aniso trilinear); add an "XPU-parity" option that uses Bilinear + Trilinear on the CPU.

---

## 6. Design for Nova

### 6.1 Modules
- `core/texture/texture.h`: PODs and `ITextureSampler` (no OIIO).
- `core/texture/ray_cone.h`: §3.2.
- `core/texture/memory_sampler.{h,cpp}`: in-memory bilinear + mip reference implementation over `nova::Image`, for Catch2 tests and analytic checks.
- `hydra/textureSystem.{h,cpp}`: the OIIO implementation and the registry (owned by `HdNovaRenderParam`, next to `HdNovaTextureCache`).
- `hydra/textureConvert.{h,cpp}`: the auto-tx conversion via `make_texture`, and `.rat` via `imaketx`.
- `hydra/colorSpace.{h,cpp}`: shared with `textureCache.cpp`.
- `core/scene`: `Scene` holds `std::vector<TextureInfo>`, a matrix table and a non-owning `const ITextureSampler*`, set by the host at commit.

```cpp
// core/texture/texture.h
namespace nova {
using TextureId = uint32_t;
enum class TexWrap : uint8_t { Periodic, Clamp, Mirror, Black, Constant, File }; // Constant: MaterialX `default` outside [0,1]
enum class TexFilter : uint8_t { Closest, Linear, Cubic };
enum TexFlags : uint32_t { kTexMissing = 1, kTexUdim = 2 };

struct TexSampler {            // per texture instruction (bytecode constant pool)
    TextureId texture;
    TexWrap wrapS, wrapT; TexFilter filter;
    uint8_t channels;          // 1..4
    uint8_t firstChannel;      // MaterialX `layer` resolved on the host
    uint8_t subimage;          // multi-part EXR layer
    uint16_t colorMatrix;      // 0 = identity, else index into the scene's 3x3 table
    float width;               // karma_width x global scale (derivative multiplier)
    float blur;                // karma_blur + global blur (fraction of [0,1] uv)
    float fallback[4];         // MaterialX `default` / UsdUVTexture `fallback`
};
struct TextureInfo { uint32_t flags; float average[4]; };

struct TexQuery {              // one texture instruction over the active lanes (SoA)
    uint32_t count;
    const float *s, *t;                       // MaterialX/USD uv: v up, UDIM rows above 1
    const float *dsdx, *dtdx, *dsdy, *dtdy;   // full-diameter footprint derivatives
    const float *rnd;                         // stochastic filtering sample, or nullptr
};

class ITextureSampler {        // one virtual call per batch, like IAccel
public:
    virtual ~ITextureSampler() = default;
    // out[c * count + i]; status[i]: 0 = ok, 1 = fallback written (missing tile/file, read error)
    virtual void sample2D(const TexSampler &, const TexQuery &, float *out, uint8_t *status) const = 0;
};
} // namespace nova
```

OIIO implementation (hydra):
```cpp
HdNovaTextureSystem::HdNovaTextureSystem() {
    _ic = OIIO::ImageCache::create(true);              // husk --oiio-* options apply
    _ts = OIIO::TextureSystem::create(false, _ic);
    _ts->attribute("flip_t", 1);                       // USD/MaterialX v-up, also for UDIM rows
    _ts->attribute("gray_to_rgb", 1);
    _ts->attribute("stochastic", 3);                   // MIP + aniso by TextureOpt::rnd
}
HdNovaTextureSystem::~HdNovaTextureSystem() { OIIO::TextureSystem::destroy(_ts); OIIO::ImageCache::destroy(_ic); }

void HdNovaTextureSystem::sample2D(const nova::TexSampler &smp, const nova::TexQuery &q, float *out, uint8_t *st) const {
    const Entry &e = _entries[smp.texture];
    auto fallback = [&](uint32_t i) { for (int c = 0; c < smp.channels; ++c) out[c * q.count + i] = smp.fallback[c]; st[i] = 1; };
    if (!e.handle) { for (uint32_t i = 0; i < q.count; ++i) fallback(i); return; }
    OIIO::TextureSystem::Perthread *pt = _ts->get_perthread_info();   // thread-specific lookup once per batch
    OIIO::TextureOpt opt;                                             // per batch: OIIO writes to it
    opt.swrap = toOiio(smp.wrapS); opt.twrap = toOiio(smp.wrapT);     // UDIM: Clamp; Constant: Clamp + test below
    opt.interpmode = toOiio(smp.filter); opt.anisotropic = 8;
    opt.firstchannel = smp.firstChannel; opt.subimage = smp.subimage;
    opt.sblur = opt.tblur = smp.blur;
    if (e.udim) opt.missingcolor = smp.fallback;                      // sparse tiles: silent, like Karma
    float r[4]; uint32_t failed = 0;
    for (uint32_t i = 0; i < q.count; ++i) {
        const float s = q.s[i], t = q.t[i];
        if ((e.udim && (s < 0.0f || t < 0.0f || s >= 10.0f)) ||
            (smp.wrapS == nova::TexWrap::Constant && (s < 0.0f || s > 1.0f)) ||
            (smp.wrapT == nova::TexWrap::Constant && (t < 0.0f || t > 1.0f))) { fallback(i); continue; }
        opt.rnd = q.rnd ? q.rnd[i] : -1.0f;
        const float k = smp.width;
        if (!_ts->texture(e.handle, pt, opt, s, t, k * q.dsdx[i], k * q.dtdx[i], k * q.dsdy[i], k * q.dtdy[i],
                          smp.channels, r)) { ++failed; fallback(i); continue; }
        applyMatrix(smp.colorMatrix, r, smp.channels);
        for (int c = 0; c < smp.channels; ++c) out[c * q.count + i] = r[c];
        st[i] = 0;
    }
    if (failed) e.reportFailures(failed, _ts->geterror());           // one atomic add per batch, first message kept
}
```

### 6.2 Registration (Hydra)
1. `HdNovaMaterial::Sync` translates image nodes into a `TextureRequest {resolvedPath (may contain <UDIM>), colorSpaceHint (explicit / auto / raw), channel type, layer}`.
2. The registry deduplicates by (path, chosen color space, mtime). Material syncs run in parallel, so it holds a mutex, but conversions run outside it (TBB `task_group`).
3. Per request it determines the color space (§4.4), queries `good`, `imagespec` (tiled? miplevels, format, channels) and `inventory_udim`, and picks a policy:
   - `Direct`: linear, tiled and mipped.
   - `Matrix`: the same, but a linear gamut different from scene_linear.
   - `Convert`: untiled, unmipped, or non-linear color → half, linear, tiled 64, mipped EXR. Options: `maketx:fixnan=box3`, `maketx:constant_color_detect=1`, `maketx:opaque_detect=1`, `compression=zip`, explicit `oiio:ColorSpace=lin_rec709`.
   - `ConvertRat`: `imaketx`.
   - `Missing`.
4. Conversion cache:
   - Default location `$HOUDINI_TEMP_DIR/nova_tx/` (option `nova:textureCacheDir`, plus "use an existing `<file>.tx`/`.exr` next to the source if newer").
   - Key: SHA-1 of (canonical path, size, mtime, source color space, cache format version).
   - Write to a temporary name and rename atomically (several husk processes may run at once).
   - For UDIM, convert each tile under one unique prefix, so the converted `<prefix>.<UDIM>.exr` inventories correctly.
   - Packaged `usdz[...]` assets: extract them through `ArResolver::OpenAsset` into the cache dir, then treat them as files.
5. On commit, the core receives `TextureInfo` plus matrices. The OIIO handles stay on the host, indexed by `TextureId`.
6. IPR: when a material re-syncs, compare mtimes. Call `ts->invalidate(path, /*force=*/false)` and reconvert when the source changed. Handles stay valid.
7. Statistics: per texture, count lookups and failures (one add per batch). At the end of a render (`nova:textureStats ≥ 1`), print `ts->getstats(level)` and the IC's `stat:cache_memory_used`, `stat:bytes_read` and `stat:fileio_time` to stderr. List textures that needed conversion and those that never used a level other than 0 (`stat:mipsused`).
8. Errors:
   - A missing file gives one warning on stderr via `HD_NOVA_WARN`, since husk doesn't print Tf diagnostics, and returns `default`.
   - `nova:abortOnMissingTexture` mirrors `karma:global:abortmissingtexture`.
   - `nova:missingTextureColor` is a debug override (e.g. magenta).
   - Read errors during the render are reported once per texture after the pass.
9. Settings:
   - `nova:textureFilterWidth` (0.25; Karma: `karma:global:mtlx_imagewidth` multiplies the per-node `karma_width`).
   - `nova:textureBlur` (0; Karma: `karma:global:mtlx_imageblur`).
   - `nova:textureAutoTx` (1), `nova:textureCacheDir`, `nova:abortOnMissingTexture`, `nova:textureStats`.
   - Honor `karma_width` and `karma_blur` on `mtlximage` for compatibility.

Input mapping (stdlib defaults from `$HFS/bin/usd_plugins/usdMtlx/resources/libraries/stdlib/stdlib_defs.mtlx:139-151`):
- MaterialX `image`:
  - `uaddressmode`/`vaddressmode`: `periodic` (default) / `clamp` / `mirror` / `constant`.
  - `filtertype`: `linear` (default) / `closest` / `cubic` → OIIO `InterpBilinear` / `InterpClosest` / `InterpBicubic`, MIP mode Default.
  - `default` is black by default.
  - `framerange`/`frameoffset` tokens are not supported by UsdMaterialX (Houdini help).
- `UsdUVTexture`: `wrapS`/`wrapT` black, clamp, repeat, mirror, or `useMetadata` (→ `WrapDefault`, which falls back to black). Also `fallback`, `scale`/`bias` (applied after the lookup), and `sourceColorSpace`.
- UDIM sets always use Clamp inside a tile, to avoid seams from periodic wrapping.

---

## 7. Pitfalls
1. `colortransformid` is ignored for untiled images and automip levels, and quantizes uint8 tiles (§1.8).
2. **8-bit data converted to linear 8-bit bands in the darks** (0.003922 instead of 0.003035). Convert to half float.
3. OIIO's UDIM index uses `int(s)`, so u ∈ (−1, 0) reads tile 1001. Karma returns `default`; guard for it in Nova.
4. Missing UDIM tiles fail silently (ok=0, no error text). Use `missingcolor` for UDIM sets only, because it also hides real errors.
5. `texture()` writes to the caller's `TextureOpt` (wrap modes, subimage). Never share one across threads, and don't reuse it across textures with `WrapDefault`.
6. maketx files carry `wrapmodes black,black`, so `WrapDefault` turns black outside [0,1]. Always pass explicit wrap modes.
7. `gray_to_rgb` defaults to 0, so a grey texture read as RGB returns (g, fill, fill).
8. `make_texture` copies `oiio:ColorSpace` from the input; set it after converting, or later readers decode twice.
9. Houdini's config maps unmatched files (`.tx`, `.rat`, `.hdr`) to ACES2065-1 through the default rule. Only OIIO's `getColorSpaceFromFilepath` skips it; calling the OCIO API directly would not.
10. OIIO cannot read `.rat`. `iconvert` linearizes with gamma 2.2, so use `imaketx`.
11. The batched OIIO API is a scalar loop, so batching inside OIIO gains nothing. Coherence comes from sorting shading by material.
12. Applying the sRGB decode after the lookup is biased: up to −57 % on high-contrast textures at coarse MIP levels.
13. For texture-driven emission or lights, NEE and BSDF sampling must use the same footprint widening, otherwise you get seams (Cycles `bsdf.h:59-71`).
14. PNG alpha is premultiplied on read, in linear space. This matches Karma; don't enable `unassociatedalpha`.
15. Anisotropic lookups are about 3× more expensive than isotropic ones. Use `anisotropic=8` plus `stochastic=3`.
16. On the GPU, hardware bilinear has 8-bit weight precision and no RGB format, so pad to RGBA.

---

## 8. Tests

**Unit tests (Catch2, no OIIO):**
- Camera cone: the pinhole spread equals `atan(2·tan(fovY/2)/H)` at the image center, compared against finite-difference rays. Orthographic: width = pixel size, spread = 0.
- Footprint: at normal incidence the derivative in texels is W·(texels per world unit); at angle θ the major axis is W/cosθ and the minor axis W (as in `conemath.cpp`).
- `uvGradients`: for uv an affine map of position, `∇u·e = Δu` along each edge; invariant to triangle vertex order; zero for degenerate uv.
- Curvature: on a sphere mesh of radius R with exact normals, k ≈ 1/R, so a mirror reflection gains spread 2W/(R|cosθ|).
- `MemorySampler`: wrap modes (periodic, clamp, mirror, black, constant) on a 4×1 ramp; MIP mean conservation.

**OIIO level (`hydra` label; expected values measured above):**
- MIP texture built with `maketx:mipimages` (script `karma_mip/scripts/make_data.py`): an orthographic plane at 16 / 64 / 4 texels per pixel must give level 2.00 / 4.0 / ≈0 with `nova:textureFilterWidth=0.25`. That is Karma's result; Nova must match it.
- Wrap: 4×1 ramp with texels 1, 2, 3, 4, closest filtering, at s = −0.1 / 1.1 / 1.6:
  - Clamp: 1, 4, 4.
  - Periodic: 4, 1, 3.
  - Mirror: 1, 4, 2.
  - Black: 0, 0, 0.
  - Constant: `default`, `default`, `default`.
- UDIM (`karma_mip/scripts/make_udim.py`), plane uv u ∈ [−1, 2], v ∈ [0, 2]:
  - u < 0 → default.
  - Tiles 1001 red, 1002 green.
  - 1011: white at v = 1.75, blue at v = 1.25.
  - 1012 missing → default.
  - Exactly what Karma renders.
- Color, constant sRGB PNGs (untiled, and as a uint8 tx): code 10 → 0.003035 ± 1e-5 and code 128 → 0.215861 ± 1e-4. This catches both the 8-bit quantization and the untiled no-op.
- A 50 % checker sRGB PNG at a coarse footprint → 0.5 (filtered in linear), not 0.214.
- ACEScg EXR with the constant c → M·c (matrix above).
- A grey file read as RGB replicates the value; a PNG with RGBA (1, 1, 1, 0.5) → color 0.502.
- Missing file → `default` plus exactly one stderr line; `nova:abortOnMissingTexture` aborts.
- `.rat` input → converted through `imaketx`, value 0.2158.

**husk, cli and reference tests:**
- Run the scenes above with `husk --renderer Nova` and with `nova_cli`.
- Karma comparisons through the existing `tests/reference` harness and `nova_imgcmp`:
  - a grazing-angle checkerboard;
  - a mirror sphere reflecting a texture (specular cone propagation and curvature);
  - a diffuse room with an 8k texture. Its image mean must equal a Nova render that always uses level 0 (filtering preserves the mean). `stat:bytes_read` must be far lower with cones than without.
- Extend `tests/husk/check_textures.py` (currently light textures only) to `mtlximage` on surfaces, covering png, jpg, uint8 tx (rule 4), half EXR and `*ACEScg*` names.

---

## 9. Scratch files (throwaway)
Under `C:/Users/Synti/AppData/Local/Temp/claude/C--Users-Synti-Desktop-renderEngine/ac6add8e-dc52-40d3-838a-501e34988043/scratchpad/m3research/textures/`:
- `oiiotest/test.cpp`: color transforms, MIP selection, UDIM, wrap modes, missing files, timing. Output in `out.txt`.
- `oiiotest/probe2.cpp`: constant colors, grey to RGB, private TS on the shared IC, thread scaling.
- `oiiotest/probe3.cpp`: automip vs. `make_texture` cost.
- `oiiotest/probe4.cpp`: stochastic filtering.
- `oiiotest/probe5.cpp`: OCIO matrix extraction and file rules.
- `oiiotest/conemath.cpp`: footprint math check.
- `oiiotest/build.bat`: MSVC build against `$HFS`.
- `husk_probe/probe.py`: reads husk's shared ImageCache settings.
- `karma_mip/`: Karma MIP-level, UDIM, missing-file and alpha scenes with their EXR results.
- `oiio_src/`: OIIO v2.5.18.0 sources. `refs/`: pbrt-v4, Cycles, Falcor and OTK sources and the JCGT ray-cones paper.

Web sources:
- [JCGT 2021, Improved Shader and Texture LOD Using Ray Cones](https://jcgt.org/published/0010/01/01/)
- [Arnold for Houdini: Textures](https://help.autodesk.com/cloudhelp/ENU/AR-Houdini/files/arnold_for_houdini_ah_Textures_html.html)
- [Arnold answers: why use tx](https://answers.arnoldrenderer.com/questions/164/why-should-i-use-tx-textures-with-arnold.html)
- [Autodesk forum: Arnold texture sampling](https://forums.autodesk.com/t5/arnold-general-rendering-forum/for-better-arnold-chapter-1-50-bad-texture-sampling/m-p/11047820)