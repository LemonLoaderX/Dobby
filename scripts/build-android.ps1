[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$AndroidNdkRoot = $env:ANDROID_NDK_ROOT,
    [int]$AndroidApiLevel = 23
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($AndroidNdkRoot)) { $AndroidNdkRoot = $env:ANDROID_NDK_HOME }
if ([string]::IsNullOrWhiteSpace($AndroidNdkRoot)) { throw "Set ANDROID_NDK_ROOT or ANDROID_NDK_HOME." }

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $cmake -or -not $ninja) {
    $sdkRoot = $env:ANDROID_SDK_ROOT
    if ([string]::IsNullOrWhiteSpace($sdkRoot)) {
        $sdkRoot = Split-Path -Parent (Split-Path -Parent ([System.IO.Path]::GetFullPath($AndroidNdkRoot)))
    }
    $cmakeInstall = Get-ChildItem -LiteralPath (Join-Path $sdkRoot "cmake") -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    $extension = if ($IsWindows) { ".exe" } else { "" }
    if ($cmakeInstall) {
        $cmake = Get-Item -LiteralPath (Join-Path $cmakeInstall.FullName "bin\cmake$extension") -ErrorAction SilentlyContinue
        $ninja = Get-Item -LiteralPath (Join-Path $cmakeInstall.FullName "bin\ninja$extension") -ErrorAction SilentlyContinue
    }
}
if (-not $cmake -or -not $ninja) { throw "Install Android SDK CMake/Ninja or add them to PATH." }
$cmakePath = if ($cmake.Source) { $cmake.Source } else { $cmake.FullName }
$ninjaPath = if ($ninja.Source) { $ninja.Source } else { $ninja.FullName }
$buildRoot = Join-Path $repositoryRoot "build-output\android-arm64\$Configuration"
$outputRoot = Join-Path $repositoryRoot "dist\android-arm64\$Configuration"
New-Item -ItemType Directory -Force -Path $buildRoot, $outputRoot | Out-Null

& $cmakePath -S $repositoryRoot -B $buildRoot -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninjaPath" `
    "-DCMAKE_TOOLCHAIN_FILE=$AndroidNdkRoot/build/cmake/android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$AndroidApiLevel" `
    -DANDROID_STL=c++_shared "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw "Configuring Dobby failed." }
& $cmakePath --build $buildRoot --target dobby dobby_static
if ($LASTEXITCODE -ne 0) { throw "Building Dobby failed." }

$shared = Get-ChildItem -LiteralPath $buildRoot -Filter "libdobby.so" -File -Recurse | Select-Object -First 1
$static = Get-ChildItem -LiteralPath $buildRoot -Filter "libdobby.a" -File -Recurse | Select-Object -First 1
if (-not $shared -or -not $static) { throw "Dobby build outputs were not found." }
Copy-Item -LiteralPath $shared.FullName, $static.FullName -Destination $outputRoot -Force

$manifest = [ordered]@{
    formatVersion = 1
    platform = "android-arm64"
    configuration = $Configuration
    apiLevel = $AndroidApiLevel
    sourceRevision = (& git -C $repositoryRoot rev-parse HEAD).Trim()
    files = Get-ChildItem -LiteralPath $outputRoot -File | Sort-Object Name | ForEach-Object {
        [ordered]@{ name = $_.Name; size = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outputRoot "dobby-release.json") -Encoding Utf8
Write-Host "Published Dobby outputs: $outputRoot"
