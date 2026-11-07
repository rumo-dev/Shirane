param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $env:VCPKG_ROOT -and (Test-Path "$repo\.tools\vcpkg\vcpkg.exe")) {
    $env:VCPKG_ROOT = "$repo\.tools\vcpkg"
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' -property installationPath
    $env:PATH = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$env:PATH"
    if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }
}
if (-not $env:VCPKG_ROOT) { throw 'Set VCPKG_ROOT to your vcpkg installation.' }
if (-not $env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS = Join-Path $repo 'out/downloads' }
if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $repo 'out/cache' }
New-Item -ItemType Directory -Force $env:VCPKG_DOWNLOADS,$env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
$preset = 'windows-' + $Configuration.ToLowerInvariant()
Push-Location $repo
try {
    cmake --preset windows-vs2022-x64
    if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
    cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    ctest --preset $preset --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
} finally { Pop-Location }
