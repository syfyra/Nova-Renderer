# M9 research probe (docs/research/m9/sss.md): renders the scenes written by
# sss_scenes.py through husk in an isolated environment
# (docs/research/m6/probes/husk_env.ps1: own preferences, no user packages,
# never the user's Houdini session) and records the wall time per scene.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File docs\research\m9\probes\sss_renders.ps1 -Dir <outdir> [-Renderer BRAY_HdKarma|Nova] [-Stage build\vs2022\stage\Release\nova] [-Only a,b]
#
# Output: <Dir>\<Renderer>\<scene>.exr and .log, times.json.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string[]]$Only = @(),
    [string]$Renderer = 'BRAY_HdKarma',
    [string]$Stage = '',
    [string]$Extra = ''
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$huskEnv = Join-Path $here '..\..\m6\probes\husk_env.ps1'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$scenes = (Get-Content (Join-Path $Dir 'scenes.json') -Raw | ConvertFrom-Json).PSObject.Properties.Name
$out = Join-Path $Dir $Renderer
New-Item -ItemType Directory -Force $out | Out-Null
$timesFile = Join-Path $out 'times.json'
$times = @{}
if (Test-Path $timesFile) {
    (Get-Content $timesFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $times[$_.Name] = $_.Value }
}
foreach ($s in $scenes) {
    if ($Only.Count -gt 0 -and -not ($Only -contains $s)) { continue }
    $usd = Join-Path $Dir "$s.usda"
    $exr = Join-Path $out "$s.exr"
    $log = Join-Path $out "$s.log"
    $t0 = Get-Date
    $stageArg = @()
    if ($Stage) { $stageArg = @('-Stage', $Stage) }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $huskEnv -Tool husk.exe @stageArg -Log $log -TimeoutSec 1800 `
        -CmdLine "--renderer $Renderer --headlight none -s /Render/s -V2 $Extra -o `"$exr`" `"$usd`"" | Out-Null
    $code = $LASTEXITCODE
    $dt = ((Get-Date) - $t0).TotalSeconds
    $times[$s] = [math]::Round($dt, 2)
    Write-Output ("{0,-16} exit {1} {2,8:N2} s" -f $s, $code, $dt)
    $times | ConvertTo-Json | Set-Content -Path $timesFile -Encoding ascii
}
