# Runs a hython script in an isolated Houdini environment: its own empty
# preference directory (no user packages), no HOUDINI_PACKAGE_DIR/HSITE/
# PXR/HOME overrides, the installed Nova package skipped and, with -Stage,
# the staged package on HOUDINI_PATH. A running Houdini session is never
# touched.
#
#   powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script x.py -Stage build\vs2022\stage\Release\nova [args]
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [string]$Stage = '',
    [string]$Hfs = 'C:\Program Files\Side Effects Software\Houdini 22.0.466',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ScriptArgs
)

$prefs = Join-Path ([System.IO.Path]::GetTempPath()) 'nova_hython_prefs'
New-Item -ItemType Directory -Force $prefs | Out-Null
$env:HOUDINI_USER_PREF_DIR = (Join-Path $prefs 'houdini__HVER__').Replace('\', '/')
foreach ($v in 'HOME', 'HOUDINI_PACKAGE_DIR', 'HSITE', 'PXR_PLUGINPATH_NAME', 'HOUDINI_USD_DSO_PATH', 'HOUDINI_PATH') {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
$env:HOUDINI_PACKAGE_SKIPLIST = 'nova'
$env:FXHOUDINIMCP_AUTOSTART = '0'
if ($Stage) {
    $env:HOUDINI_PATH = (Resolve-Path $Stage).Path.Replace('\', '/') + ';&'
}
& (Join-Path $Hfs 'bin\hython.exe') $Script @ScriptArgs
exit $LASTEXITCODE
