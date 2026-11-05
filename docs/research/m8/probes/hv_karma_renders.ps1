# M8 research probe (houdini-volumes.md): renders the scenes written by
# hv_karma_scenes.py with Karma CPU through husk in an isolated environment
# (docs/research/m6/probes/husk_env.ps1: own preferences, no user packages,
# never the user's Houdini session) and records the wall time per scene.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File docs\research\m8\probes\hv_karma_renders.ps1 -Dir <outdir of hv_karma_scenes.py> [-Only name,name]
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string[]]$Only = @(),
    [string]$Renderer = 'BRAY_HdKarma'
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$huskEnv = Join-Path $here '..\..\m6\probes\husk_env.ps1'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$scenes = Get-Content (Join-Path $Dir 'scenes.json') | ConvertFrom-Json
$times = @{}
foreach ($s in $scenes) {
    if ($Only.Count -gt 0 -and -not ($Only -contains $s)) { continue }
    $usd = Join-Path $Dir "$s.usda"
    $exr = Join-Path $Dir "$s.exr"
    $log = Join-Path $Dir "$s.log"
    $t0 = Get-Date
    & powershell -NoProfile -ExecutionPolicy Bypass -File $huskEnv -Tool husk.exe -Log $log -TimeoutSec 900 `
        -CmdLine "--renderer $Renderer --headlight none -s /Render/settings -V2 -o `"$exr`" `"$usd`"" | Out-Null
    $dt = ((Get-Date) - $t0).TotalSeconds
    $times[$s] = [math]::Round($dt, 2)
    Write-Output ("{0,-20} exit {1} {2,7:N2} s" -f $s, $LASTEXITCODE, $dt)
}
$times | ConvertTo-Json | Set-Content -Path (Join-Path $Dir "times_$Renderer.json") -Encoding ascii
