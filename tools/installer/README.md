# Nova Setup

The installer of Nova for Houdini: one executable, `dist/NovaSetup-<version>.exe`, with Nova's package
(`dist/nova`) inside.

## What it does

- Installs per user, without administrator rights, to `%LOCALAPPDATA%\Programs\Nova` by default (any folder can
  be chosen): the package goes to `<folder>\nova`, the installer copies itself as `<folder>\Uninstall Nova.exe`.
  A folder that holds a `nova` folder or an `install.json` that is not Nova's (no Nova manifest) is refused, so
  nothing of the user's is replaced. Installing to another folder than the registered install removes that
  install (its `nova` folder, manifest, uninstaller and package files; the folder only when then empty).
- Uninstalling removes only what Nova put there; the folder goes only when it is then empty (never recursively,
  so the user's own files in a chosen folder stay). The running uninstaller is deleted by a hidden PowerShell
  that waits for it to exit.
- Registers Nova with Houdini 22.0 (the version hdNova.dll is built for): `Documents\houdini22.0\packages\nova.json`
  points Houdini at the package. A `nova.json` that was there and points elsewhere is kept as `nova.json.bak` and
  returns when Nova is uninstalled.
- Adds "Nova for Houdini" to Apps & features (`HKCU\...\Uninstall\NovaForHoudini`) with the uninstaller.
- Refuses to replace Nova's files while Houdini, hython, husk or hbatch runs (Windows locks loaded DLLs).
- Shows the Houdini installations and the graphics card it finds (Nova XPU needs an NVIDIA GPU; without one it
  renders on the CPU).

## The window

A frameless window (Tauri 2, WebView2) with its own title bar and three pages: install (location, Houdini,
graphics), progress, done; the uninstaller shows remove, progress, done. The colours of the Nova logo: off-white,
near-black text and buttons, its orange only in small accents (progress bar, focus, check mark, the dot of the mark);
no motion. The interface is `ui/` (HTML, CSS, JavaScript); `src/main.rs` is the backend. Opened in a browser
(`python -m http.server --directory tools/installer/ui`), the interface runs against a mock backend:
`?installed`, `?uninstall`, `?running`, `?notnova`, `?nogpu`, `?other` show its states.

## Command line

```
NovaSetup-0.1.2.exe --silent [--dir <folder>] [--packages-root <folder>] [--no-register]
"Uninstall Nova.exe" --uninstall [--silent]
```

`--packages-root` replaces `Documents` as the parent of `houdini22.0\packages` (tests, studio setups),
`--no-register` leaves Apps & features alone (and any registered install). Relative folders are made absolute
against the current directory. Exit code 0 on success.

Which install `--uninstall` removes: the one in `--dir`; without it, the install the executable sits in (an
`Uninstall Nova.exe` removes its own install, registered or not); an executable outside any install (the setup
run with `--uninstall`) removes the registered one. In the 0.1.0 installer the registered install came before the
executable's own, so the uninstaller of a second, unregistered install removed the registered install instead.

## Building

Needs the Rust MSVC toolchain (`rustup`), WebView2 (part of Windows 11) and an installed package in `dist/nova`:

```
cmake --install build/vs2022 --config Release
powershell -ExecutionPolicy Bypass -File tools\installer\build.ps1
```

`build.ps1` zips `dist/nova` into `payload/nova.zip`, takes the version from `CMakeLists.txt` and writes
`dist/NovaSetup-<version>.exe`. The icon is `icons/icon.svg` (the O of the Nova logo); `make_icon.py` (Houdini's
Python) renders it into `icons/icon.ico` and `icons/preview.png`.
