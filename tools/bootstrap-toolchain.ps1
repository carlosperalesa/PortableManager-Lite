#Requires -Version 5.1
[CmdletBinding()] param([switch]$Force)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$Root = Split-Path -Parent $PSScriptRoot
$Lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain.lock.json') -Raw | ConvertFrom-Json
$Dest = Join-Path $Root 'tools\toolchains\llvm-mingw'
$Compiler = Join-Path $Dest 'bin\clang++.exe'
$ReadObj = Join-Path $Dest 'bin\llvm-readobj.exe'
if ((Test-Path $Compiler) -and (Test-Path $ReadObj) -and -not $Force) {
  $v = (& $Compiler --version | Select-Object -First 1)
  if ($v -match [regex]::Escape("clang version $($Lock.llvm_version)")) { Write-Host $v; exit 0 }
  throw 'Existing toolchain has unexpected version. Use -Force only after reviewing it.'
}
$api = "https://api.github.com/repos/$($Lock.repository)/releases/tags/$($Lock.release_tag)"
$headers = @{ Accept='application/vnd.github+json'; 'User-Agent'='PM-Lite-bootstrap'; 'X-GitHub-Api-Version'='2022-11-28' }
$release = Invoke-RestMethod -Uri $api -Headers $headers
if ($release.tag_name -ne $Lock.release_tag) { throw 'Unexpected GitHub release tag.' }
$assetName = $Lock.windows.asset
$assets = @($release.assets | Where-Object { $_.name -eq $assetName })
if ($assets.Count -ne 1) { throw 'Expected exactly one official Windows toolchain asset.' }
$expected = ([string]$Lock.windows.sha256).ToLowerInvariant()
if (([string]$assets[0].digest).ToLowerInvariant() -ne "sha256:$expected") { throw 'GitHub API digest differs from the pinned SHA-256.' }
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('pm-toolchain-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
  $zip = Join-Path $tmp $assetName
  Invoke-WebRequest -Uri $assets[0].browser_download_url -OutFile $zip -Headers @{ 'User-Agent'='PM-Lite-bootstrap' } -UseBasicParsing
  $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $expected) { throw "SHA-256 mismatch: $actual" }
  $extract = Join-Path $tmp 'extract'
  Expand-Archive -LiteralPath $zip -DestinationPath $extract
  $candidates = @(Get-ChildItem -LiteralPath $extract -Recurse -File -Filter clang++.exe | Where-Object { $_.Directory.Name -eq 'bin' })
  if ($candidates.Count -ne 1) { throw 'Expected exactly one bin\clang++.exe in archive.' }
  $source = Split-Path -Parent (Split-Path -Parent $candidates[0].FullName)
  if (-not (Test-Path (Join-Path $source 'bin\llvm-readobj.exe'))) { throw 'llvm-readobj.exe missing.' }
  $version = (& $candidates[0].FullName --version | Select-Object -First 1)
  if ($version -notmatch [regex]::Escape("clang version $($Lock.llvm_version)")) { throw "Unexpected compiler version: $version" }
  New-Item -ItemType Directory -Path (Split-Path -Parent $Dest) -Force | Out-Null
  if (Test-Path $Dest) { Move-Item $Dest "$Dest.previous.$(Get-Date -Format yyyyMMddHHmmss)" }
  Move-Item $source $Dest
  Write-Host "Installed portable LLVM-MinGW. SHA-256=$actual"
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
