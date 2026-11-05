# M9 research probe (curves-hair.md): renders the scenes written by
# ch_karma_scenes.py with Karma CPU through husk in an isolated environment
# (docs/research/m6/probes/husk_env.ps1: own preferences, no user packages,
# never the user's Houdini session) and records the wall time per scene.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File docs\research\m9\probes\ch_karma_renders.ps1 -Dir <outdir of ch_karma_scenes.py> [-Only name,name] [-Skip probe_hydra]
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string[]]$Only = @(),
    [string[]]$Skip = @('probe_hydra'),
    [string]$Renderer = 'BRAY_HdKarma'
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$huskEnv = Join-Path $here '..\..\m6\probes\husk_env.ps1'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$scenes = Get-Content (Join-Path $Dir 'scenes.json') | ConvertFrom-Json
$timesFile = Join-Path $Dir "times_$Renderer.json"
$times = @{}
if (Test-Path $timesFile) {
    (Get-Content $timesFile | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $times[$_.Name] = $_.Value }
}
foreach ($s in $scenes) {
    if ($Only.Count -gt 0 -and -not ($Only -contains $s)) { continue }
    if ($Skip -contains $s) { continue }
    $usd = Join-Path $Dir "$s.usda"
    $exr = Join-Path $Dir "$s.exr"
    $log = Join-Path $Dir "$s.log"
    $t0 = Get-Date
    & powershell -NoProfile -ExecutionPolicy Bypass -File $huskEnv -Tool husk.exe -Log $log -TimeoutSec 900 `
        -CmdLine "--renderer $Renderer --headlight none -s /Render/settings -V2 -o `"$exr`" `"$usd`"" | Out-Null
    $dt = ((Get-Date) - $t0).TotalSeconds
    $times[$s] = [math]::Round($dt, 2)
    Write-Output ("{0,-28} exit {1} {2,7:N2} s" -f $s, $LASTEXITCODE, $dt)
}
$times | ConvertTo-Json | Set-Content -Path $timesFile -Encoding ascii
