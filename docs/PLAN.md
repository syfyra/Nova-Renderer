# Plan: eigene Produktions-Render-Engine für Houdini (Arbeitsname „Nova“)

## Context
Ziel ist ein eigener, produktiv nutzbarer Path Tracer im Stil von Arnold/Karma, der direkt in Houdini Solaris arbeitet.
Festgelegt durch Rückfragen:
- **C++20, zuerst CPU**, Architektur aber von Anfang an so, dass Kernels später auf die GPU (CUDA/OptiX) portiert werden können
- **Hydra Render Delegate** (`hdNova`), damit die Engine in Solaris, im Viewport und in husk erscheint
- **Produktiv nutzbar**: Robustheit, AOVs, Performance
- **Features v1**: physikalisches Uber-Material, VDB-Volumen, AOVs, IPR, **OptiX-Denoiser**
- **Materialien**: hybride VEX-Kompatibilität. MaterialX- und Standard-VOPs werden in einen eigenen Shader-Graph übersetzt, unbekannte VEX-Teile laufen über HDK-CVEX als Fallback. Der eigene Material Builder **setzt auf MaterialX auf**, plus ein eigener Uber-Shader.
- **Umgebung**: Windows 11, Houdini 21+, NVIDIA RTX

Das Verzeichnis `C:\Users\Synti\Desktop\renderEngine` ist leer, es ist ein kompletter Neubau.

## Architektur

```
renderEngine/
  CMakeLists.txt            # findet Houdini über $HFS, nutzt Houdinis USD/TBB/OIIO/OpenVDB/MaterialX
  core/      (nova_core, KEINE Houdini-Abhängigkeit)
    math/        Vec/Mat/Frame, Sampling (Sobol/Owen-scrambled), Spektrum-Abstraktion (RGB, später spektral)
    scene/       SoA-Geometrie-Buffer (Mesh, Curves, Points, Instances), Lights, Camera, Volumes
    accel/       IAccel-Interface → Embree4-Backend (später OptiX-Backend)
    shading/     ShaderGraph-IR → kompiliert zu flachem Bytecode/Closure-Programm (GPU-portierbar)
    bsdf/        OpenPBR-/Standard-Surface-Lobes: GGX-Multiscatter, Sheen, Coat, Thin-Film, Transmission, SSS (Random Walk)
    lights/      Area/Dome(Importance-Map)/Distant/Sphere/Disk/Mesh, Light-BVH für viele Lichter
    volume/      NanoVDB-Grids, Delta/Ratio Tracking, Majorant-Grid, Emission (Pyro: Dichte/Temperatur/Flamme)
    integrator/  Wavefront-Path-Tracer (Ray-Queues: Generate → Intersect → Shade → Shadow), MIS, NEE, RR
    film/        AOV-System (LPE-basiert), Pixel-Filter, adaptives Sampling, progressives Akkumulieren
    denoise/     OptiX-Denoiser (Albedo/Normal-Guides, HDR, temporal optional) über CUDA-Interop
  hydra/     (hdNova Plugin: HdRenderDelegate, HdRenderPass, HdRenderBuffer, Mesh/BasisCurves/Points/Volume/Instancer/Material/Light/Camera Sprims+Rprims)
    plugInfo.json
  houdini/   (HDK-abhängig)
    cvex_bridge/  führt VEX-Teilgraphen per CVEX_Context batchweise aus (Fallback-Shader-Node)
    package/      nova.json (Houdini-Package), UsdRenderers.json (Delegate-Registrierung, Render-Settings, AOV-Menüs)
    otls/         Nova Render Settings LOP, Nova Material Builder + Shader-Nodes (MaterialX-basiert), nova_standard_surface
  tools/     nova_cli (rendert .usd standalone → EXR, für Tests/CI)
  tests/     Unit-Tests (Catch2), Furnace-/White-Furnace-Tests, Referenzbild-Regressionen
```

**Leitprinzipien für die GPU-Fähigkeit:** keine virtuellen Aufrufe im Hot Path, Daten in SoA-Arrays, Shading als interpretierter/kompilierter Bytecode statt C++-Polymorphie, Wavefront-Integrator (gleiche Struktur wie später in CUDA). Gemeinsame Kernel-Header (`NOVA_HOST_DEVICE`) für CPU und CUDA.

## Material-Pipeline
1. Ein Hydra-`HdMaterialNetwork2` kommt im Delegate an.
2. Ein Übersetzer (`hydra/material/`) mappt:
   - `ND_standard_surface_*`, `ND_open_pbr_surface_*`, MaterialX-Standardnodes (Mix, Noise, Image, Ramp, Math …) → Nova-ShaderGraph-IR
   - `nova_standard_surface` und die Nova-Nodes → nativ
   - Karma-/Houdini-VOPs mit MaterialX-Äquivalent (`mtlxstandard_surface`, `kma_*`-Grundnodes) → Mapping-Tabelle
   - alles Unbekannte, z. B. VEX-Snippets und Custom-VOPs → `CvexNode` (Houdini-Bridge, nur CPU; im GPU-Modus wird eine Warnung ausgegeben und ein Fallback-Wert genutzt)
3. Die IR wird optimiert (Constant Folding, Dead Code) und in ein Bytecode-Programm übersetzt, das pro Shading-Batch ausgeführt wird.
4. Der **Nova Material Builder** ist ein Subnet-HDA (wie der Karma Material Builder) mit MaterialX-Nodes und Nova-Erweiterungen (Uber-Shader, Ramp, Triplanar, AO, Curvature, RaySwitch, UserData-Lookup). Er exportiert MaterialX-konforme USD-Shader, dadurch bleibt das Material auch in Karma ansehbar.

## Meilensteine (jeweils lauffähig und getestet)

| # | Inhalt | Ergebnis |
|---|--------|----------|
| M0 | CMake-Setup gegen Houdini 21 (`$HFS/toolkit`), VS2022, vcpkg nur für Embree/Catch2; OptiX SDK + CUDA finden | `nova_core` und `hdNova.dll` bauen, das Package lädt in Houdini |
| M1 | Minimaler Delegate: Mesh + Kamera + Dome/Distant-Light, Embree, Lambert, progressiver Render-Buffer | „Nova“ ist im Solaris-Viewport wählbar und zeigt ein Bild |
| M2 | Robuster Path Tracer: MIS/NEE, alle Lichttypen, Sobol, Pixel-Filter, Instancing, Subdiv (OpenSubdiv), Motion Blur (Transform + Deform), DOF | Korrekte Referenzbilder, Furnace-Tests grün |
| M3 | Shading-Kern: ShaderGraph-IR, Bytecode-VM, OpenPBR/Standard Surface komplett, Texturen (OIIO TextureSystem, UDIM, tx-Mipmaps) | MaterialX-Standard-Surface aus Solaris rendert korrekt |
| M4 | Material-Übersetzung + **Nova Material Builder** HDAs + `UsdRenderers.json`-Integration (Render-Settings-LOP, Parameter-UI) | Arnold-ähnlicher Material-Workflow in Houdini |
| M5 | **CVEX-Bridge** für VEX-Fallback (batched, thread-local Kontexte) | VEX-Material-Builder-Materialien rendern |
| M6 | AOVs: LPE (OSL-LPE-Syntax), Beauty-Komponenten, Utility (P, N, Z, albedo, crypto-matte), Multi-Part EXR via husk | Comp-fertige Ausgabe |
| M7 | **OptiX-Denoiser** (Viewport-IPR + Final), adaptives Sampling, IPR-Performance (Delta-Updates, schneller Neustart) | Interaktives, rauschfreies Look-Dev |
| M8 | Volumen: NanoVDB aus Houdini-VDB-Prims, heterogen, Emission/Blackbody, Volume-Light-Sampling | Pyro-Shots renderbar |
| M9 | Haare/Kurven (Embree-Curves, Chiang-Hair-BSDF), SSS Random Walk, Light-BVH, Light-Linking | Feature-Parität mit dem Arnold-Kern |
| M10 | GPU-Backend: CUDA-Wavefront-Kernels + OptiX-Accel, gleicher Bytecode-Interpreter auf der GPU | XPU-artiger Modus |

## Abhängigkeiten
Wo möglich werden **die von Houdini mitgelieferten Bibliotheken** genutzt, um ABI-Konflikte zu vermeiden: USD/Hydra, TBB, OpenImageIO, OpenEXR, OpenVDB (+NanoVDB), MaterialX, OpenSubdiv.
Extern: Embree 4, NVIDIA OptiX SDK 8+ und CUDA 12 (Denoiser, später GPU), Catch2.

## Verifikation
- **Unit-Tests** (`ctest`): Sampling-Verteilungen, BSDF-Energieerhaltung (White Furnace), Reziprozität, PDF-Konsistenz (Chi²-Tests)
- **Referenz-Szenen** in `tests/scenes/*.usda` (Cornell Box, Material-Sphären, Pyro-VDB, Instancing) werden mit `nova_cli` und `husk --renderer Nova` gerendert und per FLIP/RMSE gegen Referenzbilder geprüft. Zum Abgleich der Materialien dient Karma CPU.
- **In Houdini** über den fxhoudini-MCP: Testszene in LOPs aufbauen, Viewport auf Nova schalten, Screenshot erstellen, AOVs prüfen
- **Performance**: Rays/s-Benchmark pro Meilenstein, IPR-Latenz < 100 ms beim Parameterwechsel

## Erster Umsetzungsschritt nach Freigabe
M0 + M1: Projektgerüst, CMake gegen `$HFS`, Minimal-Delegate mit Embree und Lambert, Houdini-Package. Danach folgt eine kurze Prüfung in Solaris, bevor M2 beginnt.
