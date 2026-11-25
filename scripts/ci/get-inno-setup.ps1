#Requires -Version 7.2
<#
.SYNOPSIS
    Installs Inno Setup at a pinned version, for the workflow: the same compiler in the job that builds the installer
    and in the job that signs it.
.DESCRIPTION
    Downloads the installer from Inno Setup's own releases, refuses it unless its hash is the pinned one, and runs it
    silently, over whatever version the machine has. On a developer's PC: winget install JRSoftware.InnoSetup.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

# The pin: the SHA-256 GitHub lists for the release's file.
$version = '6.7.1'
$sha256 = '4d11e8050b6185e0d49bd9e8cc661a7a59f44959a621d31d11033124c4e8a7b0'
$url = "https://github.com/jrsoftware/issrc/releases/download/is-$($version.Replace('.', '_'))/innosetup-$version.exe"

$setup = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }) "innosetup-$version.exe"
Invoke-WebRequest -Uri $url -OutFile $setup -MaximumRetryCount 3 -RetryIntervalSec 5
$actual = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $sha256) {
    throw "Inno Setup $version was refused: its SHA-256 is $actual, not the pinned $sha256."
}

$process = Start-Process -FilePath $setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/ALLUSERS' -Wait -PassThru
if ($process.ExitCode -ne 0) {
    throw "Inno Setup's installer failed (exit code $($process.ExitCode))."
}

# What Windows now lists as installed, and where: it goes into the folder of an earlier install when there is one.
$installed = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1',
             'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1' |
    Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object { Get-ItemProperty -LiteralPath $_ } | Select-Object -First 1
if (-not $installed -or $installed.DisplayVersion -ne $version) {
    throw "Inno Setup $version isn't what is installed now (found: '$($installed.DisplayVersion)')."
}
$folder = $installed.InstallLocation.TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $folder 'ISCC.exe'))) {
    throw "Inno Setup $version installed, but $folder has no ISCC.exe."
}
# First on the path of the workflow's next steps, so this compiler is the ISCC.exe they find.
if ($env:GITHUB_PATH) {
    $folder | Add-Content -LiteralPath $env:GITHUB_PATH -Encoding utf8NoBOM
}
Write-Host "Inno Setup ${version}: $folder"
