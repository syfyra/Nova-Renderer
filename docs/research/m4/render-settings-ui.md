# M4 research: render settings UI for Nova (Houdini 22.0.466)

Scope: PLAN.md M4 item "`UsdRenderers.json`-Integration (Render-Settings-LOP, Parameter-UI)" (docs/PLAN.md:61).
Every statement below comes from a file in this repo, a file of the Houdini 22.0.466 install
(`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`), the HtoA 6.5.3.4 install
(`C:/Users/Synti/htoa/htoa-6.5.3.4_houdini-22.0.429/...`, referenced by `Documents/houdini22.0/packages/htoa.json`),
the HDK online page `https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html` (linked from `$HFS/houdini/python3.13libs/usdrenderers.py:13`),
or an experiment run in a separate, isolated hython/husk process.

The experiments live in the session scratchpad
(`.../scratchpad/m4research/render-settings/`: `scripts/*.py`, `exp*_out.txt`, the prototype package `novapkg/`).
Each run used its own `HOUDINI_USER_PREF_DIR`, with `HOUDINI_PACKAGE_SKIPLIST=nova` and `HOUDINI_PATH=<pkg>;&`.
They never touched the user's Houdini session.

---

## 0. Key findings

1. **`UsdRenderers.json` holds only delegate metadata** (menu, viewport behaviour, husk defaults, stats).
   It has no key for render-setting UI or AOV menus.
   PLAN.md:35 says `UsdRenderers.json` holds "Render-Settings, AOV-Menüs". That is not how Houdini works, so the plan needs a stated deviation (section 4).
2. **Renderer tabs on the stock LOPs come from dialog-script files** named
   `$HOUDINI_SOHO_PATH/parameters/<PluginId>_<Group>.ds`. The groups are `Global`, `Geometry`, `Aov`, `Product`, `Camera`, `Light` and `PortalLight`.
   `loputils.addRendererParmFolders()` adds them as spare parameters when a node is created.
   The viewport display options read `<PluginId>_Viewport.ds` instead.
3. **Prototype verified end to end.** Three `.ds` files were added to a copy of the staged Nova package (`soho/parameters/HdNovaRendererPlugin_{Global,Geometry,Viewport}.ds`). With them:
   - the stock Render Settings LOP gets a **Nova** tab;
   - it authors `custom int nova:samples = 16` on `/Render/rendersettings`;
   - husk renders that stage with Nova at 16 spp (EXR metadata `samples = 16`, `renderProgressAnnotation = "16/16 spp"`).

   Render Geometry Settings also gets a Nova tab and authors `primvars:nova:subdivLevel` (constant interpolation).
   No C++ change was needed.
4. **Karma and HtoA both use two layers.** The first is a dedicated "X Render Settings" HDA that writes everything.
   The second is a tab on the stock LOPs:
   - Karma: `.ds` files.
   - HtoA: `scripts/lop/<type>_CreateScript.py` event scripts that build the tab from Arnold metadata.
5. **There is no AOV-preset registry for delegates.**
   - The Additional Render Vars presets are a hard-coded Karma list (`loptoolutils.getAdditionalRenderVars`).
   - Karma and Arnold ship their own render-var HDAs.
   - `aovsupport: true` only makes the delegate appear in the USD Render ROP renderer menu (verified: Nova is listed, VK and Storm are not).

---

## 1. `UsdRenderers.json`

### 1.1 How Houdini reads it (`$HFS/houdini/python3.13libs/usdrenderers.py`)

- **Where the files come from.** `hou.findFiles('UsdRenderers.json')` collects every file on `HOUDINI_PATH` (l.95). The files are applied in **reverse** order with `theDataMap |= ...` (l.99-102).
  - So the file earliest on the path wins.
  - It replaces a renderer's **entire entry**: entries are not merged key by key.
  - The cache is built once per session.
  - Measured with the staged package: `findFiles` returned `[.../stage/Release/nova/UsdRenderers.json, $HFS/houdini/UsdRenderers.json]` (exp3).
- **`inherit_from`** copies another entry and lets the child override keys (l.22-35). Karma CPU and XPU inherit `BRAY_HdKarma_base` (`$HFS/houdini/UsdRenderers.json:113,120`).
- **`valid`** may be `true`/`false`, `"env:NAME"`, `"env:-NAME"` or `"platform:REGEX"` (l.50-64).
- **`menulabel`** is expanded with `hou.text.expandString` (l.66).
- **`qmlTemplate`** is resolved relative to its JSON file (l.19, l.73-84).
- **`HOUDINI_DEFAULT_RENDERERS`** (comma list, `-Name` excludes) can mask a renderer (l.111-131).
- **C++ side:** `HUSD_RendererInfo` (`$HFS/toolkit/include/HUSD/HUSD_RendererInfo.h`) receives the dict through `__import__('usdrenderers').getRendererInfo(...)`. That string is at `libHUSD.dll` 0x589558. The key names are the strings at `libHUSD.dll` 0x5895b4-0x5897c8 (listed below).

### 1.2 Every key Houdini 22 understands

Sources: the defaults come from `HUSD_RendererInfo.h:243-273`, the meanings from the header comments and the HDK page, and the key names from the libHUSD strings. `K` = Karma's value (`$HFS/houdini/UsdRenderers.json`); `N` = Nova's current value (`houdini/package/UsdRenderers.json`).

| Key | Default | Effect | K | N |
|---|---|---|---|---|
| `valid` | true | delegate available (see 1.1) | true | true |
| `menulabel` | "" (display name) | viewport/ROP menu label; fallback label of the `.ds` tab (`loputils.py:4064-4071`); fallback `rendererName` in the stats panel (`renderstats/widgets.py:431-436`) | "Karma CPU" | "Nova" |
| `menupriority` | 0 | higher = higher in the viewport renderer menu | 50 / 49 (XPU) | 60 |
| `complexitymultiplier` | 1.0 | multiplies the Hydra complexity derived from Display Options LOD (header l.61-64) | 1.3 | 1.3 |
| `depthstyle` | header: `HUSD_DEPTH_OPENGL`; HDK page: "normalized" | depth range the delegate returns (`linear`/`opengl`/`normalized`) | linear | linear |
| `defaultpurposes` | `["render"]` | purposes enabled when the delegate is activated | render | render |
| `restartrendersettings` | [] | setting names (glob allowed, e.g. `karma:object:*`) whose change makes Houdini restart the delegate | engine, overrideobject, dicingqualityscale, `karma:object:*`, renderingColorSpace, disableMotionBlur, houdini:fps (l.48-55) | — |
| `restartcamerasettings` | [] | camera attributes that force a restart | shutter:open/close (l.56-58) | — |
| `viewstats` | [] | extra stat labels shown with View Stats | 6 ray counters | `["Samples"]` |
| `statsdatapaths` | {} | label/canonical stat → JSONPath into `GetRenderStats()` (canonical: percentDone, peakMemory, totalClockTime, totalUTime, totalSTime, rendererStage, renderProgressAnnotation, renderStatsAnnotation, rendererName, activeBuckets) | many | `{Samples: samples}` |
| `needsdepth` | false | Houdini GL renders a depth pass for the delegate | false | false |
| `needsselection` | false | GL draws the selection highlight overlay | true | true |
| `allowbackgroundupdate` | true | scene-graph updates on a background thread | (true) | true |
| `destroyifdeactivated` | false | delegate destroyed when the viewport switches to another one | (false) | — (HtoA: **true**) |
| `pauseonupdate` | true | `Pause()` during scene updates (needed when the delegate reads COP textures) | (true) | true |
| `aovsupport` | true | delegate makes AOV buffers; **controls the USD Render ROP renderer list** | (true) | true |
| `viewportrenderer` | false | delegate does its own viewport rendering (Storm: true) | — | false |
| `drawmodesupport` | false | honours USD draw modes | false | false |
| `showinviewportmenu` | true | listed in the viewport renderer menu | — | — |
| `showhydraprocedurals` | true | Hydra generative procedurals shown by default (VK: false) | — | — |
| `preloadlibraries` | [] | libraries loaded before the plugin (env vars expanded) | — | — |
| `diskproducttypes` | [] | product types husk treats as files (`--make-output-path`, access checks) | `karma:checkpoint`, `karma:deep` | — |
| `husk.fast-exit` | false | husk skips teardown | 1 | 1 |
| `husk.default_output` | "" | output name when there are no render settings | karma.exr | nova.exr |
| `husk.metadata` | {} | EXR metadata; values may use `${render_stats.<jsonpath>}`, `$frame`, `$fps`, `$command_line` ... | 22 entries | — |
| `husk.stats_metadata` | `*` (HDK) | pattern of `GetRenderStats()` keys copied verbatim to metadata | "" | — (`*`) |
| `husk.verbose_callback` / `husk.verbose_interval` | "" / 0 | Python script for husk verbose output | karma_stats.py / 0.5 | — |
| `inherit_from` (Python only) | — | see 1.1 | base entry | — |
| `qmlTemplate` (Python only) | — | custom QML render-stats panel (`renderstats/widgets.py:421-426`) | BRAY_HdKarma.qml | — |
| custom keys | — | read by individual consumers, e.g. `lighttypes` and `pointinstancertypes` (Houdini GL only, HDK page) | — | — |

The HDK page also lists the settings Houdini sends to every delegate:
- `houdini:frame`, `houdini:fps`, `houdini:viewport`, `houdini:interactive` (`normal|flipbooking|playing|scrubbing|editing|tumbling|husk:mplay`);
- `stageMetersPerUnit`, `renderCameraPath`, `houdini:render_pause`, `viewerMouseClick`, `houdini:cop_texture_changed`;
- husk only: `usdFilename`, `batchCommandLine`, `huskDelegateOptions`, `husk:snapshot`;
- and the bulk `delegateRenderProducts`.

### 1.3 Nova's file: what is there and what is missing

Nova's file (`houdini/package/UsdRenderers.json`, identical in `dist/nova/`) has 17 keys. All are valid and none is wrong.

Gaps, in order of relevance:

- **`restartrendersettings`: not needed for the current settings.** `HdNovaRenderPass` compares setting values every Execute and restarts by itself (`hydra/renderPass.cpp:294-311`).
  Exceptions: `nova:textureAutoTx` and `nova:textureCacheDir` only affect textures prepared afterwards (`renderPass.cpp:299-306`). Either list them here or re-prepare the textures in code.
- **`husk.metadata`: none.** The default `*` copies all of Nova's stats.
  - Measured in `exp5.exr` (iinfo -v): fractionDone, numCompletedSamples, percentDone, rays, raysPerSecond, renderProgressAnnotation, rendererName, rendererStage, samples, totalClockTime, plus husk's own renderMemory/renderTime.
  - Comp tools need Karma-style `worldToCamera`/`worldToNDC` matrices. That requires Nova to put them into `GetRenderStats()` first, which belongs to M6.
- **`statsdatapaths`:** Nova already returns canonical names directly (`renderDelegate.cpp:324-336`). Add `peakMemory` once Nova reports memory.
- **`destroyifdeactivated`:** a product decision (see Open questions). `false` keeps the BVH and textures alive when switching renderers.
- **`diskproducttypes`, `qmlTemplate`, `husk.verbose_callback`:** only if Nova gets checkpoint/deep products, a custom stats panel, or husk progress output.

---

## 2. Renderer parameters on Solaris LOPs

### 2.1 The mechanism (`$HFS/houdini/python3.13libs/loputils.py`)

```python
def addRendererParmFolders(node, parmgroup):                 # l.4063-4072
    names  = hou.lop.availableRendererNames()  + ('Husk',)
    labels = hou.lop.availableRendererLabels() + ('Husk',)
    for name, label in zip(names, labels):
        name = hou.text.alphaNumeric(name)
        for dirpath in hou.houdiniPath('HOUDINI_SOHO_PATH'):
            dspath = dirpath + ('/parameters/%s_%s.ds' % (name, parmgroup))
            if os.path.exists(dspath):
                addDialogScriptFolder(dspath, node, label); break
```

`addDialogScriptFolder` (l.4015-4060) works as follows:
- It parses the file with `hou.ParmTemplateGroup.setToDialogScript`.
- The tab label is the file's top-level `label`, or the renderer's menu label if there is none.
- It replaces an existing folder of the same label and skips parameters whose name already exists on the node ("Duplicate folder name" warning). Names must therefore be globally unique; the `nova:` prefix ensures that.
- It inserts the folder after the last visible tab folder.

Consequences:
- The **first** matching file on `HOUDINI_SOHO_PATH` wins, so a studio can override Nova's file.
- `HOUDINI_SOHO_PATH` (default `@/soho`) only lists directories that **exist**. Without the scratch copy only `$HFS/houdini/soho` was listed (exp3); with it, `novapkg/soho` came first (exp4).
- The files are read only when a node is **created** (HDA `OnCreated`), or when the user picks **Initialize Parameters ▸ Rebuild Renderer-Specific Parameters**. That menu item is the `rebuildrendererparms:<Group>` command (l.26, l.2244-2288).

Which LOPs call it was found by scanning every LOP/ROP/OBJ HDA in hython (exp8):

| Group (`<PluginId>_<Group>.ds`) | LOP types (OnCreated) |
|---|---|
| `Global` | rendersettings |
| `Geometry` | rendergeometrysettings |
| `Aov` | rendervar, rendervar::2.0 |
| `Product` | renderproduct |
| `Camera` | camera |
| `Light` | light, light::2.0, distantlight(::2.0), domelight(::2.0, ::3.0), geometrylight, karmaskydomelight |
| `PortalLight` | portallight |
| `Viewport` | not a LOP: viewport Display Options ▸ Render tab and the viewer toolbar (2.6) |

The `Husk` pseudo-renderer always gets its tab (`Husk_Global.ds` etc.). It includes `husk:default_delegate`, which husk uses to pick a delegate after `--renderer` (help `ref/utils/husk.txt:346-352`).

### 2.2 `.ds` format and the tags that matter

Karma's files are generated (`/// Automatically generated file`, `$HFS/houdini/soho/parameters/BRAY_HdKarma_*.ds`). They follow the Edit-Properties convention:

- **One pair of parameters per USD attribute:**
  - `hou.text.encodeParm("<attr>_control")`: a string menu (`loputils.createEditPropertiesControlMenu(kwargs, '<usdtype>')`). Measured items: `set`, `setnotauthored`, `setexisting`, `add`, `multiply`, `block`, `animationblock`, `none`. The default is `none` (Do Nothing).
  - `hou.text.encodeParm("<attr>")`: the value parameter, with `disablewhen "{ ctl == block } { ctl == none }"`.
  - The encoded name decodes back to the USD attribute name, e.g. `nova:samples` → `xn__novasamples_xta` (exp gen_ds).
  - For `rendergeometrysettings` the attribute is the primvar itself, e.g. `primvars:karma:object:diffuselimit`.
- **Tags on the value parameter:**
  - `usdvaluetype` (int, float, bool, token, string, color3f, ...) sets the USD type.
  - `usdapischema` (e.g. `KarmaRenderSettingsAPI`) applies an API schema. It is optional: without it Nova's attributes are authored as `custom` (exp4).
  - `spare_category` and `uiscope`: `uiscope` is `None`, `viewport` and/or `toolbar`, and the LOPs ignore it.
  - `uiicon` sets the toolbar icon.
- **Layout:** `group {}` makes sub-tabs and `groupcollapsible {}` makes sections (`grouptag { "group_default" "1" }` = open).
  - Karma's Global file has the sub-tabs **Global** and **Default Geometry Settings**. The latter authors `karma:object:*` defaults on the RenderSettings prim (`BRAY_HdKarma_Global.ds`, group `KarmaRenderSettingsAPI_object`).
  - Hidden internal parameters use `invisible`.

### 2.3 Experiment: Nova prototype on the stock LOPs (exp4, exp5, exp13, exp14)

`scripts/gen_ds.py` (hython) wrote three files into `novapkg/soho/parameters/`:
- `HdNovaRendererPlugin_Global.ds`: 9 settings in Sampling, Image, Geometry and Textures sections;
- `HdNovaRendererPlugin_Geometry.ds`: `primvars:nova:subdivLevel`;
- `HdNovaRendererPlugin_Viewport.ds`: the 5 `uiscope viewport` settings, without control parameters.

All three parse with `setToDialogScript`. Results:

| Check | Result |
|---|---|
| `hou.lop.availableRendererNames()/Labels()` | `HdNovaRendererPlugin` / `Nova` present |
| rendersettings tabs | Frame Range, Create Primitives, Standard, Karma, **Nova**, Husk |
| rendergeometrysettings tabs | Frame Range, Create Primitives, Karma, **Nova** |
| set `nova:samples` (control `set`, 16), `nova:pixelFilter` (`set`, box), `nova:maxDepth` value 3 with control `none` | `/Render/rendersettings`: `custom int nova:samples = 16`, `custom token nova:pixelFilter = "box"`; **`nova:maxDepth` not authored** (Do Nothing = sparse) |
| rendergeometrysettings on `/cube`, `primvars:nova:subdivLevel` = 2 | `int primvars:nova:subdivLevel = 2`, interpolation `constant` |
| husk `--renderer Nova --res 64 48` on the exported stage | renders; EXR metadata `samples = 16`, `numCompletedSamples = 16`, `__delegate = HdNovaRendererPlugin` |
| hip saved **without** the `.ds`, loaded with it | no Nova tab; after `loputils.initializeParameters(node, 'rebuildrendererparms:Global')` the Nova tab is inserted between Karma and Husk |
| hip saved **with** Nova parms, loaded with Nova not installed | the Nova tab persists (spare parms live in the hip) and `nova:samples = 32` is still authored |
| USD Render ROP `renderer` menu | `default_delegate, BRAY_HdKarmaXPU, HdNovaRendererPlugin, BRAY_HdKarma` (VK/Storm absent: `aovsupport` false) |
| wall time | hython test 1.9 s; husk 64x48 16 spp 1.3 s |

Notes:
- In hython the control must be set explicitly. Whether the GUI flips it to "Set or Create" when a value is edited was not verified here.
- Setting `resolution` failed because `resolution2` is locked by `res_mode=autoheight`. This is unrelated to Nova.
- The default RenderSettings prim path of the stock LOP is `/Render/rendersettings` regardless of the node name (exp14).

### 2.4 How Karma and Arnold do it

**Karma** (`$HFS/houdini/otls/OPlibLop.hda`) uses both layers:
- *Karma Render Settings* (`karmarendersettings`) is an HDA subnet with 402 parameters and friendly names (`samplesperpixel`, ...).
  - Its internals are a `pythonscript1` LOP that calls `husd.karmautils.renderProperties(...)`, plus a stock `rendersettings` and pruning/merge nodes (exp10, exp12).
  - It authors **every** `karma:global:*` attribute and applies `KarmaRenderSettingsAPI`, `HuskRenderSettingsAPI` and `HuskRenderProductAPI` (exp9). It also creates products and render vars.
- The stock LOPs get Karma tabs from `BRAY_HdKarma_{Global,Geometry,Aov,Product,Camera,Light}.ds` and the viewport from `BRAY_HdKarma_Viewport.ds` (`$HFS/houdini/soho/parameters/`).
- `RenderPropertiesKA.json` is the generator source: it holds the same properties with `uiscope`, `uimenu` and `renderer` fields.

**Arnold (HtoA 6.5.3.4)** also uses both layers:
- `otls/lop_arnold_rendersettings.hda` (`arnold_rendersettings`, 1847 parameters, no spare parameters) is a subnet. Inside it are `rendersettings`, `renderproduct`, `arnold_rendervars`, `pythonscript`, `materiallibrary`, ... (exp6).
- The stock LOPs get an **Arnold** tab from **file event handlers** `scripts/lop/{rendersettings,rendergeometrysettings,rendervar,renderproduct,camera}_CreateScript.py`. These call `htoa.parameter.Solaris*().build()`, which builds `hou.FolderParmTemplate('Arnold', ...)` from Arnold node-entry metadata and `setParmTemplateGroup`s it (`scripts/python/htoa/parameter.py:506-560`).
  - In the isolated hython run they raised `ai.dll` not found or `KeyError` (Arnold was not on `PATH`). Only rendervar got its Arnold tab, with 4 parms. This shows the fragility of the approach.
  - The documented file-event mechanism is `HOUDINI_PATH/scripts/<category>/<type>_<event>.py` (help `hom/locations.txt:398-416`). The `CreateScript` event name used by HtoA is not documented there, but the run shows it fires ("Error running event handler", exp6).
- The viewport and lights use `.ds`: `soho/parameters/HdArnoldRendererPlugin_Viewport.ds` (unprefixed names such as `AA_samples`, `uiscope "viewport toolbar"` + `uiicon`) and `HdArnoldRendererPlugin_Light.ds`.
- Its `UsdRenderers.json` has only 5 keys, including `destroyifdeactivated: true`.

**RenderMan, Redshift:** not installed on this machine, so nothing could be verified. Both ship their own render-settings LOP HDAs; that is general knowledge, not measured here.

### 2.5 Per-object properties (Render Geometry Settings)

- **Karma** (`BRAY_HdKarma_Geometry.ds`) defines `primvars:karma:object:*` value/control pairs in collapsible sections. Measured section sizes, control parms included (exp1):

  | Section | Parms |
  |---|---|
  | Motion Blur | 22 |
  | Sampling | 26 |
  | Limits | 14 |
  | Uniform Volume | 4 |
  | Shading | 30 |
  | Light | 6 |
  | Geometry | 10 |
  | Dicing | 12 |
  | Outline | 18 |
  | Texture Baking | 6 |

  Internal properties are `invisible`. The LOP authors constant primvars on the selected prims; USD constant primvars are inherited by descendants.
- **Global defaults:** the same names without `primvars:` (`karma:object:*`) live on the RenderSettings prim (Global.ds "Default Geometry Settings").
- **Restarts:** Karma lists `karma:object:*` in `restartrendersettings`.
- **Nova today:** reads `primvars:nova:subdivLevel` (README.md:143, `hydra/mesh.cpp:302`) and Karma's light properties `karma:light:*` (README.md:127). Karma's Light tab therefore already drives Nova lights.

### 2.6 Viewport Display Options

The evidence that the viewport needs a `_Viewport.ds` file:
- `libJEDI.dll` contains, side by side: `parameters/%s_Viewport.ds` (0x384298), `uiscope`, `toolbar`, `uiicon`, `rmbmenu_parent`, and **`No options for current renderer`** (0x384249).
- Houdini's own example `$HFS/houdini/soho/parameters/HdEmbreeRendererPlugin_Viewport.ds` says in its header comment:
  - Houdini loads UI "based on the name of the Hydra plugin";
  - "the delegate is notified through the SetRenderSetting() virtual. The "name" of the parameter is sent as the TfToken";
  - names are `hou.text.encodeParm`-encoded;
  - `uiscope` `viewport` = Display Options, `toolbar` = viewer side bar, with `uiicon`.
- The HDK page says the same.

What could not be established:
- **No sign that Houdini builds this UI from `GetRenderSettingDescriptors()`.** The HDK page never mentions it, and no descriptor string was found in libJEDI, libHUSD or libLOP. The likely conclusion: without `HdNovaRendererPlugin_Viewport.ds`, the Render tab shows "No options for current renderer". **This was not verified in the GUI** (no HOM API exposes that tab).
- The viewport also uses a RenderSettings prim (libJEDI strings "Using render settings '%s' instead of '%s'" and "Unable to find a valid render settings prim"). How prim values combine with Display Options values is undocumented (HDK page) and unverified.

### 2.7 Reading the descriptors from hython (useful for tests)

`pxr.UsdImagingGL.Engine(Engine.Parameters(gpuEnabled=False))` followed by `SetRendererPlugin('HdNovaRendererPlugin')` and `GetRendererSettingsList()` works in hython without a GPU (exp15).

It returned 14 of Nova's 16 descriptors with key, label, type (INT/FLOAT/FLAG/STRING) and default. The two token-valued settings, `nova:pixelFilter` and `nova:lightStrategy`, are missing because UsdImagingGL lists only those four types.

---

## 3. AOV / render-var menus (relevant for M6)

- **No delegate-level preset registry.**
  - The Additional Render Vars LOP's `name#` menu (71 items: `C`, `beautyunshadowed`, `shadow`, `combineddiffuse`, ...) comes from `loptoolutils.getAdditionalRenderVarsMenu()`, which is a hard-coded list of Karma AOVs (`$HFS/houdini/python3.13libs/loptoolutils.py:1710, 1931`).
  - The `sourceName#` menu offers LPE strings (`C.*`, `C<RD>.*`, ...), and `sourceType` offers `raw|primvar|lpe|intrinsic` (exp7).
- **Delegate-specific render-var parameters** use the same `.ds` mechanism:
  - `<PluginId>_Aov.ds` → tab on the Render Var LOP (`BRAY_HdKarma_Aov.ds`);
  - `<PluginId>_Product.ds` → tab on the Render Product LOP.
  - HtoA instead adds `arnold:filter`/`arnold:width` via `rendervar_CreateScript.py`.
- **Presets as HDAs:** Karma ships `karmastandardrendervars` (HDA, categories with toggles) and Karma Render Settings ▸ Image Output ▸ AOVs. HtoA ships `otls/arnold_rendervars.hda` (1.1 MB) and `arnold_additional_render_vars.hda`.
- **Viewport AOV menu:** Solaris doc `solaris/support/aovs.txt` "Viewing AOVs" says the Render Outputs button lists the active render vars. `aovsupport` must be true, and Nova's is.
- **What the delegate receives:** render products arrive as `delegateRenderProducts` (HDK page). Nova already resolves RenderVars (`hydra/renderBuffer.h:57-61`).

Conclusion for M6: a "Nova Standard Render Vars" HDA, or a Nova section in a Nova Render Settings HDA, would be the Nova analogue. `UsdRenderers.json` plays no part in it.

---

## 4. Package layout and how files are found

Nova's package puts `$NOVA` on `HOUDINI_PATH` (`houdini/package/nova.json.in`: `"hpath": "$NOVA"`). The subfolders Houdini scans on `HOUDINI_PATH`:

| Path in `dist/nova/` | Found by | Used for |
|---|---|---|
| `UsdRenderers.json` | `hou.findFiles` (usdrenderers.py:95) | delegate metadata (exists) |
| `dso/usd_plugins/` | USD plugin path | hdNova (exists) |
| `soho/parameters/HdNovaRendererPlugin_<Group>.ds` | `HOUDINI_SOHO_PATH` (`@/soho`, existing dirs only) | LOP tabs + viewport options (**new**) |
| `otls/*.hda` | `HOUDINI_OTLSCAN_PATH` (`@/otls`) | Nova Material Builder (M4), optional Nova Render Settings HDA |
| `python3.13libs/nova*/` | `sys.path` (as `$HFS/houdini/python3.13libs`) | helper module for HDA callbacks or generators |
| `scripts/lop/<type>_<event>.py` | file event handlers (help `hom/locations.txt:398-416`) | **avoid** for stock types: HtoA already ships `rendersettings_CreateScript.py` etc. |

**Deviation from PLAN.md:35 (to state explicitly).** The render-settings UI and AOV menus do not belong in `UsdRenderers.json`. The render-settings UI goes into `soho/parameters/*.ds` (plus an optional HDA in `otls/`, as PLAN.md:36 already foresees). AOV menus go into HDAs (M6).

`houdini/CMakeLists.txt` needs one more `install(DIRECTORY package/soho DESTINATION "${NOVA_PACKAGE_ROOT}")`. The husk/hydra tests stage the package into `build/<preset>/stage/<config>/nova`, which must include it too.

---

## 5. Recommended design for M4

### 5.1 Files

```
houdini/package/soho/parameters/
  HdNovaRendererPlugin_Global.ds     # Render Settings LOP "Nova" tab: all 16 nova:* settings
  HdNovaRendererPlugin_Viewport.ds   # Display Options > Render: interactive subset, uiscope viewport
  HdNovaRendererPlugin_Geometry.ds   # Render Geometry Settings "Nova" tab: primvars:nova:*
houdini/tools/gen_nova_ds.py         # hython generator (one settings table -> the three .ds)
tests/houdini/test_render_settings_ui.py   # hython test (5.3), ctest label "houdini"
```

**Generation.** Commit the `.ds` files, generated from one table (key, label, USD type, default, range, menu, section, help, uiscope).

`hou.text.encodeParm` needs hou, and therefore a license. So keep the build free of hython: the generator runs by hand, and the test checks that the committed files match the table and the delegate.

The table must stay consistent with `_Initialize()` (`hydra/renderDelegate.cpp:77-94`):
- labels: today's labels contain hints such as "(-1: from complexity)" that belong in the help text;
- defaults: the `.ds` defaults are what the UI shows while the control is "Do Nothing".

**Example `Global.ds` entries** (format verified in exp4; one control/value pair shown, the others repeat it):

```
{
    name    "nova"
    label   "Nova"
    parmtag { spare_opfilter "!!SHOP/PROPERTIES!!" }
    parmtag { spare_classtags "render" }
    groupcollapsible {
        name  "nova_sampling"
        label "Sampling"
        grouptag { "group_default" "1" }
        parm {
            name    "xn__novasamples_control_y6a"          // encodeParm("nova:samples_control")
            label   "Samples"
            type    string
            default { "none" }
            menujoin {
                [ "import loputils" ]
                [ "return loputils.createEditPropertiesControlMenu(kwargs, 'int')" ]
                language python
            }
            parmtag { "sidefx::look" "icon" }
        }
        parm {
            name    "xn__novasamples_xta"                  // encodeParm("nova:samples")
            label   "Samples"
            type    int
            size    1
            default { 64 }
            range   { 1 4096 }
            help    "Samples per pixel."
            parmtag { "spare_category" "Sampling" }
            parmtag { "uiscope" "None" }
            parmtag { "usdvaluetype" "int" }
            disablewhen "{ xn__novasamples_control_y6a == block } { xn__novasamples_control_y6a == none }"
        }
        // nova:pixelFilter: type string + usdvaluetype "token" + menu { "gauss" "Gaussian" "box" "Box" ... }
        // nova:textureAutoTx: type toggle + usdvaluetype "bool"
    }
}
```

**`Viewport.ds`:** the same value parameters without control parameters and with `uiscope "viewport"`. This follows the Embree and HtoA examples and Karma's `BRAY_HdKarma_Viewport.ds`, which uses top-level `group {}` tabs.

Suggested subset: samples, maxDepth, clampIndirect, pixelFilter, pixelFilterScale, subdivLevel, textureFilterWidth. Reserve `"viewport toolbar"` + `uiicon` for later toggles (e.g. the M7 denoiser).

**`Geometry.ds`:** today only `primvars:nova:subdivLevel` (int, -1..6) exists. Later per-object settings follow the same pattern; the namespace is an open question.

### 5.2 Behaviour this design gives

- **Sparse authoring** (stock control default "Do Nothing"): only values the user changes reach the stage. Nova's C++ defaults stay the single source of the effective default.
- **Same attributes in viewport and batch:** husk (verified) and the viewport read the same `nova:*` attributes. In both cases they come from the RenderSettings prim and arrive via the `HdRenderSettingsMap`.
- **Old hip files** need "Rebuild Renderer-Specific Parameters" once (verified). Hips keep working without Nova installed (verified).
- **No C++ change is needed.**

### 5.3 Automated test (separate hython, about 2 s)

The test runs like the existing husk tests:
- `HOUDINI_USER_PREF_DIR=<build>/prefs/houdini__HVER__`;
- `HOUDINI_PATH=<stage>/nova;&`;
- `HOUDINI_PACKAGE_SKIPLIST=nova`;
- `HOUDINI_PACKAGE_DIR`, `HSITE`, `PXR_PLUGINPATH_NAME`, `HOUDINI_USD_DSO_PATH` and `HOME` removed.

Assertions: this snippet was run as-is against the prototype package, and every assertion passed. The prototype has 9 of the 16 settings, so the descriptor loop covered the 8 present keys; the soho path was asserted as `/novapkg/soho`.

```python
import hou
from pxr import UsdImagingGL, UsdGeom
enc = hou.text.encodeParm
assert 'HdNovaRendererPlugin' in hou.lop.availableRendererNames()
assert hou.houdiniPath('HOUDINI_SOHO_PATH')[0].endswith('/nova/soho')
stage = hou.node('/stage')
cube = stage.createNode('cube'); geo = stage.createNode('rendergeometrysettings'); geo.setInput(0, cube)
rs = stage.createNode('rendersettings'); rs.setInput(0, geo)
nova = rs.parmTemplateGroup().findFolder('Nova'); assert nova
# 1. every descriptor has a parm with the same default (14 via UsdImagingGL; tokens from the table)
p = UsdImagingGL.Engine.Parameters(); p.gpuEnabled = False
eng = UsdImagingGL.Engine(p); assert eng.SetRendererPlugin('HdNovaRendererPlugin')
for d in eng.GetRendererSettingsList():
    parm = rs.parm(enc(d.key)); assert parm is not None, d.key
    dv = parm.parmTemplate().defaultValue()            # bool for toggles, tuple otherwise
    assert (dv if isinstance(dv, bool) else dv[0]) == d.defValue, d.key
# 2. authoring
rs.parm(enc('nova:samples_control')).set('set'); rs.parm(enc('nova:samples')).set(16)
rs.parm(enc('nova:pixelFilter_control')).set('set'); rs.parm(enc('nova:pixelFilter')).set('box')
rs.parm(enc('nova:maxDepth')).set(3)                        # control 'none' -> must not author
prim = rs.stage().GetPrimAtPath('/Render/rendersettings')
assert prim.GetAttribute('nova:samples').Get() == 16
assert str(prim.GetAttribute('nova:pixelFilter').GetTypeName()) == 'token'
assert not prim.GetAttribute('nova:maxDepth').HasAuthoredValue()
# 3. per-object primvar
geo.parm('primpattern').set('/cube1')                       # cube LOP default primpath is /$OS
geo.parm(enc('primvars:nova:subdivLevel_control')).set('set'); geo.parm(enc('primvars:nova:subdivLevel')).set(2)
pv = UsdGeom.PrimvarsAPI(geo.stage().GetPrimAtPath('/cube1')).GetPrimvar('nova:subdivLevel')
assert pv.Get() == 2 and pv.GetInterpolation() == 'constant'
# 4. viewport file parses; names decode to known descriptor keys; uiscope contains 'viewport'
```

- Optional end-to-end step: export the stage and run `husk --renderer Nova --res 64 48`, then check `samples` in the EXR metadata (`iinfo -v`). This was measured in exp5 at 1.3 s.
- The viewport tab itself can only be checked in a GUI instance. One option is to extend `tests/houdini/run_viewport_check.ps1` with a capture of the Display Options. This is not automated yet.

---

## Recommended M4 scope (minimal)

1. Add `houdini/package/soho/parameters/HdNovaRendererPlugin_{Global,Viewport,Geometry}.ds`:
   - Global: all 16 descriptors, in Sampling / Image / Motion Blur / Geometry / Textures / Debug sections;
   - Viewport: interactive subset;
   - Geometry: `primvars:nova:subdivLevel`.

   Generate them with a hython generator from one table and install them (CMake + staging).
2. Keep `UsdRenderers.json` as is. Optionally add `restartrendersettings: ["nova:textureAutoTx", "nova:textureCacheDir"]`, or re-prepare textures in code instead.
3. Add `tests/houdini/test_render_settings_ui.py` (5.3) as a ctest with the isolated environment.
4. Document the deviation from PLAN.md:35: the render-settings UI lives in `.ds` files, not in `UsdRenderers.json`.
5. Update the README "Render settings" section: Nova tab, Rebuild Renderer-Specific Parameters.

Not in the minimal scope:
- a Nova Render Settings HDA;
- `_Light`/`_Camera`/`_Aov`/`_Product` files;
- AOV presets (M6);
- new per-object properties (each needs C++ support first);
- a USD API schema for `nova:*`.

## Open questions for the user

1. **Dedicated "Nova Render Settings" LOP (HDA), like Karma and Arnold, in M4?** PLAN.md:36 lists it under `otls/`.
   - It would author every value explicitly and could create camera, products and vars in one node.
   - The minimal scope gives only the Nova tab on the stock Render Settings LOP, plus the viewport options.
2. **Namespace for per-object settings:**
   - own `primvars:nova:object:*`, with `nova:object:*` defaults on the RenderSettings prim (Karma pattern);
   - or reuse Karma's `primvars:karma:object:*` where the meaning matches (Nova already does this for `karma:light:*`), so Karma-prepared scenes render the same.

   This also decides whether the existing `primvars:nova:subdivLevel` gets renamed.
3. **`destroyifdeactivated`:** keep `false` (the delegate stays alive and memory stays allocated when switching the viewport to another renderer) or `true` (as HtoA)?
