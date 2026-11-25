#Requires -Version 7.2
<#
.SYNOPSIS
    Builds OpenLamina-Windows-Installer.exe, the Windows installer (installer\OpenLamina.iss), the way the release workflow
    (.github/workflows/build.yml) does.
.DESCRIPTION
    From the Windows builds' DLLs: -Artifacts is a folder with windows-x64\vrclient_x64.dll and windows-x86\vrclient.dll,
    as the workflow's artifacts are, or as build-windows.ps1 leaves them when copied there.

    1. scripts\ci\package.py lays out the OpenLamina folder as OpenLamina-Windows-Portable.zip has it (bin\, the licences,
       SOURCE.txt with -Version).
    2. RuntimeSwitcher\Tests runs, then RuntimeSwitcher builds OpenLamina.exe; warnings are errors.
    3. Inno Setup compiles installer\OpenLamina.iss to <Out>\OpenLamina-Windows-Installer.exe.

    -Sign, for a release, builds nothing: from the folder and OpenLamina.exe an earlier run left in <Out> (no
    -Artifacts), it signs the two DLLs and OpenLamina.exe, then does step 3 with Inno Setup signing the uninstaller and
    the installer, and checks every signature. scripts\ci\sign.ps1 says what it signs with.

    Needs the .NET SDK 8 or later, Python, and Inno Setup 6 (winget install JRSoftware.InnoSetup; the workflow:
    scripts\ci\get-inno-setup.ps1).
.EXAMPLE
    scripts\ci\build-installer.ps1 -Artifacts artifacts -Version 2026.11.06
#>
[CmdletBinding()]
param(
    [string]$Artifacts,
    [Parameter(Mandatory)] [string]$Version,
    [string]$Out,
    [string]$Repository = 'feimos-dev/OpenLamina',
    [switch]$Sign
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Sign -and -not $Artifacts) {
    throw '-Artifacts is needed: the folder with windows-x64\vrclient_x64.dll and windows-x86\vrclient.dll.'
}
if (-not $Out) {
    $Out = Join-Path $root 'build\installer'
}
# Absolute: Inno Setup reads relative paths from the .iss file's folder.
$Out = [IO.Path]::GetFullPath($Out, (Get-Location).Path)

function Invoke-Checked([scriptblock]$command, [string]$what) {
    & $command
    if ($LASTEXITCODE -ne 0) {
        throw "$what failed (exit code $LASTEXITCODE)."
    }
}

# Windows' four-number file version: 2026.11.06 is 2026.11.6.0; a version without numbers (dev-...) is 0.0.0.0.
$numeric = '0.0.0.0'
if ($Version -match '^(\d+(?:\.\d+){0,3})(?:-[0-9A-Za-z.-]+)?$') {
    $parts = @($Matches[1].Split('.') | ForEach-Object { [long]$_ })
    if (-not ($parts | Where-Object { $_ -gt 65535 })) {
        $numeric = (@($parts) + @(0, 0, 0, 0))[0..3] -join '.'
    }
}

$onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
$iscc = @(
    $(if ($onPath) { $onPath.Source })
    Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
    Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'
    Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $iscc) {
    throw 'Inno Setup 6 is needed (winget install JRSoftware.InnoSetup).'
}

$folder = Join-Path $Out 'OpenLamina'
$switcher = Join-Path $Out 'switcher'
$setup = Join-Path $Out 'OpenLamina-Windows-Installer.exe'
# Only what this script makes in $Out is deleted; anything else there is left alone. -Sign keeps the folder and the app.
foreach ($path in $(if ($Sign) { $setup } else { $folder, $switcher, $setup })) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null

if (-not $Sign) {
    # 1. The OpenLamina folder.
    $Artifacts = (Resolve-Path -LiteralPath $Artifacts).Path
    Invoke-Checked {
        python (Join-Path $root 'scripts\ci\package.py') --version $Version --artifacts $Artifacts --windows-folder $folder `
            --repository $Repository
    } 'Laying out the OpenLamina folder'

    # 2. OpenLamina.exe, after its tests.
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:DOTNET_NOLOGO = '1'
    Invoke-Checked { dotnet test (Join-Path $root 'RuntimeSwitcher\Tests') --nologo } 'The RuntimeSwitcher tests'
    Invoke-Checked {
        dotnet build (Join-Path $root 'RuntimeSwitcher\RuntimeSwitcher.csproj') --nologo -c Release -o $switcher `
            "-p:Version=$numeric" "-p:InformationalVersion=$Version" -p:ContinuousIntegrationBuild=true
    } 'Building OpenLamina.exe'
    # Newtonsoft.Json.dll goes with it, so its licence does too.
    $packages = ((dotnet nuget locals global-packages --list) -replace '^global-packages:\s*', '').Trim()
    $jsonVersion = ([xml](Get-Content -LiteralPath (Join-Path $root 'RuntimeSwitcher\RuntimeSwitcher.csproj') -Raw)).Project.ItemGroup.PackageReference |
        Where-Object Include -EQ 'Newtonsoft.Json' | ForEach-Object Version
    $jsonLicence = Join-Path $packages "newtonsoft.json\$jsonVersion\LICENSE.md"
    if (-not (Test-Path -LiteralPath $jsonLicence)) {
        throw "$jsonLicence is missing."
    }
    Copy-Item -LiteralPath $jsonLicence -Destination (Join-Path $switcher 'LICENSE_Newtonsoft.Json.txt')
    Write-Host "OpenLamina $Version ($numeric), Newtonsoft.Json $jsonVersion"
}

# OpenLamina's own programs: Newtonsoft.Json.dll keeps its maker's signature.
$own = (Join-Path $folder 'bin\vrclient_x64.dll'), (Join-Path $folder 'bin\vrclient.dll'), (Join-Path $switcher 'OpenLamina.exe')
$options = @()
if ($Sign) {
    foreach ($file in $own) {
        if (-not (Test-Path -LiteralPath $file)) {
            throw "$file is missing: run this without -Sign (and with the same -Out) first."
        }
    }
    & (Join-Path $PSScriptRoot 'sign.ps1') @own
    # Inno Setup's sign tool "openlamina": this PowerShell running sign.ps1 on the file ($q is a quote, $f the file).
    $options += '/DSignTool=openlamina'
    $options += '/Sopenlamina=$q' + (Get-Process -Id $PID).Path + '$q -NoProfile -File $q' + (Join-Path $PSScriptRoot 'sign.ps1') + '$q $f'
}

# 3. The installer. Every file gets the commit's time, in the files and in the setup.
$commitTime = [DateTimeOffset]::FromUnixTimeSeconds([long](git -C $root log -1 --format=%ct)).UtcDateTime
Get-ChildItem -LiteralPath $folder, $switcher -Recurse -File | ForEach-Object { $_.LastWriteTimeUtc = $commitTime }
Invoke-Checked {
    & $iscc /Q "/DAppVersion=$Version" "/DNumericVersion=$numeric" "/DFolder=$folder" "/DSwitcher=$switcher" "/DOutput=$Out" `
        "/DTouchDate=$($commitTime.ToString('yyyy-MM-dd', [cultureinfo]::InvariantCulture))" "/DTouchTime=$($commitTime.ToString('HH:mm:ss', [cultureinfo]::InvariantCulture))" `
        @options (Join-Path $root 'installer\OpenLamina.iss')
} 'Compiling the installer'

if ($Sign) {
    & (Join-Path $PSScriptRoot 'check-signatures.ps1') @own $setup
    if ($LASTEXITCODE -ne 0) {
        throw 'The signatures didn''t pass the check.'
    }
}

Write-Host "  $setup ($([math]::Round((Get-Item -LiteralPath $setup).Length / 1KB)) KB)"
