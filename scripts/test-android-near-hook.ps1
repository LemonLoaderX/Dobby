[CmdletBinding()]
param(
    [string]$AndroidNdkRoot = $env:ANDROID_NDK_ROOT,
    [string]$AndroidSdkRoot = $env:ANDROID_SDK_ROOT,
    [string]$DeviceSerial,
    [int]$AndroidApiLevel = 23
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($AndroidNdkRoot)) {
    $AndroidNdkRoot = $env:ANDROID_NDK_HOME
}
if ([string]::IsNullOrWhiteSpace($AndroidNdkRoot)) {
    throw "Set ANDROID_NDK_ROOT or ANDROID_NDK_HOME."
}
if ([string]::IsNullOrWhiteSpace($AndroidSdkRoot)) {
    $AndroidSdkRoot = Split-Path -Parent (Split-Path -Parent ([IO.Path]::GetFullPath($AndroidNdkRoot)))
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$cmakeDirectory = Get-ChildItem -LiteralPath (Join-Path $AndroidSdkRoot "cmake") `
    -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $cmakeDirectory) {
    throw "Install CMake through the Android SDK."
}
$cmake = Join-Path $cmakeDirectory.FullName "bin\cmake.exe"
$ninja = Join-Path $cmakeDirectory.FullName "bin\ninja.exe"
$adb = Join-Path $AndroidSdkRoot "platform-tools\adb.exe"
foreach ($tool in @($cmake, $ninja, $adb)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required Android tool was not found at '$tool'."
    }
}

& $adb start-server | Out-Null
if ([string]::IsNullOrWhiteSpace($DeviceSerial)) {
    $devices = @(& $adb devices | Select-Object -Skip 1 | ForEach-Object {
        if ($_ -match '^([^\s]+)\s+device$') { $Matches[1] }
    })
    if ($devices.Count -ne 1) {
        throw "Pass -DeviceSerial when zero or multiple Android devices are connected."
    }
    $DeviceSerial = $devices[0]
}

$abiList = (& $adb -s $DeviceSerial shell getprop ro.product.cpu.abilist).Trim()
if ($LASTEXITCODE -ne 0 -or $abiList -notmatch '(^|,)arm64-v8a(,|$)') {
    throw "The selected device does not advertise arm64-v8a support."
}

$buildRoot = Join-Path $repositoryRoot "build-output\android-near-hook-test"
& $cmake -S $repositoryRoot -B $buildRoot -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$AndroidNdkRoot/build/cmake/android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a `
    "-DANDROID_PLATFORM=android-$AndroidApiLevel" `
    -DANDROID_STL=c++_static `
    -DCMAKE_BUILD_TYPE=Release `
    -DDOBBY_BUILD_ANDROID_NEAR_HOOK_TEST=ON
if ($LASTEXITCODE -ne 0) { throw "Configuring the Android near-hook test failed." }

& $cmake --build $buildRoot --target dobby_android_near_hook_test
if ($LASTEXITCODE -ne 0) { throw "Building the Android near-hook test failed." }
$testExecutable = Get-ChildItem -LiteralPath $buildRoot -File -Recurse |
    Where-Object Name -eq "dobby_android_near_hook_test" |
    Select-Object -First 1
if (-not $testExecutable) { throw "The Android near-hook test executable was not found." }

$remotePath = "/data/local/tmp/dobby_android_near_hook_test"
try {
    & $adb -s $DeviceSerial push $testExecutable.FullName $remotePath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Pushing the Android near-hook test failed." }
    & $adb -s $DeviceSerial shell chmod 755 $remotePath
    if ($LASTEXITCODE -ne 0) { throw "Preparing the Android near-hook test failed." }
    $output = (& $adb -s $DeviceSerial shell $remotePath) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $output -notmatch 'Dobby Android near-hook test passed') {
        throw "The Android near-hook test failed.`n$output"
    }
    Write-Host $output
}
finally {
    & $adb -s $DeviceSerial shell rm -f $remotePath 2>$null | Out-Null
}
