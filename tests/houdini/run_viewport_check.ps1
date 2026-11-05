# Solaris viewport check of the installed Nova package (dist/), in a separate
# Houdini instance that quits by itself; a running Houdini session is never
# touched. Not part of ctest (needs a GUI and a license).
#
#   powershell -ExecutionPolicy Bypass -File tests\houdini\run_viewport_check.ps1 -Out C:\temp\nova_viewport
#
# Writes 1_gl.png, 2_nova.png, 3_nova_edit.png (a prim added live),
# 5_nova_material.png and 6_nova_material_edit.png (a MaterialX Standard
# Surface assigned live, then its base color edited), 7_nova_builder.png and
# 8_nova_displacement.png (a Nova Material Builder material with Karma's AO,
# then its displacement switched on live), 4_karma.png and
# log.json (renderers found, steps, image means) to -Out, Houdini's output to
# stdout.txt/stderr.txt next to them. Uses the installed package, or with
# -Stage the staged one (build/vs2022/stage/Release/nova; the installed
# package is skipped then, so nothing needs installing while Houdini runs).
# Houdini 22.0.466 crashes in hou.exit() at the very end (also without Nova);
# the captures and log.json are complete by then. The previous run's *.png and
# log.json in -Out are deleted first (keep reference images elsewhere), and the
# script exits 1 on a timeout or when log.json does not end with the "done"
# step, so a crashed or hung run never reports old results.
#
# -Only rendersettings runs only the Nova Render Settings steps; -Only pyro
# the volume steps instead (M8): an in-memory SOP Import of the pyro sim with
# the Karma Pyro Material, fire switched on live, a frame change (the SOP
# recooks), then Karma (20_pyro_smoke ... 23_pyro_karma). -Only m9: hair,
# subsurface, 40 lights and an emissive quad, the quad made a light and the
# hair ribbons through the Render Geometry Settings LOP's Nova tab, live, then
# Karma (30_m9_nova ... 33_m9_karma). -Only xpu: Nova XPU in the viewport,
# the Nova LOP set to XPU with a sample count it does not reach, a prim added
# live, then Nova on the CPU (40_xpu ... 44_nova_cpu, ui_xpu_lop). -Only
# velocity: tests/scenes/motion_velocity.usda, velocity blur of a quad
# switched on and off live through the Render Geometry Settings LOP's Nova
# tab, then Karma (60_velocity_before ... 63_velocity_karma).
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Hfs = 'C:\Program Files\Side Effects Software\Houdini 22.0.466',
    [int]$TimeoutSeconds = 480,
    [string]$Stage = '',
    [string]$Only = ''
)

New-Item -ItemType Directory -Force $Out | Out-Null
$logPath = Join-Path $Out 'log.json'
Remove-Item (Join-Path $Out '*.png'), $logPath -ErrorAction SilentlyContinue
# Houdini takes its preference directory from HOME if set (Git Bash sets it),
# which would skip the user's packages and with them Nova.
Remove-Item Env:HOME -ErrorAction SilentlyContinue
$env:FXHOUDINIMCP_AUTOSTART = '0'
$env:HOUDINI_PATH = (Join-Path $PSScriptRoot 'hpath') + ';&'
if ($Stage) {
    # Absolute: a relative HOUDINI_PATH entry would depend on Houdini's working
    # directory (a relative -Stage left Nova out of the renderer list).
    $Stage = (Resolve-Path $Stage).Path
    $env:HOUDINI_PACKAGE_SKIPLIST = 'nova'
    $env:HOUDINI_PATH = $Stage.Replace('\', '/') + ';' + $env:HOUDINI_PATH
}
$env:NOVA_VIEWPORT_CHECK = $Out
$env:NOVA_VIEWPORT_CHECK_ONLY = $Only
$env:TF_DEBUG = 'HDNOVA_MATERIAL HDNOVA_AOV'
$p = Start-Process -FilePath (Join-Path $Hfs 'bin\houdini.exe') -ArgumentList '-foreground' -PassThru `
    -RedirectStandardOutput (Join-Path $Out 'stdout.txt') -RedirectStandardError (Join-Path $Out 'stderr.txt')
$timedOut = -not $p.WaitForExit($TimeoutSeconds * 1000)
if ($timedOut) {
    Write-Output "timeout: stopping Houdini $($p.Id)"
    Stop-Process -Id $p.Id -Force
}
# Houdini's exit code says nothing (it crashes in hou.exit() after a good
# run); the log's last step does.
$last = $null
if (Test-Path $logPath) {
    Get-Content $logPath
    try { $last = @((Get-Content $logPath -Raw | ConvertFrom-Json).steps)[-1].step } catch { $last = $null }
} else {
    Write-Output "no log.json: Houdini stopped before the check started"
}
if ($timedOut -or $last -ne 'done') {
    Write-Output "viewport check incomplete (last step: $last)"
    exit 1
}
