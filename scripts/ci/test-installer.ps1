#Requires -Version 7.2
<#
.SYNOPSIS
    Installs and uninstalls OpenLamina-Windows-Installer.exe silently, as the app's update and other programs run it,
    and checks what each leaves. For a throwaway user (the workflow's runner): it refuses if OpenLamina is installed.
.DESCRIPTION
    A silent install with /SWITCH puts the folder first in openvrpaths.vrpath, and the silent uninstall takes it out
    and removes it. -Signed also checks the signatures of what a release installs: the uninstaller, OpenLamina.exe and
    the two DLLs.
.EXAMPLE
    scripts\ci\test-installer.ps1 -Setup build\installer\OpenLamina-Windows-Installer.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Setup,
    [switch]$Signed,
    [string]$Logs = $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() })
)

$ErrorActionPreference = 'Stop'
$Setup = (Resolve-Path -LiteralPath $Setup).Path
$folder = Join-Path $env:LOCALAPPDATA 'Programs\OpenLamina'
$paths = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
$log = Join-Path $Logs 'setup.log'
$uninstallLog = Join-Path $Logs 'uninstall.log'

function Fail([string]$message) {
    "::error::$message"
    foreach ($file in $log, $uninstallLog) { if (Test-Path -LiteralPath $file) { "--- $file"; Get-Content -LiteralPath $file } }
    exit 1
}
function Runtimes {
    if (-not (Test-Path -LiteralPath $paths)) { return @() }
    @((Get-Content -LiteralPath $paths -Raw | ConvertFrom-Json).runtime | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\', '/') })
}
if (Test-Path -LiteralPath $folder) { Fail "$folder exists before the install: the runner isn't clean." }

$p = Start-Process -FilePath $Setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SWITCH', "/LOG=`"$log`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { Fail "The silent install with /SWITCH exited with code $($p.ExitCode) (100: the switch failed)." }
foreach ($file in 'bin\vrclient_x64.dll', 'bin\vrclient.dll', 'bin\version.txt', 'OpenLamina.exe', 'SOURCE.txt', 'unins000.exe') {
    if (-not (Test-Path -LiteralPath (Join-Path $folder $file))) { Fail "The install has no $file in $folder." }
}
$first = @(Runtimes)[0]
if (-not $first -or -not [string]::Equals($first, $folder, [StringComparison]::OrdinalIgnoreCase)) {
    Fail "After the install with /SWITCH, the first runtime in $paths is '$first', not $folder."
}
"Installed in $folder and first in $paths."

if ($Signed) {
    $installed = 'unins000.exe', 'OpenLamina.exe', 'bin\vrclient_x64.dll', 'bin\vrclient.dll' | ForEach-Object { Join-Path $folder $_ }
    & (Join-Path $PSScriptRoot 'check-signatures.ps1') @installed
    if ($LASTEXITCODE -ne 0) { Fail 'An installed file is not signed as the release should be.' }
}

$p = Start-Process -FilePath (Join-Path $folder 'unins000.exe') -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$uninstallLog`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { Fail "The silent uninstall exited with code $($p.ExitCode)." }
# The uninstaller finishes from a copy of itself in the temporary folder: wait for the folder to go (at most a minute)
for ($i = 0; $i -lt 60 -and (Test-Path -LiteralPath $folder); $i++) { Start-Sleep -Seconds 1 }
if (Test-Path -LiteralPath $folder) { Fail "The uninstall left ${folder}: $((Get-ChildItem -LiteralPath $folder -Recurse -Force).FullName -join ', ')" }
if (@(Runtimes) | Where-Object { [string]::Equals($_, $folder, [StringComparison]::OrdinalIgnoreCase) }) {
    Fail "After the uninstall, $paths still lists $folder."
}
"Uninstalled: $folder is gone and $paths doesn't list it."
exit 0
