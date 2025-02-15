# build.ps1

# Set the Visual Studio Build Tools path and locate the Developer Shell
$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$vsDevShell = "$vsPath\Common7\Tools\Launch-VsDevShell.ps1"

if (!(Test-Path $vsDevShell)) {
    Write-Error "Could not find VS Developer Shell at: $vsDevShell"
    exit 1
}

Write-Host "Initializing VS Developer Shell..."
& $vsDevShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

# Create or clean the build directory
if (!(Test-Path "build")) {
    Write-Host "Creating build directory..."
    New-Item -ItemType Directory -Path "build" | Out-Null
} else {
    Write-Host "Build directory exists. Cleaning up..."
    Remove-Item -Recurse -Force "build" -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path "build" | Out-Null
}

# Configure the project with CMake for x64
Write-Host "Configuring CMake..."
cmake -B build -A x64
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed."
    exit 1
}

# Build the project in Release configuration
Write-Host "Building the project..."
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed."
    exit 1
}

Write-Host "Build completed successfully."

# Move the executable from build\Release to the project root
$exeName = "ProjectCompressor.exe"
$builtExePath = Join-Path (Resolve-Path "build\Release") $exeName

if (Test-Path $builtExePath) {
    Copy-Item $builtExePath -Destination ".\$exeName" -Force
    Write-Host "Executable moved to project root: .\$exeName"
} else {
    Write-Error "Could not find built executable at: $builtExePath"
}
