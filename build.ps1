# IM Chat Server Build Script (PowerShell)
# Usage: .\build.ps1 [Debug|Release]
#        .\build.ps1 clean

param(
    [Parameter(Position=0)]
    [string]$Target = "Debug",
    [Parameter(Position=1)]
    [ValidateSet("Debug", "Release", "debug", "release")]
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Stop"

# Resolve BuildType from positional or named param
if ($Target -in @("Debug", "Release", "debug", "release")) {
    $BuildType = $Target
    $Target = "build"
}

function Write-Success { Write-Host $args -ForegroundColor Green }
function Write-Info    { Write-Host $args -ForegroundColor Cyan }
function Write-Warn    { Write-Host $args -ForegroundColor Yellow }
function Write-Err     { Write-Host $args -ForegroundColor Red }

$ProjectRoot = $PSScriptRoot

function Build-Backend {
    param([string]$Type = "Debug")
    Write-Info "=== Building C++ Backend ($Type) ==="

    # Check that the toolchain is available in PATH
    foreach ($cmd in @("cmake", "ninja", "g++")) {
        if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
            Write-Err "Command not found: $cmd"
            Write-Err "Please ensure MinGW64 toolchain is in your PATH."
            Write-Err "Example: `$env:Path = 'C:\msys64\mingw64\bin;' + `$env:Path"
            return $false
        }
    }

    Write-Info "  CMake:  $(cmake --version | Select-Object -First 1)"
    Write-Info "  g++:    $(g++ -dumpversion)"
    Write-Info ""

    $buildDir = "$ProjectRoot\build"
    $cmakeArgs = @("-S", $ProjectRoot, "-B", $buildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Type")
    if ($Type -eq "Debug") { $cmakeArgs += "-DENABLE_LTO=OFF" }

    Write-Info "Configuring CMake ($Type)..."
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { Write-Err "CMake configure failed"; return $false }

    Write-Info "Compiling..."
    & cmake --build $buildDir --config $Type
    if ($LASTEXITCODE -ne 0) { Write-Err "Compile failed"; return $false }

    Write-Success "Build SUCCESS ($Type)"
    Write-Info "Binary: $buildDir\im_chat_server.exe"

    Copy-Item "$buildDir\im_chat_server.exe" "$ProjectRoot\im_chat_server.exe" -Force
    Write-Info "Copied to: $ProjectRoot\im_chat_server.exe"
    return $true
}

function Clean-Build {
    Write-Info "=== Cleaning build files ==="
    foreach ($dir in @("build", "build-Debug", "build-Release")) {
        if (Test-Path "$ProjectRoot\$dir") {
            Remove-Item -Recurse -Force "$ProjectRoot\$dir" -ErrorAction SilentlyContinue
        }
    }
    Remove-Item "$ProjectRoot\im_chat_server.exe" -ErrorAction SilentlyContinue
    Write-Success "Clean complete"
}

switch ($Target.ToLower()) {
    "build"  { Build-Backend -Type $BuildType }
    "clean"  { Clean-Build }
    default {
        Write-Warn "Unknown target: $Target. Use: build, clean"
        Write-Info "Example: .\build.ps1 Debug"
    }
}
