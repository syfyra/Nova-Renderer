<p align="center">
  <img src="houdini/package/config/Icons/NOVA_logo.svg" width="112" alt="Nova logo">
</p>

<h1 align="center">Nova</h1>

<p align="center">
  A production path tracer for Houdini Solaris, on the CPU and on NVIDIA GPUs.<br>
  A Hydra render delegate that renders Karma scenes the way Karma does.
</p>

<p align="center">
  <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-blue.svg">
  <img alt="Houdini 22.0" src="https://img.shields.io/badge/Houdini-22.0-orange.svg">
  <img alt="Platform: Windows x64" src="https://img.shields.io/badge/platform-Windows%20x64-lightgrey.svg">
  <img alt="Version 0.1.5" src="https://img.shields.io/badge/version-0.1.5-informational.svg">
</p>

---

Nova is a physically based path tracer that plugs into Houdini's Solaris as a Hydra render delegate (`hdNova`).
Houdini sees it as two renderers, both in the viewport's renderer menu and in husk:

- **Nova** renders on the CPU: Embree 4, a wavefront path tracer and a SIMD shading VM.
- **Nova XPU** renders on NVIDIA GPUs with CUDA wavefront kernels and OptiX 9. It runs the same code with the same
  random numbers, so its images match the CPU's up to floating-point rounding. Without a usable GPU it falls back to
  the CPU and says so.

Nova speaks Karma's language. Scenes, materials, lights, render settings and AOVs set up for Karma render in Nova
with the same look, and every feature is measured against Karma CPU or an analytic value.

## Highlights

- **Karma's Solaris nodes.** All twelve visible Karma LOPs work:
  - Render Settings;
  - Render Products, Standard Render Vars and Cryptomatte;
  - Sky Dome Light, Physical Sky and Sky Atmosphere;
  - Shadow Catcher and Background Plate;
  - Blocker Light Filter and Fog Box;
  - Texture Baker.

  Nova's own nodes (Nova, Nova Render Settings, Nova (Setup), Nova Material Builder) mirror Karma's.
- **Materials.**
  - MaterialX 1.39: Standard Surface, OpenPBR, UsdPreviewSurface and any pbrlib graph.
  - The Karma Material Builder's `kma_*` nodes.
  - Houdini's Principled Shader, rendered natively.
  - VEX Material Builders, through a CVEX bridge.

  Displacement and textures (UDIMs, OpenImageIO, Karma's color-space rules) are supported too.
- **Lights.**
  - Every UsdLux light, with Karma's conventions.
  - A light tree for scenes with many lights.
  - Emissive meshes and volumes as lights.
  - Light and shadow linking, portals, Karma's sky model and atmosphere.
- **Volumes.** VDB and native Houdini volumes through NanoVDB. Unbiased spectral tracking uses majorants derived
  from the volume shader. The Karma Pyro Material is supported, as is velocity blur.
- **Hair, curves, points and subsurface scattering.** Hair uses the Chiang fibre model. Random-walk SSS matches
  Karma's calibration scenes within 0.7 %.
- **Output.** Nova speaks Karma's AOV vocabulary:
  - light path expressions and utility AOVs;
  - Cryptomatte with Karma-identical manifests;
  - multi-part and deep EXR;
  - per-var sample filters and checkpoints.
- **Look-dev and IPR.**
  - OptiX denoiser and adaptive sampling.
  - Progressive proxy passes after edits.
  - Outlines, look-dev overrides, path guiding.
- **Reproducible.** The same scene renders the same image, run to run and on CPU and GPU alike.

## Status

Version 0.1.5, for **Windows x64** and **Houdini 22.0**. `hdNova` is built against the USD of Houdini 22.0.466.
Nova was built milestone by milestone. The plan's milestones M0 to M10 ([`docs/PLAN.md`](docs/PLAN.md)) and the
Karma-compatibility milestone M11 ([`docs/design/M11.md`](docs/design/M11.md)) are complete, with a design note per
milestone in [`docs/design/`](docs/design/). About 690 automated tests cover it: unit
tests, husk and command-line renders, statistical comparisons against Karma references, and isolated Houdini
sessions. The current limitations are listed in [`docs/limitations.md`](docs/limitations.md); known differences
from Karma are documented in the design notes and in [`docs/bug-audit-2026-11.md`](docs/bug-audit-2026-11.md).

The renderer core (`nova_core`) and its unit tests also build on Linux without Houdini
([Core only, without Houdini](#core-only-without-houdini)); the Hydra delegate and Nova XPU do not yet.

Requirements at run time:

- A CPU with AVX2.
- For Nova XPU, an NVIDIA GPU with driver 590 or newer. It is developed and tested on an RTX 5080: the RTX 50
  series renders on the GPU as installed. The RTX 20, 30 and 40 and the GTX 16 series (compute capability 7.5
  and up) are **experimental**: Nova XPU is built for them and that code is checked on the RTX 5080, but it has
  not run on such a card yet. The installer offers to turn it on for such a card (`NOVA_XPU_EXPERIMENTAL_GPUS=1`
  does the same); otherwise Nova XPU renders on the CPU there ([`docs/limitations.md`](docs/limitations.md)).
  Other GPUs (AMD, Intel, older NVIDIA) and machines without one render on the CPU.

## Quick start

1. Install Nova: build and run the installer (see below), or register the package by hand
   (see [`docs/reference.md`](docs/reference.md#use-in-houdini)).
2. **Solaris:** choose **Nova** or **Nova XPU** in the viewport's renderer menu.
3. **Render setup:** TAB menu → Rendering → **Nova (Setup)** builds Nova Render Settings, a Render Pass and a USD
   Render ROP, just as Karma (Setup) does. The all-in-one **Nova** LOP works like the Karma LOP. Existing Karma
   setups render as they are.
4. **husk:**

   ```bat
   husk --renderer Nova scene.usda -o out.exr
   husk --renderer "Nova XPU" scene.usda -o out.exr
   ```

5. **Without Houdini's UI or a license:** `nova_cli scene.usda -o out.exr` reads the render settings, products and
   render vars as husk does.

## Build from source

Requirements:

- Houdini 22.0. USD/Hydra, TBB, OpenImageIO, OpenSubdiv, MaterialX and Python come from `$HFS`.
- Visual Studio 2022 Build Tools (MSVC v143, "Desktop development with C++"), which include CMake ≥ 3.26 and Ninja.
- vcpkg in `%USERPROFILE%\vcpkg` or at `%VCPKG_ROOT%`. It provides only Embree 4 and Catch2 (`vcpkg.json`).
- Optional, for Nova XPU and the OptiX denoiser: CUDA 12.8 or later.
  - The OptiX headers come from an installed OptiX SDK or are downloaded from
    [NVIDIA/optix-dev](https://github.com/NVIDIA/optix-dev). `-DNOVA_FETCH_OPTIX=OFF` skips the download.
  - The CUDA driver and OptiX are loaded at run time, so a build also runs on machines without an NVIDIA GPU.

```bat
cmake --preset vs2022
cmake --build --preset release
ctest --preset release
cmake --install build/vs2022 --config Release
```

- **First configure:** it builds Embree through vcpkg, which takes about 20 minutes. Later configures restore it
  from vcpkg's binary cache.
- **Configurations:** only Release and RelWithDebInfo exist, because Houdini ships release libraries only.
- **Nova XPU's kernels** are compiled for three GPU architectures (`NOVA_GPU_ARCHS`, default `75;80;120`: RTX 20 and
  GTX 16, RTX 30 and 40, RTX 50), which takes about 8 minutes whenever kernel-side code changes.
  `-DNOVA_GPU_ARCHS=120` builds for the RTX 50 series alone in about half the time; that is a development build,
  and `tools\installer\build.ps1` refuses to package it.
- **Install step:** it writes the Houdini package to `dist/` (`dist/nova.json` and `dist/nova/`).

**Installer.** `tools\installer\build.ps1` packs `dist/nova` into `dist/NovaSetup-0.1.5.exe`; it needs Rust with the
MSVC toolchain.
- The installer installs Nova per user, without administrator rights, and registers it with Houdini 22.0.
- It adds an uninstaller to Apps & features.
- It refuses to replace Nova's files while Houdini runs.

See [`tools/installer/README.md`](tools/installer/README.md).

### Core only, without Houdini

`nova_core`, its Catch2 unit tests (`nova_tests`) and `nova_bsdf_precompute` build against system libraries, with
GCC on Linux; neither Houdini, Windows nor vcpkg is needed. This is the default configuration away from Windows
(`-DNOVA_CORE_ONLY=ON`, [`cmake/NovaStandalone.cmake`](cmake/NovaStandalone.cmake)). On Ubuntu 24.04:

```sh
sudo apt install cmake ninja-build g++ libembree-dev libtbb-dev libosd-dev catch2
cmake --preset core
cmake --build --preset core
ctest --preset core
```

- **Libraries:** Embree 4, oneTBB, OpenSubdiv and Catch2 3 from the system. The NanoVDB 32.6 headers (the version
  Houdini 22.0 ships) are fetched from the OpenVDB repository at configure time; `-DNOVA_NANOVDB_INCLUDE_DIR=<dir>`
  names existing ones and `-DNOVA_FETCH_NANOVDB=OFF` turns the download off.
- **Not built:** the Hydra delegate, the Houdini package, `nova_cli`, `nova_imgcmp` and the GPU backend. Their
  tests, and the unit tests that need a GPU, do not run.
- **Sanitizers:** `cmake --preset core-sanitize` builds the same targets with AddressSanitizer and UBSan (float
  casts included; a finding stops the test).
- **Continuous integration:** [`.github/workflows/core.yml`](.github/workflows/core.yml) runs both presets on every
  pull request.

### Tests

The husk, reference and Houdini tests run Houdini in an isolated environment with an empty preference
directory, so installed packages never interfere and a running Houdini session is never touched. Regenerating the
Karma reference images needs a Karma license. See [`docs/reference.md`](docs/reference.md#tests) and
[`tests/reference/README.md`](tests/reference/README.md).

## Repository layout

| Directory | Contents |
|-----------|----------|
| `core/` | `nova_core`, the renderer, with no HDK or USD dependency: math, scene, Embree acceleration, BSDFs and surface models, the shading compiler and VM, lights and the light tree, volumes, the wavefront integrator, film and AOVs, baking |
| `core/gpu/` | Nova XPU: CUDA wavefront kernels, OptiX acceleration structures and programs, the device scene and film |
| `hydra/` | `hdNova.dll`, the Hydra render delegate; MaterialX network translation; the OpenImageIO texture system |
| `houdini/` | The Houdini package: `nova.json`, `UsdRenderers.json`, parameter files, the HDA build, shelf tools; the CVEX bridge (`nova_cvex.dll`) for VEX materials and the volume bridge (`nova_volume.dll`) for Houdini volumes |
| `tools/` | `nova_cli` (standalone renderer), `nova_imgcmp` (statistical image comparison), `installer/` (Nova Setup, Rust and Tauri) |
| `tests/` | Catch2 unit tests, husk and CLI test scenes, the Karma reference scenes and images, Houdini UI tests |
| `docs/` | The plan, design notes per milestone, research notes, benchmarks, the bug audit, the [reference](docs/reference.md) |
| `third_party/` | NVIDIA FLIP (header only) and the Hošek–Wilkie sky model data |
| `cmake/` | Where the libraries come from: Houdini (`NovaHoudini.cmake`), CUDA and OptiX (`NovaOptiX.cmake`), or the system in a core-only build (`NovaStandalone.cmake`) |
| `.github/workflows/` | Continuous integration: the core-only build and its unit tests on Linux |

## Documentation

- [`docs/reference.md`](docs/reference.md): using Nova in Houdini, every setting and per-object property, the
  features of each milestone with their measured differences to Karma, tools and tests.
- [`docs/design/`](docs/design/): design notes per milestone. M11 covers Karma's Solaris nodes, one note per package.
- [`docs/benchmarks.md`](docs/benchmarks.md): performance measurements, CPU and GPU.
- [`docs/limitations.md`](docs/limitations.md): the current limitations in one place.
- [`docs/bug-audit-2026-11.md`](docs/bug-audit-2026-11.md): audits, fixes and the limitations they left.
- [`docs/PLAN.md`](docs/PLAN.md): the original milestone plan, in German.

## License

Nova is released under the [MIT License](LICENSE).

Third-party code in this repository:

- [NVIDIA FLIP](third_party/flip/LICENSE), BSD-3-Clause.
- The [Hošek–Wilkie sky model data](third_party/hosek_wilkie/LICENSE), BSD-3-Clause.

Libraries used by the build:

- Embree 4, Apache-2.0, from vcpkg, linked statically.
- Houdini's own USD, TBB, OpenImageIO, OpenSubdiv and MaterialX, loaded from the Houdini installation.
- For Nova XPU, NVIDIA's CUDA driver API and OptiX, loaded at run time.

Houdini, Solaris and Karma are trademarks of Side Effects Software Inc. NVIDIA, CUDA and OptiX are trademarks of
NVIDIA Corporation. Nova is an independent project and is not affiliated with or endorsed by SideFX or NVIDIA.
