# M5 research: VEX materials and the CVEX bridge (Houdini 22.0.466)

Scope: docs/PLAN.md M5, "CVEX-Bridge für VEX-Fallback (batched, thread-local Kontexte)", result "VEX-Material-Builder-Materialien rendern".
The report covers:
- how VEX materials reach USD and Hydra;
- what Karma does with them;
- what the HDK's CVEX API allows;
- a spike that ran a VEX Material Builder's BSDF through CVEX.

Sources: the Houdini install (`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`, `$I` = `$HFS/toolkit/include`), two read-only research subagents (header and doc reading, DLL string scans), and my own hython probes.
Every probe ran in its own instance through `tests/houdini/run_hython.ps1` (own preferences, Nova package skipped); the user's Houdini session was not touched.
The probe and spike scripts are in `prototypes/`. They contain no SideFX code; `spike_principled.py` reads the Principled Shader's VFL from the install at run time.

---

## 0. Findings in short

1. **USD form** (hython probe, `prototypes/probe_vex_usd.py`):
   - **Principled Shader:**
     - `outputs:vex:surface` → a Shader with `info:implementationSource = "sourceAsset"` and the universal `info:sourceAsset = @opdef:/Vop/principledshader::2.0?SurfaceVexCode@`.
     - Parameters arrive as `inputs:<vex parm>`: `vector3f` for VEX vectors, `asset` for files. Values are authored only when not at their default; preview-tagged parameters are declared without a value.
     - With displacement on, `outputs:vex:displacement` → `...?DisplacementVexCode` with the `dispTex_*`/`dispNoise_*` inputs.
     - An auto-generated UsdPreviewSurface sits on the universal `outputs:surface`.
   - **VEX Material Builder:**
     - `implementationSource = "sourceCode"`, and `info:sourceCode` holds the complete generated VFL of the `surface` (and `displace`) function, so husk needs no hip file.
     - Promoted parameters become inputs. The builder writes **no** preview shader.
   - Houdini writes the universal `info:sourceAsset`/`info:sourceCode`, not `info:vex:*`. `tests/husk/check_materials.py` used `info:vex:sourceAsset`; M5 fixes it. The Sdr source type Houdini uses for VEX is `VEX` (`husd/shaderutils.py:1440`).
2. **Karma:**
   - Context order `kma` > `mtlx` > `vex` > universal (`help/solaris.zip`, `kug/materials.txt:44-77`).
   - VEX runs on Karma CPU only. XPU "will never support VEX-based shaders" and renders the Principled Shader through its auto preview (`karma.txt:19`, `kug/materials.txt:133-143`).
   - Karma resolves shader identity from `sourceCode`/`sourceAsset` in a scene index (`dso/usd_plugins/hdKarma/resources/plugInfo.json:44-51`).
   - VEX parameters bind to same-named primvars (Mantra convention). `UsdPrimvarReader` connections into VEX inputs become primvar bindings.
3. **CVEX API** (`$I/CVEX/*.h`, `$I/VCC/VCC_Utils.h`):
   - `CVEX_ContextT<VEX_32>` with `addInput`/`addRequiredOutput` before `load`/`loadFunction`, `setTypedData` for buffers of size 1 (uniform) or N (varying), and `run(n, interruptable, runData)` (`CVEX_Context.h:222-473`).
   - VFL source compiles at run time with `VCC_Utils::preloadLocalFunction(ctx, source)` → `CVEX_Function` (`VCC_Utils.h:27-31`). Karma (`libBRAY`) and Solaris (`libHUSD`) use CVEX and VCC inside husk.
   - Use one context per thread. Reuse it for any n ≤ the bound buffer size. Switching an input between uniform and varying needs a reload (`CVEX_Context.h:382-406`).
   - Loading is not documented as thread-safe: serialize it.
   - Libraries `libCVEX`, `libVEX`, `libVCC`, `libUT` (`$HFS/custom/houdini/dsolib`); CMake targets `Houdini::CVEX` etc. (`$HFS/toolkit/cmake/HoudiniConfig.cmake:15-16,112-117`).
   - **Only `cvex`-context functions run**. That context has no globals: `P`, `N`, `s`, `t`, ... are ordinary parameters (`vex.zip`, `contexts/cvex.txt:14`).
   - **No `bsdf` type** crosses the API (`CVEX_Value.h:29-52`). However, `eval_bsdf`, `sample_bsdf` and `albedo` are available in every context (`vex.zip`, `functions/*.txt`).
   - `texture()` runs on Houdini's own texture system inside VEX.
4. **Spike: a builder's BSDF runs through CVEX** (`prototypes/spike_*.py`).
   - **Transformation:**
     - turn the builder's surface function into a `cvex` function whose parameters are the surface globals plus Nova's inputs (light direction, two sample numbers);
     - declare `bsdf F` locally;
     - append `eval_bsdf`, `albedo` and `sample_bsdf` on `F` with exports.
   - **Shims needed:**
     - `getraylevel()`, `isshadowray()` and `rayimport()` become constants;
     - a two-argument `frontface`;
     - inside the library part before the shader, `dPds`, `dPdt`, `Ng`, `isshadingRHS()` and `objectstate()` stand-ins;
     - `init_layer`'s two reads of `P` and `N` are rewritten in memory (`shaderlayer.h:61-62`).
   - `VOP_SHADING` must stay defined; without it the Principled Shader Core leaves out its lobes.
   - Result: the code compiles (vcc, 0.2 s) and evaluates. The colour, Oren-Nayar diffuse, specular and sheen of a Principled Shader Core with a noise-driven base colour come out per lane.
   - Speed: 32 768 lanes in 50-100 ms through `hou.runVex`, Python overhead included.
5. **VEX BSDF conventions, measured:**
   - `diffuse(N)` evaluates to cos θ with pdf cos θ, and `albedo()` returns 0.5. The PBR VOPs write `2.0 * diffuse(...)`.
   - So `eval_bsdf` = 2π · f · cos θ and the physical albedo is `albedo()`.
   - `eval_bsdf`'s pdf of a mixture is 2π · Σᵢ Aᵢ pdfᵢ: the mixture pdf is not normalized. Normalizing by A = luminance(`albedo(F)`) matched the Monte-Carlo estimate (1.79 vs 1.83).
   - `sample_bsdf` returns the sampled *component's* direction and weight (`eval` = the path throughput weight; Houdini's `Fog/lighttracer.vfl` multiplies by it directly). Its pdf belongs to that component.
   - For MIS, Nova evaluates `eval_bsdf` again at the sampled direction in the same run.
6. **Limits of the CVEX route:**
   - VOP library helper functions read shading globals (`vop_xformuvw` reads `s`, `t`; `computenormal` reads `Ng`). In `cvex` they cannot see the shader's parameters.
   - Replacing them by defaults compiles but changes results where a helper falls back to a global. The Principled Shader's unconnected UVs, for example, read `s`/`t` there.
   - Surface-only statements (`pbrlighting`, for AOV exports) have to be removed.
   - The Principled Shader's 531 KB of preprocessed VFL (`opdef:...?SurfaceVflCode`, read with `hou.readFile`) compiles after three such fixes. Its texture UVs would be wrong without more patching.
   - A native translation is therefore the safer route for the Principled Shader, and CVEX for the builder code.

## 1. USD probe output (abridged)

```
def Material "principled" {
  token outputs:surface.connect = </materials/principled/principled_preview.outputs:surface>
  token outputs:vex:displacement.connect = </materials/principled/principled_displace.outputs:displacement>
  token outputs:vex:surface.connect = </materials/principled/principled_surface.outputs:surface>
  def Shader "principled_surface" {      customData = {bool HoudiniHasAutoPreviewShader = 1}
    uniform token info:implementationSource = "sourceAsset"
    uniform asset info:sourceAsset = @opdef:/Vop/principledshader::2.0?SurfaceVexCode@
    vector3f inputs:basecolor = (0.8, 0.1, 0.1)
    float inputs:rough = 0.35
    asset inputs:basecolor_texture  (declared, customData HoudiniPreviewTags)
    int inputs:basecolor_useTexture  ...
    token outputs:surface
  }
  def Shader "principled_displace" {
    uniform token info:implementationSource = "sourceAsset"
    uniform asset info:sourceAsset = @opdef:/Vop/principledshader::2.0?DisplacementVexCode@
    int inputs:dispTex_enable = 1
    token outputs:displacement
  }
}
def Material "vexbuilder" {    customData.houdini.dialogScript = <the builder's parameter UI>
  token outputs:vex:displacement.connect = </materials/vexbuilder/vexbuilder_displace.outputs:displacement>
  token outputs:vex:surface.connect = </materials/vexbuilder/vexbuilder_surface.outputs:surface>
  def Shader "vexbuilder_surface" {
    uniform token info:implementationSource = "sourceCode"
    string info:sourceCode = "#ifndef VOP_SHADING\n#define VOP_SHADING\n#endif\n ... surface\nstage_matlib_vexbuilder(...)\n{ ... }"
    token outputs:surface
  }
}
```

A builder with a Principled Shader Core, a noise and a parameter VOP writes 26 565 characters of VFL. It includes `shaderlayer.h`, `voplib.h`, `bsdf.h` and `physicalsss.h`, and ends with `F = ...; Of = ...;`.

## 2. Principled Shader 2.0 parameters

The surface function has 281 parameters (`SurfaceVflCode`, signature near line 5801).
- **Main values:** `basecolor`, `albedomult`, `metallic`, `rough`, `aniso`, `anisodir`, `ior`, `reflect`, `reflecttint`, `coat`, `coatrough`, `sheen`, `sheentint`, `sss`, `sssdist`, `ssscolor`, `sssmodel`, `sssphase`, `transparency`, `transcolor`, `transdist`, `dispersion`, `emitcolor`, `emitint`, `emitillum`, `opac`, `opaccolor`, `priority`, `alphablendmode`, `alphacutoff`.
- **Texture variants:** most of them have `*_useTexture`, `*_texture`, `*_textureWrap`, `*_textureColorSpace` and `*_monoChannel`.
- **Normal groups:** `baseBump*`, `baseNormal*`, `coatBump*`, `coatNormal*`.
- **UV transform:** `uvtrans`, `uvrot`, `uvscale`.
- **Primvar-bound inputs:** `Cd`, `Alpha`, `displayColor`, `displayOpacity`, `st`, `uv`, `baseN`, `coatN`.
- **Displacement:** 34 parameters (`dispTex_*`, `dispNoise_*`, `dispInput_enable`, `disp`, `vdisp`). It writes `P` and `N`.
- **Exports:** `export_basecolor` etc. Karma's albedo AOV reads `export_basecolor`.
- **Lobe construction** (inside the core):
  - the diffuse is `2 * diffuse(N, Ng, rough)` (Oren-Nayar) times `basecolor * albedomult`, mixed with SSS lobes by `sss`;
  - the sheen is a `cvex_bsdf("principleddiffuse_eval/sample")` lobe;
  - specular, refraction and coat are `get_bsdf("ggx", ...)` (`$HFS/houdini/vex/include/bsdf.h:79`);
  - `(1 - metallic)` scales the dielectric base, and layers are composed through `ShaderLayer` (`shaderlayer.h`).

## 3. CVEX run model (from the headers)

1. **Once per shader, under a lock:**
   - transform and compile the source to a `CVEX_Function` (`VCC_Utils::preloadLocalFunction`);
   - list its parameters (`CVEX_Function::getParameters`).
2. **Per thread and shader:**
   - one `CVEX_ContextT<VEX_32>` and one `CVEX_RunDataT<VEX_32>`;
   - varying inputs declared before `loadFunction`;
   - buffers of the maximum batch size bound with `setTypedData`.
3. **Per batch:**
   - fill lanes `0..n-1`;
   - `run(n, true, &runData)`;
   - read the outputs (a uniform output has one value).
4. **Invalidation:** `isValid()` turns false when Houdini flushes its function cache; rebuild the context then.

## 4. Open questions for the implementation

- How Hydra delivers `sourceAsset`/`sourceCode` shaders to Nova: node type id empty, the raw values in `nodeTypeInfo`? Answer with a Hydra test on a usda file.
- Whether libCVEX/libVCC work in `nova_cli` (not a Houdini application) or only in husk/houdini/hython. If they do not, `nova_cli` falls back to the preview.
- The exact mixture-pdf normalization of `eval_bsdf` (A = luminance of `albedo(F)` matched within MC noise) needs a precise test with an analytic mixture (diffuse + GGX).
- Emission: the builder's surface output has no `Ce` input; where a VEX surface sets the global `Ce`, the bridge exports it.
