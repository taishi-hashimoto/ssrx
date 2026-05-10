param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$SsrxRoot = "",
    [string]$VcpkgRoot = "",
    [string]$HackRFSource = "",
    [string]$HackRFInstall = "",
    [string]$UhdRoot = "",
    [string]$UhdDir = "",

    [ValidateSet("", "Visual Studio 18 2026", "Visual Studio 17 2022")]
    [string]$Generator = "",

    [switch]$SkipHackRFHost,
    [switch]$EnableUSRP,
    [switch]$SkipTests,
    [switch]$Fresh
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Resolve-FullPath {
    param([string]$Path)

    $executionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Find-Executable {
    param(
        [string]$Name,
        [string[]]$Candidates = @()
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    throw "Could not find $Name. Add it to PATH or install the required tool."
}

function Select-VisualStudioGenerator {
    param(
        [string]$CMakeExe,
        [string]$RequestedGenerator
    )

    if ($RequestedGenerator) {
        return $RequestedGenerator
    }

    $help = & $CMakeExe --help
    if ($help -match "Visual Studio 18 2026") {
        return "Visual Studio 18 2026"
    }
    if ($help -match "Visual Studio 17 2022") {
        return "Visual Studio 17 2022"
    }

    throw "Could not find a Visual Studio CMake generator. Install Visual Studio Build Tools with the C++ workload."
}

function Get-PresetName {
    param(
        [string]$SelectedGenerator,
        [string]$BuildConfiguration
    )

    $configSuffix = $BuildConfiguration.ToLowerInvariant()
    if ($SelectedGenerator -eq "Visual Studio 17 2022") {
        return "windows-msvc-vcpkg-vs2022-$configSuffix"
    }

    return "windows-msvc-vcpkg-$configSuffix"
}

function Assert-Path {
    param(
        [string]$Path,
        [string]$Message
    )

    if (-not (Test-Path $Path)) {
        throw "$Message`nMissing path: $Path"
    }
}

function Assert-AnyPath {
    param(
        [string[]]$Paths,
        [string]$Message
    )

    foreach ($path in $Paths) {
        if (Test-Path $path) {
            return
        }
    }

    throw "$Message`nTried:`n  $($Paths -join "`n  ")"
}

function Invoke-Logged {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    Write-Host "+ $FilePath $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE."
    }
}

if (-not $SsrxRoot) {
    $SsrxRoot = Join-Path $PSScriptRoot ".."
}
$SsrxRoot = Resolve-FullPath $SsrxRoot

if (-not $VcpkgRoot) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } else {
        $VcpkgRoot = "C:\vcpkg"
    }
}
$VcpkgRoot = Resolve-FullPath $VcpkgRoot

if (-not $SkipHackRFHost) {
    if (-not $HackRFSource) {
        $HackRFSource = Join-Path (Split-Path $SsrxRoot -Parent) "hackrf"
    }
    $HackRFSource = Resolve-FullPath $HackRFSource
} elseif ($HackRFSource) {
    $HackRFSource = Resolve-FullPath $HackRFSource
}

if (-not $HackRFInstall) {
    $HackRFInstall = Join-Path $SsrxRoot ".deps\hackrf"
}
$HackRFInstall = Resolve-FullPath $HackRFInstall

if ($UhdRoot) {
    $UhdRoot = Resolve-FullPath $UhdRoot
    Assert-Path $UhdRoot "UHD root was not found."
}

if ($UhdDir) {
    $UhdDir = Resolve-FullPath $UhdDir
    Assert-Path $UhdDir "UHD CMake package directory was not found."
}

$CMakeExe = Find-Executable "cmake" @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\17\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

$CMakeDir = Split-Path $CMakeExe -Parent
$CTestCandidate = Join-Path $CMakeDir "ctest.exe"
if (Test-Path $CTestCandidate) {
    $CTestExe = $CTestCandidate
} else {
    $CTestExe = Find-Executable "ctest"
}

$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
$ToolchainFile = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
Assert-Path $VcpkgExe "vcpkg.exe was not found. Set -VcpkgRoot or VCPKG_ROOT."
Assert-Path $ToolchainFile "The vcpkg CMake toolchain was not found. Set -VcpkgRoot or VCPKG_ROOT."

$SelectedGenerator = Select-VisualStudioGenerator $CMakeExe $Generator
$Preset = Get-PresetName $SelectedGenerator $Configuration
$BuildDir = Join-Path $SsrxRoot "build"
$Triplet = "x64-windows"
$UsrpCacheValue = if ($EnableUSRP) { "ON" } else { "OFF" }
$VcpkgFeatureArgs = @()
if ($EnableUSRP) {
    $VcpkgFeatureArgs += "-DVCPKG_MANIFEST_FEATURES=usrp"
}

$env:VCPKG_ROOT = $VcpkgRoot
$env:PATH = "$VcpkgRoot;$env:PATH"

if ($UhdRoot) {
    $env:UHD_ROOT = $UhdRoot
    $env:PATH = "$(Join-Path $UhdRoot "bin");$env:PATH"
}
if ($UhdDir) {
    $env:UHD_DIR = $UhdDir
}

Write-Host "ssrx root:       $SsrxRoot"
Write-Host "vcpkg root:      $VcpkgRoot"
if ($SkipHackRFHost) {
    Write-Host "HackRF source:   skipped"
} else {
    Write-Host "HackRF source:   $HackRFSource"
}
Write-Host "HackRF install:  $HackRFInstall"
if ($EnableUSRP) {
    Write-Host "USRP/UHD:        enabled"
    if ($UhdRoot) {
        Write-Host "UHD root:        $UhdRoot"
    }
    if ($UhdDir) {
        Write-Host "UHD CMake dir:   $UhdDir"
    }
} else {
    Write-Host "USRP/UHD:        disabled"
}
Write-Host "Generator:       $SelectedGenerator"
Write-Host "Preset:          $Preset"
Write-Host "Configuration:   $Configuration"

Write-Step "Configure ssrx without HackRF to install vcpkg manifest dependencies"
$configureBase = @(
    "-S", $SsrxRoot,
    "--preset", $Preset,
    "-DSSRX_BUILD_HACKRF=OFF",
    "-DSSRX_BUILD_USRP=OFF"
)
$configureBase += $VcpkgFeatureArgs
if ($Fresh) {
    $configureBase += "--fresh"
}
Invoke-Logged $CMakeExe $configureBase

$VcpkgInstalledRoot = Join-Path $BuildDir "vcpkg_installed"
$TripletRoot = Join-Path $VcpkgInstalledRoot $Triplet
if (-not (Test-Path $TripletRoot)) {
    $VcpkgInstalledRoot = Join-Path $SsrxRoot "vcpkg_installed"
    $TripletRoot = Join-Path $VcpkgInstalledRoot $Triplet
}
Assert-Path $TripletRoot "vcpkg dependencies were not installed for $Triplet."

Write-Step "Build ssrx without HackRF"
Invoke-Logged $CMakeExe @(
    "--build", $BuildDir,
    "--config", $Configuration
)

if (-not $SkipTests) {
    Write-Step "Run CTest without HackRF"
    Invoke-Logged $CTestExe @(
        "--test-dir", $BuildDir,
        "--output-on-failure",
        "-C", $Configuration
    )
}

$env:PATH = "$(Join-Path $TripletRoot "bin");$env:PATH"

if (-not $SkipHackRFHost) {
    $HackRFHostSource = Join-Path $HackRFSource "host"
    $HackRFBuild = Join-Path $HackRFHostSource "build"
    Assert-Path $HackRFHostSource "HackRF host source was not found. Clone HackRF or pass -HackRFSource."

    New-Item -ItemType Directory -Force $HackRFInstall | Out-Null

    $LibusbInclude = Join-Path $TripletRoot "include\libusb-1.0"
    $LibusbLibrary = Join-Path $TripletRoot "lib\libusb-1.0.lib"
    $FftwInclude = Join-Path $TripletRoot "include"
    $FftwLibrary = Join-Path $TripletRoot "lib\fftw3f.lib"
    $PThreads4WDir = Join-Path $TripletRoot "share\PThreads4W"

    Assert-Path $LibusbInclude "libusb headers were not found in vcpkg."
    Assert-Path $LibusbLibrary "libusb import library was not found in vcpkg."
    Assert-Path $FftwLibrary "FFTW3f import library was not found in vcpkg."
    Assert-Path $PThreads4WDir "PThreads4W CMake package was not found in vcpkg."

    $env:FFTW3_DIR = $TripletRoot
    $env:PKG_CONFIG_PATH = "$(Join-Path $TripletRoot "lib\pkgconfig");$(Join-Path $TripletRoot "share\pkgconfig")"

    Write-Step "Configure HackRF host with vcpkg dependencies"
    $hackrfConfigure = @(
        "-S", $HackRFHostSource,
        "-B", $HackRFBuild,
        "-G", $SelectedGenerator,
        "-A", "x64",
        "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile",
        "-DVCPKG_TARGET_TRIPLET=$Triplet",
        "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledRoot",
        "-DCMAKE_PREFIX_PATH=$TripletRoot",
        "-DCMAKE_INSTALL_PREFIX=$HackRFInstall",
        "-DLIBUSB_INCLUDE_DIR=$LibusbInclude",
        "-DLIBUSB_LIBRARIES=$LibusbLibrary",
        "-DFFTW3f_INCLUDE_DIRS=$FftwInclude",
        "-DFFTW3f_LIBRARIES=$FftwLibrary",
        "-DPThreads4W_DIR=$PThreads4WDir"
    )
    if ($Fresh) {
        $hackrfConfigure += "--fresh"
    }
    Invoke-Logged $CMakeExe $hackrfConfigure

    Write-Step "Build HackRF host"
    Invoke-Logged $CMakeExe @(
        "--build", $HackRFBuild,
        "--config", $Configuration
    )

    Write-Step "Install HackRF host into project-local .deps"
    $hackrfInstallArgs = @(
        "--install", $HackRFBuild,
        "--config", $Configuration
    )
    Write-Host "+ $CMakeExe $($hackrfInstallArgs -join ' ')" -ForegroundColor DarkGray
    & $CMakeExe @hackrfInstallArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "HackRF install reported exit code $LASTEXITCODE. Checking whether the required installed files are present."
    }
} else {
    Write-Step "Skip HackRF host build"
}

Assert-Path (Join-Path $HackRFInstall "include\libhackrf\hackrf.h") "HackRF headers were not found."
Assert-AnyPath @(
    (Join-Path $HackRFInstall "bin\hackrf.lib"),
    (Join-Path $HackRFInstall "bin\libhackrf.lib"),
    (Join-Path $HackRFInstall "lib\hackrf.lib"),
    (Join-Path $HackRFInstall "lib\libhackrf.lib")
) "HackRF import library was not found."
Assert-AnyPath @(
    (Join-Path $HackRFInstall "bin\hackrf.dll"),
    (Join-Path $HackRFInstall "bin\libhackrf.dll")
) "HackRF runtime DLL was not found."

$env:HACKRF_ROOT = $HackRFInstall
$env:PATH = "$(Join-Path $HackRFInstall "bin");$env:PATH"

Write-Step "Configure ssrx with HackRF, rbstat, and optional USRP"
$configureWithHackRF = @(
    "-S", $SsrxRoot,
    "--preset", $Preset,
    "-DHACKRF_ROOT=$HackRFInstall",
    "-DSSRX_BUILD_HACKRF=ON",
    "-DSSRX_BUILD_RBSTAT=ON",
    "-DSSRX_BUILD_USRP=$UsrpCacheValue"
)
$configureWithHackRF += $VcpkgFeatureArgs
if ($EnableUSRP -and $UhdRoot) {
    $configureWithHackRF += "-DUHD_ROOT=$UhdRoot"
}
if ($EnableUSRP -and $UhdDir) {
    $configureWithHackRF += "-DUHD_DIR=$UhdDir"
}
if ($Fresh) {
    $configureWithHackRF += "--fresh"
}
Invoke-Logged $CMakeExe $configureWithHackRF

Write-Step "Build ssrx with HackRF, rbstat, and optional USRP"
Invoke-Logged $CMakeExe @(
    "--build", $BuildDir,
    "--config", $Configuration
)

if (-not $SkipTests) {
    Write-Step "Run CTest with HackRF-enabled build tree"
    Invoke-Logged $CTestExe @(
        "--test-dir", $BuildDir,
        "--output-on-failure",
        "-C", $Configuration
    )
}

$OutDir = Join-Path $BuildDir $Configuration
if ($EnableUSRP) {
    Assert-Path (Join-Path $OutDir "ssrx-usrp.exe") "USRP/UHD build was requested, but ssrx-usrp.exe was not produced. Check -UhdRoot, -UhdDir, or UHD_DIR."
}

Write-Step "Done"
Write-Host "Built: $OutDir\ssrx-hackrf.exe"
Write-Host "Built: $OutDir\ssrx-rbstat.exe"
if ($EnableUSRP) {
    Write-Host "Built: $OutDir\ssrx-usrp.exe"
}
Write-Host ""
Write-Host "Smoke-test examples:"
Write-Host "  & `"$OutDir\ssrx-hackrf.exe`" --help"
Write-Host "  & `"$OutDir\ssrx-hackrf.exe`" --version"
Write-Host "  & `"$OutDir\ssrx-hackrf.exe`" --test --no-pps `"$SsrxRoot\conf\ssrx.yaml`""
if ($EnableUSRP) {
    Write-Host "  & `"$OutDir\ssrx-usrp.exe`" --help"
}
