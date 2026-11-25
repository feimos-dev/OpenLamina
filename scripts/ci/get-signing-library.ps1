#Requires -Version 7.3
<#
.SYNOPSIS
    Gets Azure Artifact Signing's signtool library, at a pinned version, and writes the file that names the signing
    account: what scripts\ci\sign.ps1 signs with in a release.
.DESCRIPTION
    Downloads Microsoft.ArtifactSigning.Client from nuget.org, refuses it unless its hash is the pinned one and its
    library carries Microsoft's signature, and unpacks it to -Out. Then SIGN_DLIB and SIGN_METADATA name the library
    and the account's file, for this process and (in a workflow) the steps after this one.

    The library asks the Azure CLI for its token, so `az login` comes before signing. It needs the .NET 10 runtime.
.EXAMPLE
    scripts\ci\get-signing-library.ps1 -Out $env:TEMP\signing -Endpoint https://eus.codesigning.azure.net/ -Account myaccount -CertificateProfile myprofile
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Out,
    [Parameter(Mandatory)] [string]$Endpoint,
    [Parameter(Mandatory)] [string]$Account,
    [Parameter(Mandatory)] [string]$CertificateProfile
)

$ErrorActionPreference = 'Stop'

# The pin: nuget.org's SHA-512 of the package (base64, as its catalog gives it).
$version = '1.0.151'
$sha512 = 'BvPApD/Bxbzj2uf8qBYJ0wPoZYvAySHfrsIMZ2JkMQXy7NdTbQ81Gs71A0n/tOYOIWk3xpKY3YVZ1VtH0jcZhQ=='
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.artifactsigning.client/$version/microsoft.artifactsigning.client.$version.nupkg"

$Out = [IO.Path]::GetFullPath($Out, (Get-Location).Path)
if (Test-Path -LiteralPath $Out) {
    Remove-Item -LiteralPath $Out -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$package = Join-Path $Out 'client.nupkg'
Invoke-WebRequest -Uri $url -OutFile $package -MaximumRetryCount 3 -RetryIntervalSec 5
$actual = [Convert]::ToBase64String([Security.Cryptography.SHA512]::HashData([IO.File]::ReadAllBytes($package)))
if ($actual -cne $sha512) {
    throw "Microsoft.ArtifactSigning.Client $version was refused: its SHA-512 is $actual, not the pinned $sha512."
}

$client = Join-Path $Out 'client'
[IO.Compression.ZipFile]::ExtractToDirectory($package, $client)
$dlib = Join-Path $client 'bin\x64\Azure.CodeSigning.Dlib.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $dlib
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|, )O=Microsoft Corporation(,|$)') {
    throw "$dlib isn't signed by Microsoft ($($signature.Status); $($signature.SignerCertificate.Subject))."
}

# Only the Azure CLI's login may sign (and the environment's, which a workflow doesn't set).
$metadata = Join-Path $Out 'metadata.json'
[ordered]@{
    Endpoint               = $Endpoint
    CodeSigningAccountName = $Account
    CertificateProfileName = $CertificateProfile
    ExcludeCredentials     = @(
        'ManagedIdentityCredential', 'WorkloadIdentityCredential', 'SharedTokenCacheCredential',
        'VisualStudioCredential', 'VisualStudioCodeCredential', 'AzurePowerShellCredential',
        'AzureDeveloperCliCredential', 'InteractiveBrowserCredential'
    )
} | ConvertTo-Json | Set-Content -LiteralPath $metadata -Encoding utf8NoBOM

$env:SIGN_DLIB = $dlib
$env:SIGN_METADATA = $metadata
if ($env:GITHUB_ENV) {
    "SIGN_DLIB=$dlib", "SIGN_METADATA=$metadata" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8NoBOM
}

Write-Host "Microsoft.ArtifactSigning.Client ${version}: $dlib"
Write-Host "Signing as $Account / $CertificateProfile at $Endpoint"
