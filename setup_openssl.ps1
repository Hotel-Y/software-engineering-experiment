# 下载项目锁定的 MSYS2 OpenSSL 开发包，并在解压前校验供应链哈希。
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$thirdParty = Join-Path $root "third_party"
$destination = Join-Path $thirdParty "openssl"
$package = Join-Path $thirdParty "openssl.pkg.tar.zst"
# 版本与 SHA-256 成对固定；升级依赖时必须同时更新并重新核实二者。
$url = "https://repo.msys2.org/mingw/mingw64/mingw-w64-x86_64-openssl-3.6.3-1-any.pkg.tar.zst"
$expectedSha256 = "82de7ff886112374ffae9e7b3c843c82342e198543fb024790416ef56434fe9f"

New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null
Invoke-WebRequest -Uri $url -OutFile $package

# 哈希不一致时立即停止，绝不解压来源异常或下载损坏的包。
$actualSha256 = (Get-FileHash -Algorithm SHA256 $package).Hash.ToLower()
if ($actualSha256 -ne $expectedSha256) {
    throw "OpenSSL package hash mismatch."
}

# 校验通过后再替换旧依赖目录，避免把半下载内容作为有效开发包。
if (Test-Path $destination) {
    Remove-Item -LiteralPath $destination -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $destination | Out-Null
tar -xf $package -C $destination

Write-Host "OpenSSL development package installed in: $destination"
