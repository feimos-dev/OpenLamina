#Requires -Version 7.2
<#
.SYNOPSIS
    Records SteamVR's own answers to every conformance call, with SteamVR's null driver (no headset needed), as the
    reference OpenLamina is compared with (expected\steamvr-<version>.json).
.DESCRIPTION
    1. Finds SteamVR: the paths file's runtime entry ending in \SteamVR, else Steam's steamapps\common\SteamVR.
    2. Refuses while SteamVR, Monado, Pimax's service or OVR Lighthouse Manager runs.
    3. Copies, with their SHA-256s, the paths file (%LOCALAPPDATA%\openvr\openvrpaths.vrpath) and, from Steam's config
       folder, steamvr.vrsettings, chaperone_info.vrchap, appconfig.json and vrappconfig\; and keeps the active OpenXR
       runtime (the 64-bit HKLM\SOFTWARE\Khronos\OpenXR\1 ActiveRuntime), which SteamVR takes over when it starts.
    4. Turns on the null driver in steamvr.vrsettings (driver_null.enable, steamvr.forcedDriver = null,
       steamvr.activateMultipleDrivers = false, or true with -Controllers, other keys kept) and puts SteamVR first in
       the paths file.
    5. Runs run.py against SteamVR's vrclient_x64.dll (--real-runtime: unsafe_methods.txt isn't called).
    6. Always, at the end: stops SteamVR's processes, puts every backed-up file back (and removes any of them that
       didn't exist before) and the active OpenXR runtime (writing it needs an elevated prompt), and fails unless every
       SHA-256 matches its backup and the runtime is the one before.
.EXAMPLE
    tests\conformance\record-steamvr.ps1 -Conformance build-tests\tests\oc_conformance.exe -Out out\steamvr
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Conformance,
    [Parameter(Mandatory)] [string]$Out,
    [switch]$WhatIf,
    # Only the init-matrix case, and no expected file: to check SteamVR starts with the null driver
    [switch]$Probe,
    # One at a time: a test program that ends SteamVR would otherwise take the others running beside it down too
    [int]$Jobs = 1,
    # The null headset with SteamVR's other drivers too (Lighthouse: real controllers switched on), for the cases whose
    # answers need controllers; SteamVR doesn't turn them off on exit or when idle meanwhile
    [switch]$Controllers,
    # Keep what an earlier run into the same -Out finished, and run only the rest (run.py --resume)
    [switch]$Resume
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

# 1. SteamVR and Steam's config folder
$pathsFile = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
$steamvr = $null
if (Test-Path -LiteralPath $pathsFile) {
    $paths = Get-Content -LiteralPath $pathsFile -Raw | ConvertFrom-Json
    $steamvr = $paths.runtime | Where-Object { $_ -match '[\\/]SteamVR[\\/]?$' -and (Test-Path (Join-Path $_ 'bin\vrclient_x64.dll')) } | Select-Object -First 1
}
$steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
if (-not $steamvr -and $steam) {
    $candidate = Join-Path $steam 'steamapps\common\SteamVR'
    if (Test-Path (Join-Path $candidate 'bin\vrclient_x64.dll')) { $steamvr = $candidate }
}
if (-not $steamvr) { throw 'SteamVR not found' }
if (-not $steam) { throw "Steam's folder not found (HKCU\Software\Valve\Steam SteamPath)" }
$config = Join-Path $steam 'config'
$version = (Get-Content -LiteralPath (Join-Path $steamvr 'bin\version.txt') -Raw).Trim()
Write-Host "SteamVR $version at $steamvr; Steam config $config"

# 2. Nothing VR running
$running = Get-Process -Name vrserver, vrmonitor, vrcompositor, monado-service, pi_server, OVRLighthouseManager -ErrorAction SilentlyContinue
if ($running) { throw "Close these first: $(($running.Name | Sort-Object -Unique) -join ', ')" }

# 3. The backup
$backup = Join-Path $Out 'backup'
$files = @($pathsFile) + @('steamvr.vrsettings', 'chaperone_info.vrchap', 'appconfig.json' | ForEach-Object { Join-Path $config $_ })
if (Test-Path (Join-Path $config 'vrappconfig')) {
    $files += Get-ChildItem -LiteralPath (Join-Path $config 'vrappconfig') -File | ForEach-Object FullName
}
$manifest = foreach ($f in $files) {
    [pscustomobject]@{
        Path   = $f
        Exists = Test-Path -LiteralPath $f
        Hash   = if (Test-Path -LiteralPath $f) { (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash } else { $null }
        Copy   = Join-Path $backup ([IO.Path]::GetFileName($f) + '.' + [Math]::Abs($f.GetHashCode()))
    }
}
$appconfigDir = Join-Path $config 'vrappconfig'
$appconfigBefore = if (Test-Path $appconfigDir) { @(Get-ChildItem -LiteralPath $appconfigDir -File | ForEach-Object FullName) } else { @() }

# The active OpenXR runtime: SteamVR makes itself the active one when it starts. The 64-bit view, whichever PowerShell runs
$openxrKey = 'SOFTWARE\Khronos\OpenXR\1'
function Get-ActiveRuntime {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    try {
        $key = $base.OpenSubKey($openxrKey)
        if (-not $key) { return $null }
        try {
            $value = $key.GetValue('ActiveRuntime', $null, 'DoNotExpandEnvironmentNames')
            if ($null -eq $value) { return $null }
            return [pscustomobject]@{ Value = [string]$value; Kind = $key.GetValueKind('ActiveRuntime') }
        }
        finally { $key.Dispose() }
    }
    finally { $base.Dispose() }
}
function Set-ActiveRuntime($Runtime) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    try {
        if ($null -eq $Runtime) {
            $key = $base.OpenSubKey($openxrKey, $true)
            if ($key) { try { $key.DeleteValue('ActiveRuntime', $false) } finally { $key.Dispose() } }
        }
        else {
            $key = $base.CreateSubKey($openxrKey)
            try { $key.SetValue('ActiveRuntime', $Runtime.Value, $Runtime.Kind) } finally { $key.Dispose() }
        }
    }
    finally { $base.Dispose() }
}
$activeRuntime = Get-ActiveRuntime

if ($WhatIf) {
    $manifest | Format-Table Exists, Hash, Path -AutoSize | Out-String -Width 220 | Write-Host
    Write-Host "Would keep the active OpenXR runtime (HKLM\$openxrKey ActiveRuntime): $($activeRuntime ? $activeRuntime.Value : '(none)')"
    Write-Host "Would set driver_null.enable, steamvr.forcedDriver = null, steamvr.activateMultipleDrivers = $([bool]$Controllers) in $(Join-Path $config 'steamvr.vrsettings')"
    Write-Host "Would run run.py against $(Join-Path $steamvr 'bin\vrclient_x64.dll'), then put everything back"
    return
}

New-Item -ItemType Directory -Force -Path $backup | Out-Null
foreach ($m in $manifest) { if ($m.Exists) { Copy-Item -LiteralPath $m.Path -Destination $m.Copy -Force } }
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding utf8
$activeRuntime | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'openxr-active-runtime.json') -Encoding utf8

$failed = $false
try {
    # 4. The null driver
    $settingsPath = Join-Path $config 'steamvr.vrsettings'
    $settings = if (Test-Path -LiteralPath $settingsPath) { Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json -AsHashtable } else { @{} }
    if (-not $settings.ContainsKey('driver_null')) { $settings['driver_null'] = @{} }
    if (-not $settings.ContainsKey('steamvr')) { $settings['steamvr'] = @{} }
    $settings['driver_null']['enable'] = $true
    $settings['steamvr']['forcedDriver'] = 'null'
    $settings['steamvr']['activateMultipleDrivers'] = [bool]$Controllers
    if ($Controllers) {
        # SteamVR turns the controllers off when it exits, and after five idle minutes; the run restarts it often
        if (-not $settings.ContainsKey('power')) { $settings['power'] = @{} }
        $settings['power']['powerOffOnExit'] = $false
        $settings['power']['turnOffControllersTimeout'] = 86400.0  # seconds: a day
    }
    $settings | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $settingsPath -Encoding utf8

    # SteamVR first in the paths file, as SteamVR puts itself when started from Steam: its own processes (vrmonitor
    # and the like) find the runtime there, and would otherwise load whatever is first (OpenLamina, say)
    if (Test-Path -LiteralPath $pathsFile) {
        $pathsJson = Get-Content -LiteralPath $pathsFile -Raw | ConvertFrom-Json -AsHashtable
        $others = @($pathsJson['runtime'] | Where-Object { $_ -ne $steamvr })
        $pathsJson['runtime'] = @($steamvr) + $others
        $pathsJson | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $pathsFile -Encoding utf8
    }

    # 5. The run. SteamVR is started on its own before every test program that would otherwise find it gone
    # (--keep-running): started by a test program, its shared namespace goes away with that program's first clients and
    # a later Init waits forever; and it quits everything when the scene application it counts as its first exits
    # without shutting down, which some calls of the matrix do.
    $vrclient = Join-Path $steamvr 'bin\vrclient_x64.dll'
    & python -I (Join-Path $here 'run.py') --runtime $vrclient --conformance $Conformance --xr-runtime none `
        --real-runtime --keep-running (Join-Path $steamvr 'bin\win64\vrstartup.exe') --allow-failures --jobs $Jobs `
        --out $Out @($Probe ? @('--cases', 'init-matrix') : @()) @($Resume ? @('--resume') : @())
    if ($LASTEXITCODE -ne 0) { throw "run.py failed ($LASTEXITCODE)" }
}
catch {
    $failed = $true
    Write-Warning $_
}
finally {
    # 6. SteamVR stopped, every file back as it was
    Get-Process -Name vrserver, vrmonitor, vrcompositor, vrdashboard, vrwebhelper, vrstartup, vrprismhost -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    $mismatch = @()
    foreach ($m in $manifest) {
        if ($m.Exists) {
            Copy-Item -LiteralPath $m.Copy -Destination $m.Path -Force
            if ((Get-FileHash -LiteralPath $m.Path -Algorithm SHA256).Hash -ne $m.Hash) { $mismatch += $m.Path }
        }
        elseif (Test-Path -LiteralPath $m.Path) {
            Remove-Item -LiteralPath $m.Path -Force
        }
    }
    if (Test-Path $appconfigDir) {
        Get-ChildItem -LiteralPath $appconfigDir -File | Where-Object { $appconfigBefore -notcontains $_.FullName } |
            ForEach-Object { Write-Host "Removing $($_.FullName), made during the run"; Remove-Item -LiteralPath $_.FullName -Force }
    }
    $runtimeNow = Get-ActiveRuntime
    if (($runtimeNow ? $runtimeNow.Value : $null) -ne ($activeRuntime ? $activeRuntime.Value : $null)) {
        $was = $activeRuntime ? $activeRuntime.Value : '(none)'
        try {
            Set-ActiveRuntime $activeRuntime
            Write-Host "The active OpenXR runtime put back: $was"
        }
        catch {
            $mismatch += "HKLM\$openxrKey ActiveRuntime (was $was, now $($runtimeNow.Value); writing it needs an elevated prompt)"
        }
    }
    if ($mismatch) { throw "NOT put back exactly (backups in $backup): $($mismatch -join ', ')" }
    Write-Host "Every file put back; SHA-256s match ($backup). The active OpenXR runtime is the one before."
}
if ($failed) { throw 'The recording failed; files were put back.' }

# 7. The expected answers
if ($Probe) {
    Get-Content -LiteralPath (Join-Path $Out 'raw\case-init-matrix.jsonl') | Write-Host
    return
}
# One record per line, sorted, so a new recording's diff is a line for each answer that changed
$target = Join-Path $here "expected\steamvr-$version.json"
New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
& python -I (Join-Path $here 'reference.py') (Join-Path $Out 'results.json') $target
if ($LASTEXITCODE -ne 0) { throw "reference.py failed ($LASTEXITCODE)" }
