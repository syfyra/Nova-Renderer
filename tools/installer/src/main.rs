// Nova for Houdini: the installer (tools/installer/README.md). A frameless
// window with its own interface (ui/) over a small backend: it unpacks the
// embedded package (dist/nova, zipped by build.ps1) into the install folder,
// writes a Houdini package file per chosen Houdini version, copies itself as
// the uninstaller and registers it under Apps & features (per user, no
// administrator rights). The same executable installs and uninstalls from
// the command line with --silent.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io::{Cursor, Write};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use tauri::{Emitter, Manager};
use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
use winreg::RegKey;

const PAYLOAD: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/payload/nova.zip"));
const VERSION: &str = match option_env!("NOVA_VERSION") {
    Some(v) => v,
    None => env!("CARGO_PKG_VERSION"),
};
// The Houdini versions the package's hdNova.dll is built for (its USD ABI).
const HOUDINI_MINOR: &str = "22.0";
const UNINSTALL_KEY: &str = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\NovaForHoudini";
const UNINSTALLER: &str = "Uninstall Nova.exe";
const MANIFEST: &str = "install.json";

#[derive(Serialize, Clone)]
struct Houdini {
    version: String,
    path: String,
    supported: bool,
    minor: String,
    packages: String,
}

#[derive(Serialize, Clone)]
struct Gpu {
    name: String,
    nvidia: bool,
    memory_gb: f64,
}

// What an installation left behind (its manifest, install.json).
#[derive(Serialize, Deserialize, Clone)]
struct Installed {
    dir: String,
    version: String,
    packages: Vec<String>,
}

#[derive(Serialize)]
struct SystemInfo {
    version: String,
    houdini_minor: String,
    houdinis: Vec<Houdini>,
    gpus: Vec<Gpu>,
    default_dir: String,
    installed: Option<Installed>,
    uninstall: bool,
    payload_mb: f64,
}

#[derive(Serialize, Clone)]
struct Progress {
    fraction: f64,
    text: String,
}

// Where the run puts things: the Houdini preference root (Documents, or
// --packages-root), whether it registers the uninstaller, and the mode.
struct Options {
    packages_root: PathBuf,
    register: bool,
    uninstall: bool,
    dir: Option<PathBuf>,
}

fn forward(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

// Typed folders end up in nova.json and the registry, which Houdini and Apps &
// features resolve against their own working directory: made absolute where
// they enter. Not canonicalize, whose \\?\ prefix would land in those files.
fn absolute(p: PathBuf) -> PathBuf {
    std::path::absolute(&p).unwrap_or(p)
}

// Two spellings of one folder (Windows paths ignore case).
fn same_dir(a: &Path, b: &Path) -> bool {
    match (fs::canonicalize(a), fs::canonicalize(b)) {
        (Ok(x), Ok(y)) => x == y,
        _ => {
            let key = |p: &Path| forward(&absolute(p.to_path_buf())).trim_end_matches('/').to_lowercase();
            key(a) == key(b)
        }
    }
}

// `a` is `b` or a folder inside it (Windows paths ignore case; `a` need not
// exist yet).
fn is_within(a: &Path, b: &Path) -> bool {
    let key = |p: &Path| forward(&absolute(p.to_path_buf())).trim_end_matches('/').to_lowercase();
    let (a, b) = (key(a), key(b));
    a == b || a.starts_with(&(b + "/"))
}

// Nova installs into a new folder or over its own install (its manifest is
// there). A nova folder or install.json without Nova's manifest is the user's
// and is never replaced.
fn check_folder(dir: &Path) -> Result<(), String> {
    if read_manifest(dir).is_none() && (dir.join("nova").exists() || dir.join(MANIFEST).exists()) {
        return Err(format!("NOT_NOVA:{}", dir.display()));
    }
    Ok(())
}

// Removes the package files that still point at `nova`; one kept as
// nova.json.bak when Nova was installed returns.
fn remove_packages(packages: &[String], nova: &Path) {
    for p in packages {
        let file = PathBuf::from(p);
        if fs::read_to_string(&file).map_or(false, |t| t.contains(&forward(nova))) {
            let _ = fs::remove_file(&file);
            if let Some(folder) = file.parent() {
                let bak = folder.join("nova.json.bak");
                if bak.exists() {
                    let _ = fs::rename(&bak, &file);
                }
            }
        }
    }
}

fn is_running_exe(file: &Path) -> bool {
    let me = std::env::current_exe().ok().and_then(|p| fs::canonicalize(p).ok());
    me.is_some() && me == fs::canonicalize(file).ok()
}

// The running uninstaller cannot delete its own file: a hidden PowerShell
// waits for process `pid` to exit (the window stays open until Close),
// retries the delete while the image is still mapped, then removes `dir` only
// if it is empty (never recursively: the folder may hold the user's own
// files). The paths go through the environment, so no quoting can break them.
fn cleanup_command(pid: u32, exe: &Path, dir: &Path) -> std::process::Command {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    const SCRIPT: &str = "Wait-Process -Id ([int]$env:NOVA_CLEANUP_PID) -ErrorAction SilentlyContinue; \
        for ($i = 0; $i -lt 60 -and [IO.File]::Exists($env:NOVA_CLEANUP_EXE); $i++) { \
        try { [IO.File]::Delete($env:NOVA_CLEANUP_EXE) } catch { Start-Sleep -Milliseconds 500 } }; \
        if (-not [IO.File]::Exists([IO.Path]::Combine($env:NOVA_CLEANUP_DIR, 'install.json'))) { \
        try { [IO.Directory]::Delete($env:NOVA_CLEANUP_DIR) } catch { } }";
    let powershell = std::env::var_os("SystemRoot")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\Windows"))
        .join(r"System32\WindowsPowerShell\v1.0\powershell.exe");
    let mut cmd = std::process::Command::new(powershell);
    cmd.args(["-NoProfile", "-NonInteractive", "-Command", SCRIPT])
        .env("NOVA_CLEANUP_PID", pid.to_string())
        .env("NOVA_CLEANUP_EXE", exe)
        .env("NOVA_CLEANUP_DIR", dir)
        // Not the install folder: a working directory there would keep it.
        .current_dir(std::env::temp_dir())
        .creation_flags(CREATE_NO_WINDOW)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    cmd
}

// Houdini installations: the registry's and the default install folder's.
fn find_houdinis(packages_root: &Path) -> Vec<Houdini> {
    let mut found: BTreeMap<String, String> = BTreeMap::new();
    if let Ok(sesi) = RegKey::predef(HKEY_LOCAL_MACHINE).open_subkey(r"SOFTWARE\Side Effects Software") {
        for name in sesi.enum_keys().flatten() {
            if let Some(v) = name.strip_prefix("Houdini ") {
                if let Ok(key) = sesi.open_subkey(&name) {
                    if let Ok(path) = key.get_value::<String, _>("InstallPath") {
                        found.insert(v.to_string(), path);
                    }
                }
            }
        }
    }
    let program_files = std::env::var("ProgramFiles").unwrap_or_else(|_| r"C:\Program Files".into());
    if let Ok(entries) = fs::read_dir(Path::new(&program_files).join("Side Effects Software")) {
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().to_string();
            if let Some(v) = name.strip_prefix("Houdini ") {
                if e.path().join("bin").join("houdini.exe").exists() || e.path().join("bin").join("hython.exe").exists() {
                    found.entry(v.to_string()).or_insert_with(|| e.path().to_string_lossy().to_string());
                }
            }
        }
    }
    let mut list: Vec<Houdini> = found
        .into_iter()
        .filter(|(v, _)| v.split('.').count() == 3 && v.chars().next().map_or(false, |c| c.is_ascii_digit()))
        .map(|(version, path)| {
            let minor = version.split('.').take(2).collect::<Vec<_>>().join(".");
            let packages = packages_root.join(format!("houdini{minor}")).join("packages");
            Houdini {
                supported: minor == HOUDINI_MINOR,
                packages: packages.to_string_lossy().to_string(),
                version,
                path,
                minor,
            }
        })
        .collect();
    list.sort_by(|a, b| {
        let key = |h: &Houdini| h.version.split('.').map(|p| p.parse::<u32>().unwrap_or(0)).collect::<Vec<_>>();
        key(b).cmp(&key(a))
    });
    list
}

fn find_gpus() -> Vec<Gpu> {
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1, DXGI_ADAPTER_FLAG_SOFTWARE};
    let mut out = Vec::new();
    unsafe {
        let Ok(factory) = CreateDXGIFactory1::<IDXGIFactory1>() else {
            return out;
        };
        let mut i = 0;
        while let Ok(adapter) = factory.EnumAdapters1(i) {
            i += 1;
            let Ok(desc) = adapter.GetDesc1() else { continue };
            if desc.Flags & (DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32) != 0 {
                continue;
            }
            let len = desc.Description.iter().position(|&c| c == 0).unwrap_or(desc.Description.len());
            out.push(Gpu {
                name: String::from_utf16_lossy(&desc.Description[..len]).trim().to_string(),
                nvidia: desc.VendorId == 0x10DE,
                memory_gb: desc.DedicatedVideoMemory as f64 / 1_073_741_824.0,
            });
        }
    }
    out
}

// Houdini processes that may have Nova's DLLs loaded (they cannot be replaced
// while loaded).
fn running_houdini() -> Vec<String> {
    use windows::Win32::Foundation::CloseHandle;
    use windows::Win32::System::Diagnostics::ToolHelp::{
        CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
    };
    const NAMES: [&str; 10] = [
        "houdini.exe", "houdinifx.exe", "houdinicore.exe", "happrentice.exe", "hindie.exe", "hescape.exe",
        "hython.exe", "husk.exe", "hbatch.exe", "mplay.exe",
    ];
    let mut out = Vec::new();
    unsafe {
        let Ok(snap) = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) else {
            return out;
        };
        let mut e = PROCESSENTRY32W { dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32, ..Default::default() };
        if Process32FirstW(snap, &mut e).is_ok() {
            loop {
                let len = e.szExeFile.iter().position(|&c| c == 0).unwrap_or(e.szExeFile.len());
                let name = String::from_utf16_lossy(&e.szExeFile[..len]);
                if NAMES.contains(&name.to_lowercase().as_str()) && !out.contains(&name) {
                    out.push(name);
                }
                if Process32NextW(snap, &mut e).is_err() {
                    break;
                }
            }
        }
        let _ = CloseHandle(snap);
    }
    out
}

fn registered() -> Option<Installed> {
    registered_at().map(|(_, i)| i)
}

// Whether the Apps & features entry is this folder's install (or names
// none, or an install that is gone): uninstalling another, unregistered
// install must not orphan the registered one.
fn owns_registration(dir: &Path) -> bool {
    let Ok(key) = RegKey::predef(HKEY_CURRENT_USER).open_subkey(UNINSTALL_KEY) else { return false };
    match key.get_value::<String, _>("InstallLocation") {
        Ok(loc) => same_dir(Path::new(&loc), dir) || read_manifest(Path::new(&loc)).is_none(),
        Err(_) => true,
    }
}

// The registered folder (where its manifest was read) and that manifest.
fn registered_at() -> Option<(PathBuf, Installed)> {
    let key = RegKey::predef(HKEY_CURRENT_USER).open_subkey(UNINSTALL_KEY).ok()?;
    let dir: String = key.get_value("InstallLocation").ok()?;
    let dir = PathBuf::from(dir);
    read_manifest(&dir).map(|i| (dir, i))
}

fn read_manifest(dir: &Path) -> Option<Installed> {
    serde_json::from_str(&fs::read_to_string(dir.join(MANIFEST)).ok()?).ok()
}

fn default_dir() -> PathBuf {
    dirs::data_local_dir().unwrap_or_else(|| PathBuf::from(r"C:\Nova")).join("Programs").join("Nova")
}

fn package_json(nova: &Path) -> String {
    format!(
        "{{\n    \"enable\": \"houdini_version >= '{min}' and houdini_version < '{max}'\",\n    \"load_package_once\": true,\n    \"env\": [\n        {{ \"NOVA\": \"{dir}\" }}\n    ],\n    \"hpath\": \"$NOVA\"\n}}\n",
        min = HOUDINI_MINOR,
        max = next_minor(HOUDINI_MINOR),
        dir = forward(nova)
    )
}

fn next_minor(minor: &str) -> String {
    let mut parts = minor.split('.');
    let major = parts.next().unwrap_or("0");
    let m: u32 = parts.next().and_then(|p| p.parse().ok()).unwrap_or(0);
    format!("{major}.{}", m + 1)
}

fn dir_size(dir: &Path) -> u64 {
    fs::read_dir(dir)
        .map(|entries| {
            entries
                .flatten()
                .map(|e| match e.metadata() {
                    Ok(m) if m.is_dir() => dir_size(&e.path()),
                    Ok(m) => m.len(),
                    Err(_) => 0,
                })
                .sum()
        })
        .unwrap_or(0)
}

fn install(opts: &Options, dir: &Path, minors: &[String], progress: &dyn Fn(f64, &str)) -> Result<Installed, String> {
    let running = running_houdini();
    if !running.is_empty() {
        return Err(format!("HOUDINI_RUNNING:{}", running.join(", ")));
    }
    let err = |what: &str, e: &dyn std::fmt::Display| format!("{what}: {e}");
    for p in [dir, opts.packages_root.as_path()] {
        if !p.is_absolute() {
            return Err(format!("not an absolute folder: {}", p.display()));
        }
    }
    check_folder(dir)?;
    // Apps & features knows one Nova: an install registered in another folder
    // is replaced (removed below once the new files are in place).
    let previous = if opts.register { registered_at().filter(|(d, _)| !same_dir(d, dir)) } else { None };
    // Inside the previous install's own package folder (e.g. <old>\Nova on
    // a case-insensitive file system): its files could not be removed.
    if let Some((old_dir, _)) = &previous {
        if is_within(dir, &old_dir.join("nova")) {
            return Err(format!("INSIDE_OLD:{}", old_dir.display()));
        }
    }
    progress(0.02, "Preparing the install folder");
    fs::create_dir_all(dir).map_err(|e| err(&format!("cannot create {}", dir.display()), &e))?;
    // Uninstallers renamed for their cleanup that it could not delete.
    if let Ok(entries) = fs::read_dir(dir) {
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().to_string();
            if name.starts_with(UNINSTALLER) && name.ends_with(".del") {
                let _ = fs::remove_file(e.path());
            }
        }
    }
    // A manifest from the start, so that a retry after a failed install
    // recognises its own partial nova folder.
    if read_manifest(dir).is_none() {
        let provisional = Installed { dir: dir.to_string_lossy().to_string(), version: VERSION.to_string(), packages: Vec::new() };
        let text = serde_json::to_string_pretty(&provisional).unwrap_or_default();
        // An earlier uninstaller's cleanup may remove the empty folder just
        // created: once more.
        fs::write(dir.join(MANIFEST), &text)
            .or_else(|_| fs::create_dir_all(dir).and_then(|_| fs::write(dir.join(MANIFEST), &text)))
            .map_err(|e| err("cannot write the install manifest", &e))?;
    }
    let nova = dir.join("nova");
    if nova.exists() {
        progress(0.04, "Removing the previous version");
        fs::remove_dir_all(&nova).map_err(|e| err("cannot remove the previous version", &e))?;
    }

    // The package: entries under nova/.
    let mut archive = zip::ZipArchive::new(Cursor::new(PAYLOAD)).map_err(|e| err("damaged payload", &e))?;
    let total: u64 = (0..archive.len()).filter_map(|i| archive.by_index(i).ok().map(|f| f.size())).sum();
    let mut done = 0u64;
    for i in 0..archive.len() {
        let mut file = archive.by_index(i).map_err(|e| err("damaged payload", &e))?;
        let Some(rel) = file.enclosed_name() else { continue };
        let out = dir.join(&rel);
        if file.is_dir() {
            fs::create_dir_all(&out).map_err(|e| err("cannot create a folder", &e))?;
            continue;
        }
        if let Some(parent) = out.parent() {
            fs::create_dir_all(parent).map_err(|e| err("cannot create a folder", &e))?;
        }
        let name = rel.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
        progress(0.06 + 0.82 * done as f64 / total.max(1) as f64, &format!("Copying {name}"));
        let mut target = fs::File::create(&out).map_err(|e| err(&format!("cannot write {}", out.display()), &e))?;
        std::io::copy(&mut file, &mut target).map_err(|e| err(&format!("cannot write {}", out.display()), &e))?;
        target.flush().map_err(|e| err("cannot write", &e))?;
        done += file.size();
    }

    // The uninstaller: this executable.
    progress(0.9, "Adding the uninstaller");
    let uninstaller = dir.join(UNINSTALLER);
    if let Ok(me) = std::env::current_exe() {
        if fs::canonicalize(&me).ok() != fs::canonicalize(&uninstaller).ok() {
            fs::copy(&me, &uninstaller).map_err(|e| err("cannot add the uninstaller", &e))?;
        }
    }

    // The install this one replaces: Nova's files there and its package files
    // (a nova.json.bak it kept returns, so the loop below keeps it again).
    // Only what that install created; its folder goes only when empty.
    if let Some((old_dir, old)) = &previous {
        progress(0.92, "Removing the previous version");
        let old_nova = old_dir.join("nova");
        let nested = fs::canonicalize(dir).ok().zip(fs::canonicalize(&old_nova).ok()).map_or(false, |(d, o)| d.starts_with(o));
        if old_nova.exists() && !nested {
            fs::remove_dir_all(&old_nova)
                .map_err(|e| err(&format!("cannot remove the previous version in {}", old_dir.display()), &e))?;
        }
        remove_packages(&old.packages, &old_nova);
        let _ = fs::remove_file(old_dir.join(MANIFEST));
        let old_uninstaller = old_dir.join(UNINSTALLER);
        if !is_running_exe(&old_uninstaller) {
            let _ = fs::remove_file(&old_uninstaller);
        }
        let _ = fs::remove_dir(old_dir);
    }

    // A package file per Houdini version; one already there that points
    // elsewhere is kept as nova.json.bak.
    progress(0.94, "Registering Nova with Houdini");
    let mut packages = Vec::new();
    for minor in minors {
        let folder = opts.packages_root.join(format!("houdini{minor}")).join("packages");
        fs::create_dir_all(&folder).map_err(|e| err(&format!("cannot create {}", folder.display()), &e))?;
        let file = folder.join("nova.json");
        let text = package_json(&nova);
        if let Ok(old) = fs::read_to_string(&file) {
            if !old.contains(&forward(&nova)) && !folder.join("nova.json.bak").exists() {
                fs::rename(&file, folder.join("nova.json.bak")).map_err(|e| err("cannot keep the old package file", &e))?;
            }
        }
        fs::write(&file, text).map_err(|e| err(&format!("cannot write {}", file.display()), &e))?;
        packages.push(file.to_string_lossy().to_string());
    }

    let installed = Installed { dir: dir.to_string_lossy().to_string(), version: VERSION.to_string(), packages };
    fs::write(dir.join(MANIFEST), serde_json::to_string_pretty(&installed).unwrap_or_default())
        .map_err(|e| err("cannot write the install manifest", &e))?;

    if opts.register {
        progress(0.97, "Adding Nova to Apps & features");
        let (key, _) = RegKey::predef(HKEY_CURRENT_USER).create_subkey(UNINSTALL_KEY).map_err(|e| err("registry", &e))?;
        let quoted = format!("\"{}\"", uninstaller.display());
        let kb = (dir_size(dir) / 1024) as u32;
        let values: [(&str, String); 8] = [
            ("DisplayName", "Nova for Houdini".into()),
            ("DisplayVersion", VERSION.into()),
            ("Publisher", "Nova".into()),
            ("InstallLocation", dir.to_string_lossy().to_string()),
            ("DisplayIcon", format!("{},0", uninstaller.display())),
            ("UninstallString", format!("{quoted} --uninstall")),
            ("QuietUninstallString", format!("{quoted} --uninstall --silent")),
            ("Comments", "Production path tracer for Houdini Solaris (Nova CPU and Nova XPU)".into()),
        ];
        for (name, value) in values.iter() {
            key.set_value(name, value).map_err(|e| err("registry", &e))?;
        }
        key.set_value("EstimatedSize", &kb).map_err(|e| err("registry", &e))?;
        key.set_value("NoModify", &1u32).map_err(|e| err("registry", &e))?;
        key.set_value("NoRepair", &1u32).map_err(|e| err("registry", &e))?;
    }
    progress(1.0, "Nova is installed");
    Ok(installed)
}

fn uninstall(opts: &Options, dir: &Path, progress: &dyn Fn(f64, &str)) -> Result<(), String> {
    // Only a folder with Nova's manifest: nothing is removed from any other
    // (a mistyped --dir could name the user's own nova folder).
    let Some(manifest) = read_manifest(dir) else {
        return Err(format!("NOT_NOVA:{}", dir.display()));
    };
    let running = running_houdini();
    if !running.is_empty() {
        return Err(format!("HOUDINI_RUNNING:{}", running.join(", ")));
    }
    let registration = opts.register && owns_registration(dir);
    let nova = dir.join("nova");
    progress(0.1, "Removing Nova from Houdini");
    remove_packages(&manifest.packages, &nova);
    progress(0.4, "Removing the files");
    if nova.exists() {
        fs::remove_dir_all(&nova).map_err(|e| format!("cannot remove {}: {e}", nova.display()))?;
    }
    let _ = fs::remove_file(dir.join(MANIFEST));
    if registration {
        progress(0.8, "Removing Nova from Apps & features");
        let _ = RegKey::predef(HKEY_CURRENT_USER).delete_subkey_all(UNINSTALL_KEY);
    }
    // The uninstaller and then the folder, only if that is empty: the folder
    // may be one the user chose that holds their own files. The running
    // uninstaller is left to a process that waits for this one to exit.
    let uninstaller = dir.join(UNINSTALLER);
    if is_running_exe(&uninstaller) {
        // Renamed first (a running image can be renamed): a reinstall into
        // this folder before the cleanup runs writes an uninstaller the
        // cleanup never touches.
        let doomed = dir.join(format!("{UNINSTALLER}.{}.del", std::process::id()));
        let target = if fs::rename(&uninstaller, &doomed).is_ok() { doomed } else { uninstaller.clone() };
        let _ = cleanup_command(std::process::id(), &target, dir).spawn();
    } else {
        let _ = fs::remove_file(&uninstaller);
        let _ = fs::remove_dir(dir);
    }
    progress(1.0, "Nova is removed");
    Ok(())
}

// The install an uninstall removes: --dir; else the install this executable
// sits in (its manifest is there), registered or not; else the registered
// install (a setup executable run with --uninstall from anywhere else). The
// executable's own install comes before the registered one: the uninstaller of
// a second, unregistered install (--no-register, a studio or test folder)
// must remove that install, never the registered one.
fn choose_uninstall_dir(dir: Option<&Path>, here: Option<&Path>, registered: Option<&Path>) -> Option<PathBuf> {
    if let Some(d) = dir {
        return Some(d.to_path_buf());
    }
    if let Some(h) = here.filter(|h| read_manifest(h).is_some()) {
        return Some(h.to_path_buf());
    }
    registered.map(Path::to_path_buf)
}

fn uninstall_dir(opts: &Options) -> Option<PathBuf> {
    let here = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf));
    let registered = registered().map(|i| PathBuf::from(i.dir));
    choose_uninstall_dir(opts.dir.as_deref(), here.as_deref(), registered.as_deref())
}

// ---- The window's commands ------------------------------------------------

struct State(Mutex<Options>);

#[tauri::command]
fn system_info(state: tauri::State<State>) -> SystemInfo {
    let opts = state.0.lock().unwrap();
    let installed = if opts.uninstall {
        uninstall_dir(&opts).and_then(|d| read_manifest(&d))
    } else {
        opts.dir.as_ref().and_then(|d| read_manifest(d)).or_else(registered)
    };
    SystemInfo {
        version: VERSION.into(),
        houdini_minor: HOUDINI_MINOR.into(),
        houdinis: find_houdinis(&opts.packages_root),
        gpus: find_gpus(),
        default_dir: opts
            .dir
            .clone()
            .or_else(|| installed.as_ref().map(|i| PathBuf::from(&i.dir)))
            .unwrap_or_else(default_dir)
            .to_string_lossy()
            .to_string(),
        installed,
        uninstall: opts.uninstall,
        payload_mb: PAYLOAD.len() as f64 / 1_048_576.0,
    }
}

#[tauri::command]
fn running() -> Vec<String> {
    running_houdini()
}

#[tauri::command]
fn pick_folder(window: tauri::Window, start: String) -> Option<String> {
    let mut dialog = rfd::FileDialog::new().set_title("Install Nova to").set_parent(&window);
    let start = PathBuf::from(start);
    if let Some(existing) = start.ancestors().find(|p| p.is_dir()) {
        dialog = dialog.set_directory(existing);
    }
    let registered = registered_at().map(|(d, _)| d);
    dialog.pick_folder().map(|p| {
        // The registered install's folder (or its nova package folder) is
        // that install: used as is, never nested into.
        if let Some(r) = &registered {
            if same_dir(&p, r) || same_dir(&p, &r.join("nova")) {
                return r.to_string_lossy().to_string();
            }
        }
        // A folder named Nova is used as is; any other gets a Nova folder inside.
        if p.file_name().map_or(false, |n| n.to_string_lossy().eq_ignore_ascii_case("nova")) {
            p.to_string_lossy().to_string()
        } else {
            p.join("Nova").to_string_lossy().to_string()
        }
    })
}

fn emitter(window: tauri::Window) -> impl Fn(f64, &str) {
    move |fraction, text| {
        let _ = window.emit("progress", Progress { fraction, text: text.to_string() });
    }
}

#[tauri::command]
async fn run_install(
    window: tauri::Window,
    state: tauri::State<'_, State>,
    dir: String,
    minors: Vec<String>,
) -> Result<Installed, String> {
    let (root, register) = {
        let o = state.0.lock().unwrap();
        (o.packages_root.clone(), o.register)
    };
    tauri::async_runtime::spawn_blocking(move || {
        let opts = Options { packages_root: root, register, uninstall: false, dir: None };
        let progress = emitter(window);
        install(&opts, &absolute(PathBuf::from(dir.trim())), &minors, &progress)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn run_uninstall(window: tauri::Window, state: tauri::State<'_, State>) -> Result<(), String> {
    let (root, register, dir) = {
        let o = state.0.lock().unwrap();
        (o.packages_root.clone(), o.register, uninstall_dir(&o))
    };
    let dir = dir.ok_or("Nova is not installed")?;
    tauri::async_runtime::spawn_blocking(move || {
        let opts = Options { packages_root: root, register, uninstall: true, dir: None };
        let progress = emitter(window);
        uninstall(&opts, &dir, &progress)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn open_folder(path: String) {
    let _ = std::process::Command::new("explorer").arg(path).spawn();
}

// ---- Entry -------------------------------------------------------------------

fn parse_args() -> (Options, bool) {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let value = |flag: &str| args.iter().position(|a| a == flag).and_then(|i| args.get(i + 1)).cloned();
    let opts = Options {
        packages_root: absolute(
            value("--packages-root").map(PathBuf::from).or_else(dirs::document_dir).unwrap_or_else(|| PathBuf::from(".")),
        ),
        register: !args.iter().any(|a| a == "--no-register"),
        uninstall: args.iter().any(|a| a == "--uninstall"),
        dir: value("--dir").map(|d| absolute(PathBuf::from(d))),
    };
    (opts, args.iter().any(|a| a == "--silent"))
}

// --silent: no window; messages to the console that started it, exit code 0
// on success.
fn silent(opts: Options) -> i32 {
    unsafe {
        let _ = windows::Win32::System::Console::AttachConsole(windows::Win32::System::Console::ATTACH_PARENT_PROCESS);
    }
    let progress = |_: f64, text: &str| println!("{text}");
    let result = if opts.uninstall {
        match uninstall_dir(&opts) {
            Some(dir) => uninstall(&opts, &dir, &progress),
            None => Err("Nova is not installed".into()),
        }
    } else {
        let dir = opts.dir.clone().or_else(|| registered().map(|i| PathBuf::from(i.dir))).unwrap_or_else(default_dir);
        let minors: Vec<String> = {
            let found: Vec<String> =
                find_houdinis(&opts.packages_root).into_iter().filter(|h| h.supported).map(|h| h.minor).collect();
            if found.is_empty() { vec![HOUDINI_MINOR.to_string()] } else { found }
        };
        install(&opts, &dir, &minors, &progress).map(|i| println!("Installed Nova {} to {}", i.version, i.dir))
    };
    match result {
        Ok(()) => 0,
        Err(e) => {
            let e = e.strip_prefix("HOUDINI_RUNNING:").map(|p| format!("close Houdini first ({p} running)")).unwrap_or(e);
            let e = e
                .strip_prefix("NOT_NOVA:")
                .map(|p| {
                    if opts.uninstall {
                        format!("{p} is not a Nova install; nothing was removed")
                    } else {
                        format!("{p} holds a nova folder or {MANIFEST} that is not Nova's; choose another folder")
                    }
                })
                .unwrap_or(e);
            let e = e
                .strip_prefix("INSIDE_OLD:")
                .map(|p| format!("the folder is inside the Nova install in {p}; choose {p} itself or another folder"))
                .unwrap_or(e);
            eprintln!("Nova setup failed: {e}");
            1
        }
    }
}

fn main() {
    let (opts, quiet) = parse_args();
    if quiet {
        std::process::exit(silent(opts));
    }
    tauri::Builder::default()
        .manage(State(Mutex::new(opts)))
        .invoke_handler(tauri::generate_handler![
            system_info,
            running,
            pick_folder,
            run_install,
            run_uninstall,
            open_folder
        ])
        .setup(|app| {
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.set_title(&format!("Nova {VERSION} Setup"));
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("the setup window failed to start");
}

// The file-system helpers, in folders under %TEMP%\nova_fix (no registry, no
// Houdini packages folder is touched).
#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join("nova_fix").join(format!("installer_{}_{name}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn exited_pid() -> u32 {
        let mut c = std::process::Command::new("cmd").args(["/C", "exit"]).spawn().unwrap();
        let pid = c.id();
        c.wait().unwrap();
        pid
    }

    #[test]
    fn paths_are_made_absolute() {
        assert!(absolute(PathBuf::from("Nova")).is_absolute());
        let d = scratch("same");
        let upper = PathBuf::from(d.to_string_lossy().to_uppercase() + "\\");
        assert!(same_dir(&d, &upper));
        assert!(!same_dir(&d, &d.join("other")));
    }

    #[test]
    fn foreign_nova_folder_is_refused() {
        let d = scratch("foreign");
        assert!(check_folder(&d.join("new")).is_ok());
        fs::create_dir_all(d.join("nova").join("src")).unwrap();
        assert_eq!(check_folder(&d), Err(format!("NOT_NOVA:{}", d.display())));
        fs::write(d.join(MANIFEST), "{\"dir\": \"x\", \"version\": \"0.1.0\", \"packages\": []}").unwrap();
        assert!(check_folder(&d).is_ok());
        let e = scratch("foreign_manifest");
        fs::write(e.join(MANIFEST), "{\"name\": \"someone else's\"}").unwrap();
        assert!(check_folder(&e).is_err());
    }

    #[test]
    fn uninstall_refuses_a_folder_without_novas_manifest() {
        let d = scratch("uninstall_foreign");
        fs::create_dir_all(d.join("nova").join("src")).unwrap();
        fs::write(d.join("nova").join("src").join("file"), "mine").unwrap();
        let opts = Options { packages_root: d.clone(), register: false, uninstall: true, dir: Some(d.clone()) };
        assert_eq!(uninstall(&opts, &d, &|_, _| {}), Err(format!("NOT_NOVA:{}", d.display())));
        assert_eq!(fs::read_to_string(d.join("nova").join("src").join("file")).unwrap(), "mine");
    }

    // Without --dir an uninstaller removes the install it sits in; only an
    // executable outside any install falls back to the registered one (before,
    // the registered install came first: the uninstaller of an unregistered
    // copy removed the registered install instead of its own).
    #[test]
    fn uninstall_removes_the_install_the_executable_sits_in() {
        let d = scratch("uninstall_target");
        let manifest = "{\"dir\": \"x\", \"version\": \"0.1.1\", \"packages\": []}";
        let (own, registered, downloads, foreign, chosen) =
            (d.join("own"), d.join("registered"), d.join("downloads"), d.join("foreign"), d.join("chosen"));
        for f in [&own, &registered, &downloads, &foreign] {
            fs::create_dir_all(f).unwrap();
        }
        fs::write(own.join(MANIFEST), manifest).unwrap();
        fs::write(registered.join(MANIFEST), manifest).unwrap();
        fs::write(foreign.join(MANIFEST), "{\"name\": \"someone else's\"}").unwrap();
        // The uninstaller of an unregistered install, another one registered.
        assert_eq!(choose_uninstall_dir(None, Some(&own), Some(&registered)), Some(own.clone()));
        // The registered install's own uninstaller.
        assert_eq!(choose_uninstall_dir(None, Some(&registered), Some(&registered)), Some(registered.clone()));
        // A setup executable outside any install, or beside a manifest that
        // is not Nova's: the registered install, else none.
        assert_eq!(choose_uninstall_dir(None, Some(&downloads), Some(&registered)), Some(registered.clone()));
        assert_eq!(choose_uninstall_dir(None, Some(&foreign), Some(&registered)), Some(registered.clone()));
        assert_eq!(choose_uninstall_dir(None, Some(&downloads), None), None);
        assert_eq!(choose_uninstall_dir(None, None, None), None);
        // --dir decides, wherever the executable is.
        assert_eq!(choose_uninstall_dir(Some(&chosen), Some(&own), Some(&registered)), Some(chosen.clone()));
        // An unregistered install without another one registered.
        assert_eq!(choose_uninstall_dir(None, Some(&own), None), Some(own));
    }

    #[test]
    fn folders_inside_an_old_install_are_recognised() {
        let d = scratch("within");
        let old_nova = d.join("old").join("nova");
        fs::create_dir_all(old_nova.join("otls")).unwrap();
        assert!(is_within(&old_nova, &old_nova));
        assert!(is_within(&d.join("old").join("Nova"), &old_nova));
        assert!(is_within(&old_nova.join("sub"), &old_nova));
        assert!(!is_within(&d.join("old"), &old_nova));
        assert!(!is_within(&d.join("old").join("nova2"), &old_nova));
    }

    #[test]
    fn package_files_of_an_install_are_removed_and_backups_return() {
        let d = scratch("packages");
        let nova = d.join("A").join("nova");
        let ours = d.join("p1").join("nova.json");
        let restored = d.join("p2").join("nova.json");
        let foreign = d.join("p3").join("nova.json");
        for f in [&ours, &restored, &foreign] {
            fs::create_dir_all(f.parent().unwrap()).unwrap();
        }
        fs::write(&ours, package_json(&nova)).unwrap();
        fs::write(&restored, package_json(&nova)).unwrap();
        fs::write(restored.with_file_name("nova.json.bak"), "theirs").unwrap();
        fs::write(&foreign, "theirs").unwrap();
        let list: Vec<String> = [&ours, &restored, &foreign].iter().map(|f| f.to_string_lossy().to_string()).collect();
        remove_packages(&list, &nova);
        assert!(!ours.exists());
        assert_eq!(fs::read_to_string(&restored).unwrap(), "theirs");
        assert_eq!(fs::read_to_string(&foreign).unwrap(), "theirs");
    }

    #[test]
    fn cleanup_keeps_a_folder_with_user_files() {
        let d = scratch("cleanup_user");
        let exe = d.join(UNINSTALLER);
        fs::write(&exe, "exe").unwrap();
        fs::write(d.join("notes.txt"), "mine").unwrap();
        cleanup_command(exited_pid(), &exe, &d).status().unwrap();
        assert!(!exe.exists());
        assert_eq!(fs::read_to_string(d.join("notes.txt")).unwrap(), "mine");
    }

    #[test]
    fn cleanup_waits_for_the_running_uninstaller() {
        let d = scratch("cleanup_wait");
        let exe = d.join(UNINSTALLER);
        // A copy of ping.exe stands in for the running uninstaller: its image
        // cannot be deleted for about a second.
        let system = std::env::var_os("SystemRoot").map(PathBuf::from).unwrap().join("System32");
        fs::copy(system.join("PING.EXE"), &exe).unwrap();
        let mut running = std::process::Command::new(&exe)
            .args(["-n", "2", "127.0.0.1"])
            .stdout(std::process::Stdio::null())
            .spawn()
            .unwrap();
        assert!(fs::remove_file(&exe).is_err());
        cleanup_command(running.id(), &exe, &d).status().unwrap();
        let _ = running.wait();
        assert!(!exe.exists());
        assert!(!d.exists());
    }
}
