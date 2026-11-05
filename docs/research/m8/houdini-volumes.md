# M8 research: how volumes reach Nova in Houdini, and Karma's volume conventions (Houdini 22.0.466)

Scope: docs/PLAN.md M8, "Volumen: NanoVDB aus Houdini-VDB-Prims, heterogen, Emission/Blackbody, Volume-Light-Sampling | Pyro-Shots renderbar". The rendering algorithms (tracking, majorants, volume light sampling) are covered by a separate report. This one answers three questions:

1. How volume data reaches a third-party Hydra delegate, in the Solaris viewport and in husk. This covers USD as Houdini's LOPs author it, what Hydra hands the delegate, how in-memory SOP volumes can be read, and what OpenVDB/NanoVDB Houdini ships.
2. Karma's volume conventions: the default volume shading, the Karma Volume / Pyro Material / MaterialX volume nodes, emission and blackbody, and the render settings with their defaults. Karma CPU reference renders back these up.
3. A recommended data path for M8, what Nova must implement for Karma parity, and test scenes.

Sources:
- the Houdini install (`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`, `$I` = `$HFS/toolkit/include`);
- the help archives in `$HFS/houdini/help` (extracted to a scratch directory, described in my own words, not copied);
- `$HFS/houdini/RenderPropertiesKA.json` and `$HFS/houdini/karmaShaderNodes.json` (property and node registries);
- a strings/exports scan of `BRAY_HdKarma.dll` and `USD_SopVol.dll`;
- probes run in isolated processes.

Every Houdini process ran isolated: hython through `tests/houdini/run_hython.ps1`, husk through `docs/research/m6/probes/husk_env.ps1`. Each had its own preference directory, no user packages and the installed Nova package skipped. The user's Houdini session was not touched. The staged Nova package came from `build/vs2022/stage/Release/nova`, read only.

Probes are in `probes/` (section 7). They contain no SideFX code. The Karma Pyro Material is built at run time from Houdini's own tool recipe, and nothing generated from it is committed.

Each claim is tagged with its source: **(measured)** for renders and timings, **(probe)** for hython or Hydra-probe output, **(USD)** for authored USD dumped by a probe, **(header)** with file:line, **(help)** with page, **(registry)** for the JSON registries, **(strings)** for binary scans.

The work started in an earlier session (2026-10-14) that was cut off before it could write a report. All of its probes were re-run for this report on 2026-11-03.

---

## 0. Findings in short

1. **Houdini authors three kinds of field prims, and only one of them is a plain file reference.**
   - The **Volume LOP** authors `OpenVDBAsset` fields (for `.vdb`) or `HoudiniFieldAsset` fields (for `.bgeo.sc`) with an absolute `filePath`, `fieldName` and `fieldIndex`. It authors no extent, no `fieldDataType` and no `fieldClass` (USD).
   - **SOP Import / SOP Create** of in-memory SOP volumes author `filePath = @op:/obj/pyro/OUT.sop.volumes:SDF_FORMAT_ARGS:...&t=0.625@`. This is a path into a live SOP cook, not a file. The field prims also carry extents, xformOps and the SOP primitive attributes as primvars, and their values are time-sampled when the SOP is time dependent (USD).
   - **A Pyro Solver SOP outputs Houdini native volumes by default**: `conv_vdb` = 0, measured as 6 `Volume` prims. They become `HoudiniFieldAsset` fields. Only after a Convert VDB do they become `OpenVDBAsset` (probe).
2. **Outside the Houdini process, the op: paths turn into files, but native volumes stay `.bgeo.sc`.**
   - The **USD ROP** writes `<layer>.usd.volumes/<n>.bgeo.sc` (native) or `<n>.vdb` (VDB) next to each saved SOP layer, and rewrites `filePath` and `fieldIndex` (measured).
   - The **USD Render ROP** does the same into `$HOUDINI_TEMP_DIR/usd_renders/usdrender_<pid>_<n>_<n>/` (husk saw `./0.bgeo.sc`) and deletes the files after the render (measured).
   - So a VDB-only reader cannot render the most common pyro path: an SOP sim through SOP Import, then husk.
3. **What Hydra hands the delegate (probe delegate `hdVolumeProbe`, in-process and in husk).**
   - An `HdVolume` rprim of type `volume` with field descriptors (binding name, field id, prim type).
   - `HdField` bprims of type `openvdbAsset`, `houdiniFieldAsset` or `field3dAsset`. `sceneDelegate->Get()` returns `filePath` (SdfAssetPath; op: paths unresolved), `fieldName`, `fieldIndex` and, for SOP-imported VDBs, `fieldDataType = float` and `fieldClass = fogVolume`.
   - Volume LOP volumes report an **empty extent**, so the delegate must compute bounds from the grids.
   - Materials arrive with terminal **`volume`**, for both `outputs:kma:volume` and `outputs:mtlx:volume`.
   - Karma settings arrive as render settings (`karma:object:volumelimit` and others) and as constant primvars from Render Geometry Settings (probe).
4. **Nova gets nothing today (measured).** With the staged Nova and `TF_DEBUG=HD_*PRIM_ADDED`, husk added no volume rprim and no field bprim, only the material sprim. Nova then warned that the Pyro material has "no surface terminal". The cause: Nova declares only `mesh`/`sphere` rprims and `renderBuffer` bprims.
5. **Karma reads all three sources through one resolver, and so can Nova.**
   - `BRAY_HdKarma.dll` imports `SOPgetVDBVolumePrimitiveWithIndex` and `SOPgetHoudiniVolumePrimitive` (strings). These are C exports of `$HFS/houdini/dso/USD_SopVol.dll`, a 15 KB wrapper around `HUSD_HydraField::getVolumePrimitive` in `libHUSD`, which is not in the public headers.
   - They take (path, field name, index) and return a `GT_PrimVDB` or `GT_PrimVolume`. These have public headers (`$I/GT/GT_PrimVDB.h:88` `getGrid()`, `GT_PrimVolume` with the `GEO_PrimVolume`).
   - A probe helper DLL (`hv_volume_helper.dll`, HDK-linked, loaded with `LoadLibrary` by an HDK-free probe delegate, the pattern of Nova's `nova_cvex.dll`) resolved **every** case and converted it to NanoVDB. The cases were in-memory op: VDB and native volumes in hython, the same inside a UsdImagingGL engine (as in the Solaris viewport), and `.vdb`/`.bgeo.sc` files inside husk.
   - Lookups took 0.02-0.05 ms (op:) and 0.3-3.4 ms (files). Conversion to NanoVDB took 2.5-7 ms per grid for an 80x128x80 sim (probe).
6. **OpenVDB 13.0.0 (ABI 13, namespace `v13_0_sesi`) and header-only NanoVDB 32.6.0 ship with Houdini (header).**
   - `openvdb_sesi.lib/.dll` depends only on `tbb12`, `blosc`, `zlib1` and the CRT (imports). It is available as the CMake target `Houdini::Dep::openvdb_sesi`.
   - An HDK-free exe linking only `openvdb_sesi` read `hv_pyro.vdb`, converted its 3 grids with `nanovdb::createNanoGrid` (1-1.3 ms each), wrote a `.nvdb` and read it back with NanoVDB alone, with 0 mismatches (measured).
   - NanoVDB on its own reads only `.nvdb`, never `.vdb` (header `nanovdb/util/IO.h`).
7. **Karma's default volume (no material) scatters white light isotropically, with σs = σt = density and no emission (measured).**
   - The alpha of a unit cube of density 1 is 0.6321 = 1 - e^-1. The same holds for no material, the Pyro Material defaults and an ND_volume/anisotropic_vdf MaterialX material.
   - In a white furnace (dome radiance 1) with volume limit 64, it reached 0.634 against the albedo-1 limit 1 - e^-1 = 0.632.
   - Temperature and flame fields do not emit without a material.
8. **The volume shading units are standard (measured).**
   - `scattering` and `absorption` are coefficients per unit length.
   - `emission` adds radiance per unit length and is **not** multiplied by density.
   - `anisotropy` is Henyey-Greenstein with positive values meaning forward scattering: single scattering at 90 degrees with g = 0.5 matched the analytic HG value. Iso and g = 0.5 were both 1.3 % above analytic, with the same offset for all three materials.
   - Fields are sampled trilinearly with voxel-centred samples and value 0 outside: emission integrals across the cube edges matched to 4 digits.
9. **Karma's `ND_blackbody` is power-scaled, not the MaterialX reference (measured).**
   - The colour has the chromaticity of MaterialX's `mx_blackbody` (within 0.002 from 2000 to 10000 K), but its luminance grows about like T^4: about **1e-6 · σT⁴/π** (0.79-0.996 times that from 800 to 10000 K).
   - The luminance is 0.69 at 2500 K and 11.1 at 5000 K, where MaterialX gives 1.0.
   - Below about 1700 K Karma's colour is redder than the MaterialX fit.
   - Nova's current `blackbody` lowering (`hydra/material/translator.cpp:1394`) is the luminance-1 MaterialX version, so Pyro fire in blackbody mode would be 0.69x to 11x off.
10. **The Karma Pyro Material is a plain MaterialX network.**
    - The material (recipe `sidefx::recipe::vop::kma_pyromaterial`) arrives in Hydra as **148 nodes** ending in a native `kma_volume`. The nodes are standard MaterialX ones, 10 `kma_ramp_*`, Houdini-library nodegraphs (`ND_hmtlxcomplementcolor_vector3`), `ND_geompropvalue_*` reading the fields `density`, `temperature`, `Cd` and `Alpha`, two `ND_blackbody`, and `ND_dot` nodes with notes `ray:isshadowray` and `ray:level` (USD, probe).
    - Only smoke is on by default: `enablesmoke` 1, `enablefire` 0, `enablescatter` 0.
    - Fire reads `temperature` for both intensity and colour. It maps colour through a ramp from black to (1, 0.327, 0.1) by default, or through blackbody with `firetempscale` 2500 K per unit.
11. **Karma's volume defaults (registry, help, LOP probe).**
    - `karma:object:volumelimit` is **0**. At 0, camera rays still get direct lighting (single scattering) and emission, but there are no volume bounces and no indirect light. A furnace measured 0.388 at limit 0 against 0.634 at limit 64.
    - The other defaults: `volumesteprate` 0.25, `volumeshadowsteprate` 1, `volumequality` 1, `volumesamplingchannel` "density", `volumefilter` "box" with width 1, `screendoorlimit` 4, and `treat_as_lightsource` "No".
    - The Pyro "Create Render Stage" setup sets `treat_as_lightsource` to 1 when the sim has emission.
12. **Recommendation, in short (section 5).**
    - Build an HDK helper `nova_volume.dll` modelled on `nova_cvex.dll`. It resolves every field through Houdini's resolver and returns NanoVDB buffers through an HDK-free C ABI, so hdNova and core stay HDK-free and GPU-ready.
    - Implement `volume` / `openvdbAsset` / `houdiniFieldAsset` in Hydra, the `kma_volume` and MaterialX volume closures, field-reading `geompropvalue`, Karma's blackbody, and `ray:isshadowray` on transmittance rays.
    - Test against Karma with the calibration cubes and the pyro scenes in section 4.

---

## 1. What Houdini's LOPs author

### 1.1 Test scene (`probes/hv_build_scene.py`)

An isolated hython builds a SOP pyro sim (16 frames, voxel size 0.05) and saves `hv_scene.hip` plus `pyro.vdb` (the converted VDB grids density, temperature and flame; copied to `probes/hv_pyro.vdb`, 585 KB, fp16), `pyro.bgeo.sc` and `native.bgeo.sc`.

The sim is a sphere source, Volume Rasterize Attributes and a Pyro Solver SOP sourcing density, temperature, flame (from `burn`) and vel.

It also builds these LOP networks:

| Prim path | LOP | Source |
|---|---|---|
| `/vdbfile` | Volume LOP | `pyro.vdb` |
| `/bgeofile` | Volume LOP | `native.bgeo.sc` (native volumes density 40³, temperature 20³) |
| `/sop_pyro` | SOP Import | live Pyro Solver output (native) |
| `/sop_pyro_vdb` | SOP Import | the same after Convert VDB (fog VDB, fp16) |
| `/sop_cached` | SOP Import | a File SOP reading `pyro.bgeo.sc` |
| `/sop_native` | SOP Import | Volume SOPs (native) |
| `/sop_vdbsops` | SOP Import | VDB from Polygons (fog) |
| `/sopcreate` | SOP Create | VDB from Polygons inside the LOP |
| `/stage/pyro_stage` | Pyro "Create Render Stage" emulation | SOP Import + Render Geometry Settings + material library with the Karma Pyro Material recipe |

The last row uses the UI-free functions of `dopsparsepyrotools`.

The Pyro Solver's output at frame 16 is 6 **`Volume`** prims (`density`, `temperature`, `flame`, `vel.x`, `vel.y`, `vel.z`, each 80x128x80), and `conv_vdb` defaults to 0 (probe). The solver's render bindings default to `bind_density` "density", `bind_flame` "flame" and `bind_temperature` "flame" (probe).

`dopsparsepyrotools.linkMaterialToSolver` raised "Invalid parameter name" in headless 22.0.466, so the material keeps the recipe defaults (probe).

### 1.2 Authored prims (`probes/hv_dump_stage.py`, full dump `probes/hv_out_authored_usd.txt`)

All volumes are `Volume` prims with one `field:<name>` relationship per field. The field prims are their children (USD).

| | Volume LOP `.vdb` | Volume LOP `.bgeo.sc` | SOP Import / SOP Create, VDB | SOP Import, native volume |
|---|---|---|---|---|
| field prim type | `OpenVDBAsset` | `HoudiniFieldAsset` | `OpenVDBAsset` | `HoudiniFieldAsset` |
| `filePath` | absolute file path | absolute file path | `op:<sop>.sop.volumes:SDF_FORMAT_ARGS:...` | the same |
| `fieldName` | grid name | volume name | grid name | volume name, unsanitized (`vel.x`) |
| `fieldIndex` | 0 | primitive number in the file (0, 1) | 0 (index among same-named grids) | primitive number in the SOP detail (`vel.z` = 5) |
| `fieldDataType` / `fieldClass` | not authored | not authored | `float` / `fogVolume` | not authored |
| extent | none (on the volume either) | none | authored on volume and fields | authored on volume and fields |
| xformOps on fields | none | none | translate/pivot/orient (rotateXYZ) /scale, identity | the same |
| primvars on fields | none | none | the SOP primitive attributes: `P` (volume centre), `minimum`, `maximum`, `shop_materialpath`, `dopobject`, ... | the same |
| time samples | static | static | one sample per cooked frame if the SOP is time dependent (the sim: yes) | the same |

The field prim **name** and the binding name are sanitized (`vel_x`), while `fieldName` keeps `vel.x` (USD).

`/sop_cached` (a File SOP) still becomes an `op:` path, because SOP Import always imports the cooked SOP, not the file behind it (USD).

The op: path has this shape (USD):

```
op:/obj/pyro/OUT.sop.volumes:SDF_FORMAT_ARGS:authormaterialpath=0&globalauthortimesamples=1&pathprefix=/sop_pyro
   &savepath=<hip dir>/usd/sop_pyro.usd&savepathtimedep=0&setmissingwidths=0.010000&t=0.625
```

- `t` is the cook time in seconds: frame 16 at 24 fps.
- SOP Create paths point into the LOP's own SOP network (`op:/stage/sopcreate_vdb/sopnet/OUT.sop.volumes:...&partitionattribs=shop_materialpath&...`).
- The anonymous SOP layer's `HoudiniLayerInfo` custom data lists the same paths under `HoudiniVolumeFilePaths` (USD).
- `XUSD_Utils.h:281-286` declares `HUSDaddVolumeLockedGeos`, which adds "locked geos" for those listed paths (header).
- `XUSD_LockedGeoRegistry.h:48-54` keeps cooked SOP `GU_Detail`s alive per node path and arguments, so a stage that references them never sees a recooked detail (header).

### 1.3 Pyro render stage (`probes/hv_out_pyro_stage_material.txt`)

The Pyro "Create Render Stage" functions author three things (USD):
- the SOP Import under `/pyro/OUT` (native fields `volume_0/{density,temperature,flame,vel_x,vel_y,vel_z}`);
- a binding to `/materials/karmapyromaterial1`;
- Render Geometry Settings as constant primvars on the volume: `karma:object:treat_as_lightsource` = 1, `lightsource_diffusescale` = 1, `lightsource_specularscale` = 1, `mblur` = true, `vblur` = 1 (velocity blur) and `volumevblurscale` = 1.

The material's outputs are `outputs:kma:volume`, `outputs:aov_fire` and `outputs:aov_scatter`. The last two are Karma AOV nodes with their own RenderVars under `/materials/karmapyromaterial1/Vars` and `/Render/Products/Vars` (USD).

### 1.4 Saving to disk

**USD ROP** (`hv_dump_stage.py save`, `probes/hv_out_usd_rop_save.txt`). Each SOP layer is saved as `<savepath>` (`import_pyro.usd`, `sopcreate_vdb.usd`), and its volumes go into a sibling directory `<layer>.usd.volumes/`. Native volumes become one `<n>.bgeo.sc` per SOP, holding all of its fields; VDBs become `<n>.vdb` (measured).

The saved field prims point there with relative paths (`./import_pyro.usd.volumes/0.bgeo.sc`). `fieldIndex` is renumbered to the order in the saved file: density, which was 0 in memory, became 5. They stay `HoudiniFieldAsset` (measured). Volume LOP paths become relative (`../pyro.vdb`).

**USD Render ROP** (`probes/hv_usdrender_rop.py`, output `probes/hv_out_usdrender_rop.txt`). Rendering `/stage/pyro_stage` plus a camera and Karma Render Settings through a `usdrender` ROP runs husk out of process.

With the probe delegate as renderer, husk's field bprims showed `filePath = ./0.bgeo.sc`, resolved to `$HOUDINI_TEMP_DIR/usd_renders/usdrender_24416_1633_1/0.bgeo.sc`, with renumbered `fieldIndex` values. After the render nothing but the VEX code cache was left in the temp directory (measured).

So in husk every in-memory volume is a plain file, but native volumes stay `.bgeo.sc` `HoudiniFieldAsset`s. Reading them needs Houdini's GEO library or Houdini's resolver.

---

## 2. What Hydra hands the delegate

### 2.1 Prim types and adapters

- `UsdVolImagingTokens` = `field3dAsset` and `openvdbAsset` (`$I/pxr/usdImaging/usdVolImaging/tokens.h`). `HdPrimTypeTokens->volume` is the rprim and terminal token (`$I/pxr/imaging/hd/tokens.h:251,286`) (header).
- `houdiniFieldAsset` comes from Houdini's `HD_HoudiniFieldAssetAdapter`, based on `UsdImagingFieldAdapter`, in the `hdHoudini` plugin (`$HFS/houdini/dso/usd_plugins/hdHoudini/resources/plugInfo.json` → `USD_HD.dll`). The schema is `UsdHoudiniHoudiniFieldAsset`, which inherits `FieldAsset` (`$I/HUSD/UsdHoudini/houdiniFieldAsset.h`).
- Pixar's legacy-prim data source already counts it as a volume field type (`$I/pxr/imaging/hd/dataSourceLegacyPrim.h:30-32,118`) (header).
- Both husk and hython register the adapter: `houdiniFieldAsset` bprims arrived in both (probe).
- `HdField` has the tokens `filePath` and `fieldName` and the dirty bits `DirtyTransform` and `DirtyParams` (`$I/pxr/imaging/hd/field.h:19-21`). The scene-index schema `HdVolumeFieldSchema` has `filePath`, `fieldName`, `fieldIndex`, `fieldDataType` and `vectorDataRoleHint` (`volumeFieldSchema.h:36-41`) (header).
- **A delegate receives these prims only if it lists `volume` in `GetSupportedRprimTypes` and the field types in `GetSupportedBprimTypes`.** Nova lists neither (section 2.5).

### 2.2 Field bprims (`probes/hv_volume_probe/hv_probe_delegate.cpp`, outputs `hv_out_hydra_inprocess_*.txt`, `hv_out_husk_probe.txt`)

`VolProbeField::Sync` logs `sd->Get(id, filePath | fieldName | fieldIndex | fieldDataType | fieldClass)`, the transform and the scene-index prim (probe):

| | Volume LOP `.vdb` | SOP Import VDB | SOP Import native (op:) | after USD ROP / Render ROP |
|---|---|---|---|---|
| prim type | `openvdbAsset` | `openvdbAsset` | `houdiniFieldAsset` | unchanged |
| `filePath` | `SdfAssetPath` with resolved path | asset = resolved = the `op:` string | the same | relative asset, absolute resolved path |
| `fieldName` | TfToken `density` | TfToken | TfToken `vel.x` | unchanged |
| `fieldIndex` | int 0 | int 0 | int primitive number | renumbered (bgeo) |
| `fieldDataType` | empty token | `float` | empty | as authored |
| `fieldClass` | empty token | `fogVolume` | empty (`void`) | as authored |
| transform | identity (the field's world transform) | identity | identity | identity |
| scene-index data sources | `volumeField`, `xform`, `primvars`, ... | plus `geomXformVectors` | plus `primvars` with the SOP attributes | unchanged |

- Fields sync with `DirtyTransform|DirtyParams` (0x3) (probe).
- The transform `Get` returns is the field prim's **world** transform, so it includes the parent `Volume`'s xform. In the Karma calibration scenes the cubes were offset by translating the `Volume` prim, and Karma placed them there (measured, section 4).
- For op: paths the asset path is not resolvable by any file API. Only Houdini's resolver can read it (section 3).

### 2.3 Volume rprim

`VolProbeVolume::Sync` (probe):
- `GetVolumeFieldDescriptors` returns one `{fieldName = binding name (relationship name without "field:"), fieldId, fieldPrimType}` per relationship, for example `'vel_x' -> /pyro/OUT/volume_0/vel_x (houdiniFieldAsset)`.
- `GetMaterialId`, visibility and the transform.
- `GetExtent`: the authored extent for SOP imports. For Volume LOP volumes it is the **empty box** (±3.4e38 inverted), so bounds must come from the grids.
- Constant primvars: every Render Geometry Settings property arrives as `karma:object:*`, for example `karma:object:treat_as_lightsource = 1 (int)`, `volumevblurscale`, `mblur` and `vblur`.

### 2.4 Materials

`GetMaterialResource` returns an `HdMaterialNetworkMap` with the terminal **`volume`** (probe):
- the Pyro Material (`outputs:kma:volume`): 148 nodes and 195 connections, with terminal node `kma_pyroshader1/kma_volume1` (id `kma_volume`);
- a hand-written MaterialX material (`outputs:mtlx:volume` → `ND_volume`): 5 nodes, terminal `ND_volume`.

The probe declared the render contexts `kma`, `mtlx` and `""`, which is Nova's order. The full node-id census is in `probes/hv_out_husk_probe_materials.txt`.

### 2.5 Render settings

- With the Karma Render Settings LOP, the render-delegate settings map (147 entries through the USD Render ROP) includes `karma:global:screendoorlimit = 4`, `karma:global:volumecomponents = volume`, `karma:object:volumelimit = 0` and `karma:object:volumequality = 1`.
- A hand-written RenderSettings prim with `float karma:object:volumelimit = 4` arrived as such (probe).
- Per-object overrides arrive as the constant primvars in 2.3.

### 2.6 Nova today (`probes/hv_out_husk_nova.txt`)

`husk --renderer HdNovaRendererPlugin` on the ROP-saved scene (Volume LOP, SOP-imported and pyro volumes plus a camera) ran with `TF_DEBUG=HD_RPRIM_ADDED HD_BPRIM_ADDED HD_SPRIM_ADDED`.

It added only `aov_color`, the material, the camera and **husk's automatic headlight** `/husk_headlight/__the_headlight` (the scene had no lights). It added no volume and no field. Nova warned `material /materials/karmapyromaterial1: render context 'kma': no surface terminal` (measured).

`hydra/renderDelegate.cpp:44-62` declares `mesh`, `sphere` and `renderBuffer` only (read).

---

## 3. Reading the data, in memory and on disk

### 3.1 How Karma does it

- `BRAY_HdKarma.dll` contains `SOPgetVDBVolumePrimitiveWithIndex`, `SOPgetHoudiniVolumePrimitive`, `BRAY_HdField::Sync` and the `GT_PrimVDB` / `GT_PrimVolume` types (strings).
- The only other DLL with these names is `$HFS/houdini/dso/USD_SopVol.dll` (14.8 KB). It exports `SOPgetHoudiniVolumePrimitive`, `SOPgetVDBVolumePrimitive` and `SOPgetVDBVolumePrimitiveWithIndex` as C functions and imports `HUSD_HydraField::getVolumePrimitive(const UT_StringRef&, const UT_StringRef&, int, const UT_StringRef&)` from `libHUSD.dll` (exports, imports).
- `HUSD_HydraField` has no header in `$I/HUSD` (header).
- From the disassembly, each export converts its `(const char *path, const char *name, int index)` arguments to `UT_StringRef`s, adds the field prim type token, calls `getVolumePrimitive` and returns the result unchanged as a `GT_Primitive *`. Nothing is reference-counted on the way out, so ownership presumably stays with Houdini's cache (disassembly; not documented).
- The public parts of the machinery are `XUSD_LockedGeoRegistry::getGeometry(nodepath, args)` (`$I/HUSD/XUSD_LockedGeoRegistry.h:54`), `HUSDaddVolumeLockedGeos`, and `GT_PrimVDB::getGrid()` / `GT_PrimVolume` with `GEO_PrimVolume::getVoxelHandle()` / `indexToPos()` (`$I/GEO/GEO_PrimVolume.h:335,449`) (header).

### 3.2 The probe helper (`probes/hv_volume_probe/hv_volume_helper.{h,cpp}`)

`hv_volume_helper.dll` links the HDK (`libGT`, `libGEO`, `libUT`, `libSYS`, `libCE`) and `openvdb_sesi`. At first use it `LoadLibrary`s `USD_SopVol.dll` and `GetProcAddress`es the two exports. It then branches on the result:
- `GT_PRIM_VDB_VOLUME` (29): reads the `openvdb::FloatGrid` and runs `nanovdb::createNanoGrid` on it.
- `GT_PRIM_VOXEL_VOLUME` (28): walks the dense `UT_VoxelArray`, builds an OpenVDB FloatGrid of the non-zero voxels and converts that.

Either way it returns statistics and the index-to-world matrix through a C struct. Voxel centres sit at integer indices (`GT_PrimitiveTypes.h:70-71`) (header, probe).

Results (`probes/hv_out_query_fields.txt`, `hv_out_hydra_inprocess_*.txt`, `hv_out_husk_probe.txt`) (probe):

| Where | Source | Result |
|---|---|---|
| hython, ctypes (`hv_query_fields.py`) | every op: field (native and VDB, SOP Import and SOP Create), `.vdb`, `.bgeo.sc`, the USD ROP's `.volumes/*.bgeo.sc` and `*.vdb`, index -1 | all resolved, values and transforms consistent between sources; missing file → null |
| hython, `UsdImagingGL.Engine(gpuEnabled=False)` with the HDK-free probe delegate (`hv_hydra_inprocess.py`), i.e. Hydra in the cooking process as in the Solaris viewport | `/stage/pyro_stage/OUT` and `/stage/merge_all` | every field bprim resolved from inside `HdField::Sync` |
| husk with the probe delegate on the ROP-saved USD | `.vdb` and `.bgeo.sc` files | all resolved (no Houdini session, no SOPs) |
| husk through the USD Render ROP | temp `./0.bgeo.sc` | resolved |

Statistics (probe):
- Native 80x128x80 density: 819,200 voxels, range [0, 4.063], voxel 0.05, index (0,0,0) at world (-1.975, -1.175, -1.975), the centre of the first cell.
- The same field as VDB: 40,641 active voxels, bbox (18,20,17)-(61,107,61), the same origin.
- NanoVDB (float, zero voxels skipped): 740 KB for density. Velocity components, which are non-zero everywhere, take 2.28 MB each.
- Lookups: op: 0.02-0.05 ms; `.vdb` 1.1-2.4 ms; `.bgeo.sc` 0.3-3.4 ms. A second lookup of the same file took 1.06 ms, so there is no visible cache.
- Conversion: 2.5-3.5 ms per VDB grid, 3.8-7 ms per dense native field (dense scan included).

### 3.3 The options for Nova

| Option | In-memory op: (viewport) | husk (temp/ROP files) | Native volumes | Notes |
|---|---|---|---|---|
| (i) HDK helper DLL calling `SOPget*` (USD_SopVol) | yes (probe) | yes (probe) | yes (probe) | Same resolver and index semantics as Karma. The entry points are undocumented C exports: check them with `GetProcAddress` and pin to the Houdini version. Copy or convert immediately and never keep the `GT_Primitive*`. Serialize calls. |
| (ii) HDK helper using public headers only: parse the op: path into node path + `SDF_FORMAT_ARGS`, call `XUSD_LockedGeoRegistry::getGeometry`, pick `GEO_PrimVDB`/`GEO_PrimVolume` by `fieldIndex`; files via `GU_Detail::load` | plausible (header) | yes via `GU_Detail::load` | yes | Not probed. More code; a fallback if `USD_SopVol` changes. |
| (iii) files only (OpenVDB `openvdb_sesi` in hdNova, or a cache-to-disk rule) | **no** | `.vdb` only | **no** (`.bgeo.sc`) | Fails on the default SOP pyro path in both the viewport and husk (sections 1.4, 2.2). |

### 3.4 OpenVDB / NanoVDB in Houdini 22.0.466

| Item | Value | Source |
|---|---|---|
| OpenVDB headers | `$I/openvdb`, library 13.0.0, `OPENVDB_ABI_VERSION_NUMBER` 13 (`version.h:59-74`), namespace `openvdb::v13_0_sesi` (`version.h:119-121`), `OPENVDB_USE_BLOSC`/`ZLIB`/`DELAYED_LOADING` | header |
| OpenVDB library | `$HFS/custom/houdini/dsolib/openvdb_sesi.lib`, `$HFS/bin/openvdb_sesi.dll`; CMake `Houdini::Dep::openvdb_sesi` (`toolkit/cmake/HoudiniConfig.cmake:17-18`) | header |
| `openvdb_sesi.dll` imports | `tbb12.dll`, `blosc.dll`, `zlib1.dll`, CRT; no HDK library | imports |
| NanoVDB | header-only, `$I/nanovdb`, version 32.6.0 (`NanoVDB.h:133-135`). Includes `util/CreateNanoGrid.h` (OpenVDB adaptor only with `NANOVDB_USE_OPENVDB`, line 43), `util/IO.h` (NanoVDB's own format, "does NOT depend on OpenVDB"), `util/cuda/*` | header |
| Shipped VDB samples | `$HFS/houdini/usd/assets/shaderball/preview_volume.vdb`, `$HFS/houdini/usd/scenes/noise_sampler/cloud.vdb` | files |

`probes/hv_volume_probe/hv_vdb2nano.cpp` is HDK-free. It links `openvdb_sesi` and `tbb`/`blosc`/`z` and compiles with Nova's flags (`/MD`, C++20, `/Zc:__cplusplus`, `NANOVDB_USE_OPENVDB`). On `hv_pyro.vdb` it ran as follows (`probes/hv_out_vdb2nano.txt`) (measured):
- It opened the file (VDB format 13.0/225) and read 3 float fog grids (40,641 / 35,716 / 53,038 active, 0.9 ms each).
- It converted them with `createNanoGrid`: 740 / 747 / 790 KB, 1.0-1.3 ms each.
- It wrote a 2.28 MB `.nvdb` (float, uncompressed, against the 585 KB fp16 VDB) and read it back with `nanovdb::io::readGrids` in 1.5 ms, with 0 mismatching voxels.

So hdNova *could* link `openvdb_sesi` without the HDK. It is present in every Houdini install, uses Houdini's TBB like `Nova::TBB`, and is `/MD`. But that path alone covers only `.vdb` files (3.3 (iii)).

---

## 4. Karma's volume conventions

### 4.1 Settings and defaults

The Karma Render Settings LOP and Render Geometry Settings LOP were probed with `probes/hv_karma_volume_settings.py`, output in `probes/hv_out_karma_settings.txt`. Defaults come from `RenderPropertiesKA.json`. The same property can be set globally (on the RenderSettings prim) or per object (`primvars:karma:object:*`).

| Property | Default | Meaning |
|---|---|---|
| `karma:object:volumelimit` | 0 | Volume bounces. At 0 a volume gets direct lighting and emission but no further volume bounce and no indirect light from other objects; the help suggests raising it for realism. Measured furnace below. |
| `karma:object:volumesteprate` | 0.25 | "Voxels per sample": the march step is the voxel size divided by the rate, so at 0.25 one sample per 4 voxels. |
| `karma:object:volumeshadowsteprate` | 1 | Multiplier of the step rate for shadow rays. |
| `karma:object:volumequality` | 1 | Multiplier on min/max secondary samples for indirect volume. |
| `karma:object:volumesamplingchannel` | "density" | Field used for empty-space culling. Emissive volumes with zero density should use another field. |
| `karma:object:volumefilter` / `volumefilterwidth` | "box" / 1 | Box, gaussian, ... ; width in voxels. |
| `karma:object:volumeuniform` / `volumeuniformsamples` | false / 4 | Render surfaces as uniform volumes; samples ignored when screendoor > 0. |
| `karma:object:volumevblurmode` / `volumevblurscale` / `volumevblurlatticequality` | Lattice Deformation / 1 / 0.1 | Volume velocity blur. |
| `karma:object:treat_as_lightsource` | No (0; 1 Yes, 2 Auto) | "Efficient Emission Sampling": treat emissive objects as lights. |
| `karma:object:lightsource_samplingquality` / `_diffusescale` / `_specularscale` | 1 / 1 / 1 | Emission-as-light controls. |
| `karma:global:screendoorlimit` | 4 | Stochastic transparency samples. |
| `karma:global:volumecomponents` | "volume" | Components counted as volume bounces. |
| Karma XPU | | Supports only `kma_volume`, `kma_pyropreview` and `kma_whitewater` as volume terminals, and no volume filter. |

Sources: the LOP probe and the registry for the defaults, the help (`nodes/lop/rendergeometrysettings`, `karmarendersettings`) for the meanings. The help page `solaris/karma_xpu` is the source for the XPU row.

Karma's own registry (`karmaShaderNodes.json`) defines `kma_volume` with these inputs and defaults: `absorption` (0,0,0), `scattering` (0,0,0), `emission` (0,0,0), `anisotropy` 0, `secondarymix` 0, `secondaryanisotropy` 0, `extinctionmodifier` 0, `contributionmodifier` 1, `modifierstartlevel` 1 (the Pyro Material sets 0) (registry).

The help describes the three modifier inputs as multiple-scattering approximations. From the given number of volume bounces on, the extinction is scaled by a power of the extinction modifier, and the albedo by the contribution modifier (help `nodes/vop/kma_volume`).

### 4.2 Measurements

The calibration scenes (`probes/hv_karma_scenes.py`, `hv_karma_renders.ps1`, `hv_karma_measure.py`; full output in `probes/hv_out_karma_measure.txt`) are unit cubes of a 20³ VDB (voxel 0.05) seen through exactly one unit of depth by an orthographic camera at 80x80 px. Each scene has three cubes:
- **bare**: no material;
- **pyro**: the Karma Pyro Material with recipe defaults;
- **mtlx**: `ND_volume(ND_anisotropic_vdf(scattering = density), ND_uniform_edf(0))`.

Karma CPU ran with path-traced convergence, 64 samples, a box pixel filter and no colour limit. Each render took 1.4-2.0 s (measured).

| Scene | bare | pyro | mtlx | Expected |
|---|---|---|---|---|
| `cal_alpha`: no lights, density 1, alpha | 0.6321 | 0.6321 | 0.6321 | 1 - e^-1 = 0.6321, so **σt = density** |
| `cal_furnace_vl0`: dome radiance 1 (not visible to camera), limit 0 | 0.388 | 0.379 | 0.390 | single scattering only |
| `cal_furnace_vl64`: limit 64 | 0.634 | 0.633 | 0.631 | albedo 1: 1 - e^-1 = 0.632, so **σs = σt** |
| `cal_distant`: normalized distant light, irradiance 1, from above; R mean | 0.03084 | 0.03084 | 0.03084 | iso single scatter E/(4π)·e^-(0.5-y)·(1-e^-1) = 0.03046 (x1.013) |
| `cal_distant_g05`: anisotropy 0.5 | 0.03084 (no input) | 0.01655 | 0.01655 | HG at 90 degrees: 0.01634 (x1.013) |

Emission cubes (`cal_emit_*`) use density 0.001 and temperature 0.5 / 1 / 2. The Pyro Material has fire on, default ranges (0, 1), and the field values `temperature`. The RGB means are below (measured).

| | T = 0.5 | T = 1 | T = 2 | Explanation |
|---|---|---|---|---|
| ramp colour mode (default) | (0.2457, 0.0804, 0.0246) | (0.9829, 0.3214, 0.0983) | (1.0163, 0.3323, 0.1016) | Colour ramp black → (1, 0.327, 0.1) times intensity = clamp(T). The analytic integral with trilinear edges is 0.2458 / 0.9833 / 1.0167 (R). |
| ramp, step rate 1 / 0.05 | same means; std 0 / 0.003 | | | The step rate changes noise, not the mean. |
| fire on, smoke off | identical to the first row | | | Density 0.001 is negligible. |
| no material | 0 | 0 | 0 | A bare volume does not emit. |
| blackbody colour mode | (0.0715, 0.0053, 0) | (1.4277, 0.5311, 0.0963) | (13.535, 10.683, 8.445) | See 4.4. |
| no density field at all (temperature + flame only), with or without `volumesamplingchannel` = temperature | (0.9833, 0.3216, 0.0983), alpha 0 | | | Emission is not culled when density is missing. |

Conclusions:
- Extinction is σt = density per unit length.
- The default and the Pyro-default scattering albedo is 1, with an isotropic phase function normalized to 1/(4π).
- `anisotropy` is Henyey-Greenstein with g > 0 forward.
- Emission is radiance per unit length and is not scaled by density.
- Fields are sampled trilinearly with voxel-centred samples and 0 outside the active region. The edge integrals 0.95 + 2·0.05/3 and 0.95 + 2·(0.025 + 0.025/3) reproduce the measured 0.9829 and 1.0163.
- The 1.3 % excess in single scattering is the same for all phase functions and materials; it was not investigated further.

### 4.3 Karma Pyro Material (`probes/hv_out_karma_scenes.txt`, `hv_out_husk_probe_materials.txt`)

The recipe `sidefx::recipe::vop::kma_pyromaterial` (applied headlessly through `hrecipes.storage`, since `hou.data.applyToolRecipe` needs `hou.ui`) builds a `subnet` material with a `kma_pyroshader` node (probe).

That node is a MaterialX nodegraph: `kma_pyrosmokecolor`, `kma_pyrofireemission` → `kma_pyrofirecolor`, `kma_pyroscatteremission` → `kma_pyroscattercolor`, `kma_pyrovolumemask` and `kma_volume`. Hydra flattens it to 148 shader nodes (probe). The node ids are:
- `ND_switch_color3I` x19, `ND_switch_floatI` x12, `ND_range_float` x12, `ND_geompropvalue_float` x13, `ND_geompropvalue_color3` x2;
- `ND_multiply_*`, `ND_add_*`, `ND_dot_*` (x7), `ND_separate2_vector2` x8, `ND_power_float`, `ND_max_*`, `ND_clamp_*`, `ND_mix_color3`, `ND_luminance_color3`, `ND_colorcorrect_color3`, `ND_ifequal_color3`, `ND_convert_boolean_float`, `ND_combine3_vector3`;
- `ND_hmtlxcomplementcolor_vector3` x2, a Houdini-library nodegraph in `$HFS/houdini/materialx/libraries/houdini/hmtlxcomplementcolor.mtlx` built from standard nodes;
- `kma_ramp_color` x7, `kma_ramp_float` x3;
- `ND_blackbody` x2;
- one `kma_volume`.

Fields read by `geompropvalue` (probe):
- `density`, for smoke, the smoke, absorption and shadow colour sources, and the masks;
- `temperature`, for the density control, fire intensity and fire colour;
- `Cd` and `Alpha`;
- `__scatter__` / `scatter`, for scatter emission.

Ray-type dependence, read from the network and not measured:
- `kma_pyrosmokecolor` switches its absorption output on a `ND_dot` with note `ray:isshadowray`. Shadow colour and shadow intensity act only on shadow (transmittance) rays.
- A `ray:level` read feeds the multiple-scattering modifiers.

Interface defaults that matter (probe; the full list is in `hv_out_karma_scenes.txt`):
- smoke: `enablesmoke` 1, `densityscale` 1, `smokecolor` (1,1,1), `smokeint` 1, `absorpcolor` (1,1,1), `shadowcolor` (1,1,1), `shadowint` 1;
- phase: `anisotropy` 0, `usesecondaryanisotropy` 0, `secondarymix` 0.5, `secondaryanisotropy` 0;
- multiple-scattering modifiers: `extinctionfalloff` 0, `albedocontribution` 1, `startlevel` 0;
- fire: `enablefire` 0, `fireint` ← temperature, `fireintrange` (0,1), `fireintmode` 1 (ramp, linear 0→1), `fireintscale` 1, `firecolor` ← temperature, `firecolormode` 0 (ramp; 1 = blackbody), `firecolorrange` (0,1), `firetempscale` 2500, `fireenablemask` 0;
- scatter: `enablescatter` 0, `scatterint` 1;
- emission: `emission` (0,0,0), added to the total.

The ramp values are on the inner nodegraph: fire colour from (0,0,0) to (1, 0.327, 0.1), linear (USD).

So the fire emission is colour(T) x intensity(T), where:
- T is the `temperature` value;
- intensity = clamp-ranged T, with an optional ramp, times `fireintscale`;
- colour = the ramp of the clamped range, or `ND_blackbody(T · firetempscale)`.

With the defaults the material renders identically to a bare volume: `pyro_mat` = `pyro_bare` = `pyro_mtlx` to 4 digits (measured, 4.5).

Supported MaterialX volume nodes: Karma CPU rendered the `ND_volume` + `ND_anisotropic_vdf` + `ND_uniform_edf` material (with `outputs:mtlx:volume`, no `volumematerial` node) and the `ND_blackbody` → `uniform_edf` material (measured). `ND_absorption_vdf` exists in the library (`pbrlib_defs.mtlx:218`) but was not rendered. The help says Karma supports volume materials only on volume prims, and never surface and volume materials on one prim (help `solaris/materialx`).

### 4.4 Blackbody (`cal_bb_mtlx`, `cal_bb_pyro`)

Eight thin cubes emit `uniform_edf(ND_blackbody(temperature · 2500))` per unit length. The Pyro Material in blackbody mode, with the intensity range shrunk to (0, 0.01), gave **identical** numbers, so its blackbody mode is `ND_blackbody` (measured).

| T [K] | Karma RGB | Rec.709 Y | Y / (1e-6·σT⁴/π) | normalized rgb, Karma | normalized rgb, `mx_blackbody` |
|---|---|---|---|---|---|
| 800 | (0.0284, -0.0002, -0.0003) | 0.0059 | 0.794 | (1.02, -0.01, -0.01) | (0.52, 0.31, 0.17) |
| 1000 | (0.0645, 0.0017, -0.0006) | 0.0149 | 0.824 | (0.98, 0.03, -0.01) | (0.84, 0.16, 0) |
| 1500 | (0.2696, 0.0351, -0.0034) | 0.0822 | 0.900 | (0.895, 0.117, -0.011) | (0.877, 0.123, 0) |
| 2000 | (0.6981, 0.1775, 0.0053) | 0.2758 | 0.955 | (0.7925, 0.2015, 0.006) | (0.7910, 0.2027, 0.0063) |
| 2500 | (1.4342, 0.5323, 0.0963) | 0.6926 | 0.982 | (0.6953, 0.2580, 0.0467) | (0.6935, 0.2592, 0.0473) |
| 4000 | (6.532, 4.244, 2.437) | 4.600 | 0.996 | (0.4944, 0.3212, 0.1844) | (0.4925, 0.3217, 0.1858) |
| 6000 | (24.89, 22.31, 21.64) | 22.81 | 0.975 | (0.3615, 0.3241, 0.3144) | (0.3602, 0.3241, 0.3156) |
| 10000 | (148.2, 168.6, 241.9) | 169.6 | 0.939 | (0.2653, 0.3018, 0.4329) | (0.2646, 0.3016, 0.4338) |

- Karma's `ND_blackbody` returns the MaterialX chromaticity from 2000 K up (within 0.002), and a redder colour below about 1700 K. Its brightness follows roughly the Stefan-Boltzmann law: 1e-6·σT⁴/π W/(m²·sr), times 0.79-0.996.
- The local exponent is 4.2 at 1000-2000 K and 3.93 at 6000-10000 K, which is close to, but not exactly, a Planck integral over a finite band. Its MaterialX reference implementation (`pbrlib/genglsl/mx_blackbody.glsl` in `$HFS`) returns luminance 1 at every temperature.
- The help notes that blackbody mode builds the emitted power into the colour, so the intensity should be reduced, or set to 1 for the pure physical colour (help `nodes/vop/kma_pyrofirecolor`).
- Negative channels are not clamped in the MaterialX path, but are clamped in the Pyro path (the 800-1500 K B values).
- **Nova's `blackbody` (`hydra/material/translator.cpp:1394-1416`) is the luminance-1 version and would be off by 0.69x at 2500 K and 11x at 5000 K.**

### 4.5 Pyro sim renders (`pyro_*`, `probes/hv_karma_pyro_contact.png`)

Setup:
- `hv_pyro.vdb` at 200x300, perspective camera;
- one distant key light (intensity 3, `normalize = 1`, rotate (-30, 50, 0)), no dome, black background;
- Karma defaults (Variance convergence) except where noted.

"Karma" is the render time from the log; "husk" is the whole process (measured).

| Scene | Karma [s] | husk [s] | mean RGB | max Y | Notes |
|---|---|---|---|---|---|
| `pyro_bare` | 1.62 | 2.16 | (0.0138, 0.0138, 0.0138) | 0.166 | |
| `pyro_bare_vl4` | 2.20 | 2.71 | (0.0199, 0.0199, 0.0199) | 0.275 | +44 % from volume bounces |
| `pyro_mat` | 1.76 | 2.29 | = `pyro_bare` | 0.166 | |
| `pyro_mat_vl4` | 2.20 | 2.71 | = `pyro_bare_vl4` | 0.275 | |
| `pyro_mtlx` | 1.51 | 2.04 | = `pyro_bare` | 0.166 | |
| `pyro_fire` | 1.67 | 2.19 | (0.0805, 0.0356, 0.0205) | 0.409 | ramp colour |
| `pyro_fire_bb` | 1.76 | 2.28 | (0.0783, 0.0335, 0.0162) | 0.533 | blackbody |
| `pyro_fire_vl4` | 2.51 | 3.05 | (0.1138, 0.0506, 0.0293) | 0.791 | +41 % |
| `pyro_fire_sr1` | 1.82 | 2.35 | (0.0766, 0.0344, 0.0201) | 0.388 | step rate 1: -4.8 % mean against 0.25, not investigated |

The emission cubes did not change with the step rate, so the difference in `pyro_fire_sr1` is in the lighting path.

Mean alpha was 0.079 everywhere: only the density field gives opacity (measured).

### 4.6 Gotchas met while building the scenes (measured)

- `UsdLuxDistantLight` without `inputs:normalize = 1` treats `intensity` as the radiance of a 0.53-degree disk. That is an irradiance of about 2e-4·intensity, so volumes looked black (max 1e-5). Houdini's Distant Light LOP authors normalize on.
- husk adds a distant headlight when the stage has no lights. Use `--headlight none`, as `tests/reference/tools/refcommon.py` does.
- A USDA layer with two root `over "materials"` specs had no effect in husk, and nothing was printed. Merge the overrides into one spec.
- The aperture of an orthographic USD camera is in tenths of a scene unit (40 = 4 units). The cubes landed exactly where that predicts.

### 4.7 Scattering details for M8.3 (`probes/hv_m83_probe.py`, output `probes/hv_out_m83_probe.txt`)

Added during M8.3 (2026-11-03). One unit cube of density 1 at 40x40 px, 256 path-traced samples, means over the cube's middle; Karma CPU (measured).

- **Light multipliers.** Volume scattering takes the light's `inputs:diffuse` multiplier, never `inputs:specular`: diffuse 0 makes a distant-lit cube and a furnace black, specular 0 changes nothing. So volume events count as diffuse for the multipliers, directly and through continuations.
- **Light path expressions.** Volume scattering is a `V` event with no scattering class and no label: `CVL` = `C<V.>L` = 0.0312 (single scattering), while `C<VD>L`, `C<VG>L`, `C<VS>L`, `CDL`, `CGL`, `C<V.'volume'>L` and `C<..'volume'>L` are 0. `CVVL` and `CV+L` (= the beauty) behave as expected. Volume emission is an `O` event: `CO` + `CV+O` = the beauty of an emitting cube; `CL` is 0.
- **Light-only rays collect no volume emission.** At volume limit 0 an emitting, scattering cube gives `CVO` = 0 (0.135 at limit 4): the continuation past the limit gathers lights only, as for surfaces.
- **A after V.** `CVA` = 0.632 = the alpha of the cube: the event's albedo weighted by the chance of the event.
- **First-hit AOVs over volumes.** Karma treats the sampled volume event as the hit: `ray:hitP`, `ray:hitdist` and `ray:hitN` average over samples between the volume events (hit distance 4.92 mean, N = 0) and the plane behind (N = 1); `C<RD>A` is 0 at volume events; `ray:hitPz` reads 4.65 with and without the plane (an implementation detail of Karma's depth).
- **Secondary lobe.** `secondarymix` is the weight of the second lobe: g = 0.5, g2 = 0, mix 0.25 gives 0.0204 = 0.75·HG(0.5) + 0.25·HG(0) at 90 degrees; swapping the lobes gives 0.0276.
- **`ND_absorption_vdf`** absorbs (alpha 0.632) and does not scatter (lit cube black).
- **Modifiers** (`kma_volume`, scattering = density, furnace = dome of radiance 1 not seen by the camera):

| extinction e | contribution c | start | alpha | limit 0 | limit 1 | limit 2 | limit 64 |
|---|---|---|---|---|---|---|---|
| 0 | 1 | 0 | 0.6323 | 0.3853 | 0.5403 | 0.5980 | 0.6310 |
| 0.5 | 1 | 0 | 0.4547 | 0.3339 | 0.4003 | 0.4083 | 0.4090 |
| 0.5 | 1 | 1 | 0.6323 | 0.3853 | 0.5105 | 0.5338 | 0.5363 |
| 0.5 | 1 | 2 | 0.6323 | 0.3853 | 0.5403 | 0.5869 | 0.5966 |
| 0.25 | 1 | 0 | 0.5411 | 0.3663 | 0.4744 | 0.4964 | 0.5016 |
| 0.9 | 1 | 0 | 0.3338 | 0.2710 | 0.2962 | 0.2974 | 0.2974 |
| 0 | 0.5 | 0 | 0.6323 | 0.1927 | 0.2310 | 0.2383 | 0.2402 |
| 0 | 0.5 | 1 | 0.6323 | 0.3853 | 0.4626 | 0.4773 | 0.4799 |
| 0 | 0.5 | 2 | 0.6323 | 0.3853 | 0.5403 | 0.5696 | 0.5768 |
| 0.5 | 0.5 | 1 | 0.6323 | 0.3853 | 0.4489 | 0.4538 | 0.4546 |

Reading of the table (levels count the scattering events before a ray; camera rays are level 0):
- From the start level on, the extinction is scaled by exp(-e·(level - start + 1)): at start 0 the camera alpha is 1 - exp(-τ·e^-e) for e = 0.25, 0.5 and 0.9 (optical depth 0.779, 0.606, 0.406 against 1.000).
- The contribution modifier multiplies the albedo at every scattering event from the start level on (each order of scattering gains one factor c: 0.50, 0.25, 0.126 of the base orders at start 0).
- A light sample's shadow ray sees the volume at its event's level, and so does the light the event's continuation gathers: at start 1 and limit 0 nothing changes (to five digits), and at limit 1 or more Karma is darker than the base where a continuation that gathered its lights at its own, thinner level would be brighter. Only where the next event happens follows the continuation's own level. Nova reproduces the whole table this way (M8.3, ctest `houdini_volume_karma`).

### 4.8 Emissive volumes as light sources (`probes/hv_m84_probe.py`, output `probes/hv_out_m84_probe.txt`)

Added during M8.4 (2026-11-03). A fire cube (`kma_volume`: absorption = density, emission (2, 1, 0.5)·density) and a white smoke cube on a grey diffuse ground (`standard_surface`, base 0.5, specular 0), no other light, seen from above; red channel means over the ground between the cubes, the smoke and the fire; 256 path-traced samples, Karma CPU (measured).

| treat_as_lightsource | diffuse scale | limit | ground (beauty / LPE) | smoke (beauty / LPE) | fire (beauty / LPE) |
|---|---|---|---|---|---|
| 0 | 1 | 0 | 0.0556 = `CDO` | 0.0015 = `CDO` (`CVO` 0) | 1.594: `CO` 1.388, `CDO` 0.207 |
| 1 | 1 | 0 | 0.0560: `CDL` 0.0554 | 0.0112: `CVL` 0.0081, `CDL` 0.0019 | 1.598: `CL` 1.388, `CDL` 0.210 |
| 0 | 1 | 4 | 0.0561 | 0.0222: `CVO` 0.0092 | 1.595 |
| 1 | 1 | 4 | 0.0568 | 0.0204: `CVL` 0.0081 | 1.597 |
| 1 | 0 | 4 | 0 | 0 | 1.388 = `CL` |
| 1 | 2 | 4 | 0.1135 (x2) | 0.0415 (x2) | 1.808: `CDL` x2 |
| 1 | 1, specular scale 0 | 4 | as specular 1 | as specular 1 | as specular 1 |

- With `treat_as_lightsource` 1 the volume's emission is a light everywhere: an `L` event (also seen from the camera: `CL` instead of `CO`), light-sampled (the smoke is lit at volume limit 0, `CVL`), and gathered by light-only rays.
- `lightsource_diffusescale` multiplies the light on diffuse surfaces and volumes, by light samples and by continuations alike; the camera's direct view keeps the plain emission. `lightsource_specularscale` does not act on these diffuse receivers.
- Karma marches emission with its step rate. At the default 0.25 (5 steps through the unit cube) the fire comes out about 10 % bright (2·(1 - e^-1) = 1.264 exact, a left-sum over 5 steps 1.395, Karma 1.388); at step rate 4 Karma gives 1.458 for the fire region (direct view plus the ground under it) and at 16 1.453, Nova 1.451. Comparisons of absorbing emitters use step rate 4 (ctest `houdini_volume_karma`).
- At volume limit 0 Karma's smoke lit by the fire light is 0.0103 (step rate 4 and 16), Nova's 0.0098 with its light-sampled, phase-sampled and MIS estimates agreeing within noise; at limit 4 both give 0.0193.

---

## 5. Recommendations for M8

### 5.1 Data path

1. **An HDK helper `nova_volume.dll` (proposed `houdini/volume_bridge`), built like `houdini/cvex_bridge`.**
   - It links the Houdini target and `openvdb_sesi`; hdNova `LoadLibrary`s it from next to the module, as with `nova_cvex.dll`.
   - It resolves *every* field `(filePath, fieldName, fieldIndex, prim type)` through `USD_SopVol.dll`'s `SOPgetVDBVolumePrimitiveWithIndex` / `SOPgetHoudiniVolumePrimitive`, the resolver Karma uses.
   - It converts the result to NanoVDB on its side: VDB through `createNanoGrid`; native volumes from the voxel array, skipping exact zeros and keeping the index-to-world with voxel centres at integer indices.
   - It returns the NanoVDB buffer, grid type (float, Vec3f for `vel`/`Cd`) and transform through an HDK-free C header, in the style of `core/shading/external.h`.
   - hdNova and core stay HDK-free. Core consumes NanoVDB (header-only, the same data on the GPU later).
   - This covers the Solaris viewport (op:), husk through the USD Render ROP (temp `.bgeo.sc`/`.vdb`), saved USD (`.volumes/`) and plain Volume LOP files with one code path (probe, 3.2).
2. **Guards.**
   - Resolve the exports with `GetProcAddress` and refuse the helper on a different Houdini build.
   - Copy or convert at once, and never keep the returned `GT_Primitive*`; its ownership is undocumented.
   - Serialize calls.
   - Re-read on `DirtyParams`: SOP recooks change `HoudiniDataId` and the `&t=` in the path.
   - Fallbacks: `.vdb` through `openvdb_sesi` directly, `.nvdb` through NanoVDB IO for tests without Houdini, and, if `USD_SopVol` ever changes, option (ii) of 3.3 (public `XUSD_LockedGeoRegistry`).
3. **Order of work:** start with `.vdb` from the Volume LOP, then `.bgeo.sc` (USD ROP / Render ROP), then op: in the viewport. These are the same helper calls, so "file VDB first" is a test order, not a different design.
4. **Do not require users to cache to VDB.** The default pyro output is native volumes, and Houdini's own save paths keep them as `.bgeo.sc` (1.4).

### 5.2 What Nova must implement for Karma parity

1. **Hydra.**
   - Declare the rprim `volume` and the bprims `openvdbAsset`, `houdiniFieldAsset` and `field3dAsset` (the last one warns).
   - Field `Sync`: read `filePath`, `fieldName` and `fieldIndex`, plus `fieldDataType` when authored.
   - Volume `Sync`: `GetVolumeFieldDescriptors` (binding names such as `vel_x`), the material, constant `karma:object:*` primvars, and the transform.
   - Compute bounds from the grids when the extent is empty (Volume LOP).
   - Grid placement: grid index-to-world, then the field prim's world transform.
2. **Materials.**
   - Accept the terminal `volume` from the `kma` and `mtlx` contexts.
   - Implement `kma_volume`: absorption, scattering, emission, HG anisotropy, the secondary lobe mix, and the extinction/contribution modifiers above the start level.
   - Implement `ND_volume` with `anisotropic_vdf`, `absorption_vdf` and `uniform_edf`.
   - Make `geompropvalue` on a volume sample the named field: trilinear, voxel-centred, 0 outside, default when the field is missing.
   - Evaluate `ray:isshadowray` = 1 on transmittance and shadow rays, and `ray:level` for the modifiers.
   - Check that Nova's nodegraph expansion finds `ND_hmtlxcomplementcolor_vector3` in `$HFS/houdini/materialx/libraries/houdini`.
3. **Default volume (no material).** σs = σt = density, white, isotropic, no emission (4.2).
4. **Blackbody.** Replace the luminance-1 lowering with Karma's power-scaled curve. Generate a LUT of RGB against temperature from Karma with the sweep in `hv_karma_scenes.py` (extend `TB`), say 64 points from 500 to 25,000 K. The analytic model 1e-6·σT⁴/π·rgb_mx(T) is only within ±6 % above 1500 K.
5. **Settings.**
   - `karma:object:volumelimit` (default 0, measured meaning in 4.1) maps onto Nova's per-type limits (M4.1).
   - Read `volumesteprate`, `volumeshadowsteprate` and `volumesamplingchannel` as quality and majorant hints.
   - `treat_as_lightsource` turns an emissive volume into a sampled light (the M8 plan item "Volume-Light-Sampling"; the pyro stage sets it).
   - `volumefilter` "box" / width 1 is trilinear.
6. **Later (motion blur).** `vel` arrives as a Vec3 VDB or as native `vel.x/.y/.z` scalar fields, and `karma:object:volumevblurscale` sits on the prim.

### 5.3 Test scenes

1. **Calibration against Karma.** These are the cube scenes of 4.2, generated by `hv_karma_scenes.py` with tiny VDBs, so they can be committed:
   - alpha/σt;
   - furnace at limit 0 and 64, analytic at the high limit;
   - normalized distant light with g = 0 and 0.5 (analytic);
   - emission ramp (analytic edge integrals);
   - the blackbody sweep (the Karma LUT);
   - emission without a density field.
   All renders take about 2 s in Karma.
2. **Pyro looks.** `hv_pyro.vdb` with the key light: bare, limit 4, fire with ramp, fire with blackbody, fire at limit 4. Karma refs at 200x300 take about 2 s each.
   - The Pyro Material is a SideFX-generated network. Generate it at test time with the recipe, as the probe does, rather than committing it.
   - For unit tests, a hand-written `kma_volume` network is enough.
3. **Plumbing (hython / husk, isolated).**
   - SOP pyro (native) → SOP Import, rendered by Nova in-process (the `hv_hydra_inprocess.py` pattern, as in the viewport).
   - The same through a USD Render ROP (temp `.bgeo.sc`).
   - A USD-ROP-saved stage (`.volumes/`).
   - Volume LOP `.vdb` and `.bgeo.sc`.
   - Convert-VDB SOP Import (`fieldClass` / `fieldDataType` authored).
   - A frame change (new `&t=`).
   - A missing file.
   - Run Nova's `run_viewport_check.ps1` with a pyro step once the viewport path works.

---

## 6. Not measured / open

- The real Solaris viewport. The in-process case was simulated with `UsdImagingGL.Engine(gpuEnabled=False)` in hython.
- Field-level transforms that differ from the volume's. All authored ones were identity.
- `fieldIndex` = -1 against Volume LOP indices for multi-grid files with duplicate names.
- Whether `SOPget*` is thread-safe, and whether repeated calls leak.
- Whether `nova_cli` (outside husk/hython) registers the `hdHoudini` adapter for `HoudiniFieldAsset` prims.
- `ND_absorption_vdf`, the secondary HG lobe and the extinction/contribution modifiers. The help describes them; they were not rendered. (Rendered in M8.3: section 4.7.)
- The exact Karma blackbody function (4.4 gives the measured curve and a model within ±6 %).
- The 1.3 % single-scatter excess, and the 4.8 % step-rate difference in `pyro_fire_sr1`. These are rendering-algorithm topics.
- Karma XPU.

## 7. Probe files (`docs/research/m8/probes/`)

| File | Purpose |
|---|---|
| `hv_build_scene.py` | builds `hv_scene.hip`, `pyro.vdb`, `pyro.bgeo.sc` and `native.bgeo.sc` (1.1) |
| `hv_m83_probe.py`, `hv_out_m83_probe.txt` | the M8.3 probes of 4.7 (light multipliers, LPE events, first-hit AOVs, lobes, `absorption_vdf`, modifiers) |
| `hv_m84_probe.py`, `hv_out_m84_probe.txt` | the M8.4 probe of 4.8 (emissive volumes as light sources) |
| `hv_dump_stage.py` | dumps authored volume/field prims, the pyro material and bindings; `save` writes them with a USD ROP |
| `hv_query_fields.py` | resolves every field through `hv_volume_helper.dll` in hython (ctypes) |
| `hv_hydra_inprocess.py` | drives a Hydra delegate in-process on a cooked LOP stage (UsdImagingGL, no GPU) |
| `hv_usdrender_rop.py` | renders the pyro stage through a USD Render ROP and lists what husk sees |
| `hv_karma_volume_settings.py` | Karma Render Settings / Render Geometry Settings volume parameters, defaults and authored attributes |
| `hv_karma_scenes.py`, `hv_karma_renders.ps1`, `hv_karma_measure.py` | the Karma calibration and pyro scenes, their husk renders and the measurements (section 4) |
| `hv_volume_probe/` | `hdVolumeProbe` (HDK-free logging delegate), `hv_volume_helper` (HDK helper, `SOPget*` → NanoVDB), `hv_vdb2nano` (HDK-free OpenVDB → NanoVDB round trip); stand-alone CMake project |
| `hv_pyro.vdb` | the pyro sim at frame 16 (density, temperature, flame; fp16, 585 KB) |
| `hv_out_*.txt` | probe outputs with scratch paths replaced: authored USD, ROP saves, helper queries, Hydra in-process and husk logs, Nova husk log, Karma settings, Karma measurements, pyro material interface |
| `hv_karma_pyro_contact.png` | Karma renders `pyro_bare`, `pyro_bare_vl4`, `pyro_fire`, `pyro_fire_bb`, `pyro_fire_vl4`, `pyro_mtlx` (sRGB) |

Rebuilding the probe project: run `cmake -S docs/research/m8/probes/hv_volume_probe -B <scratch> -G "Visual Studio 17 2022" -A x64` and `cmake --build <scratch> --config Release`, with the VS Build Tools' CMake on `PATH`. The delegate is staged as a Houdini package in `<scratch>/pkg`; pass it to the runners with `-Stage <scratch>/pkg`.
