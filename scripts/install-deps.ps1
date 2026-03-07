#Requires -RunAsAdministrator
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Write-Host "Installing dependencies for Windows..."

if (Get-Command winget -ErrorAction SilentlyContinue) {
    Write-Host "Using winget..."
    winget install --id Kitware.CMake --accept-package-agreements --accept-source-agreements
    winget install --id OpenCppCoverage.OpenCppCoverage --accept-package-agreements --accept-source-agreements
    winget install --id LLVM.LLVM --accept-package-agreements --accept-source-agreements
    winget install --id Microsoft.WinDbg --accept-package-agreements --accept-source-agreements
} elseif (Get-Command choco -ErrorAction SilentlyContinue) {
    Write-Host "Using choco..."
    choco install -y cmake opencppcoverage llvm windbg
} else {
    Write-Error "Neither winget nor choco found. Please install one of them first."
}

Write-Host ""
Write-Host "Note: Visual Studio 2022 with C++ workload must be installed separately."
Write-Host "Done."
