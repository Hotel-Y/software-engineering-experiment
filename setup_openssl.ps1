$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$thirdParty = Join-Path $root "third_party"
$destination = Join-Path $thirdParty "openssl"
$package = Join-Path $thirdParty "openssl.pkg.tar.zst"
$url = "https://mirror.msys2.org/mingw/mingw64/mingw-w64-x86_64-openssl-3.6.3-1-any.pkg.tar.zst"
$expectedSha256 = "82de7ff886112374ffae9e7b3c843c82342e198543fb024790416ef56434fe9f"

New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null
Invoke-WebRequest -Uri $url -OutFile $package

$actualSha256 = (Get-FileHash -Algorithm SHA256 $package).Hash.ToLower()
if ($actualSha256 -ne $expectedSha256) {
    throw "OpenSSL package hash mismatch."
}

if (Test-Path $destination) {
    Remove-Item -LiteralPath $destination -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $destination | Out-Null
tar -xf $package -C $destination

Write-Host "OpenSSL development package installed in: $destination"
