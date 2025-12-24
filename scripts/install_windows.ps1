<#
.SYNOPSIS
  Installs build tools and project dependencies on Windows for the College Fantasy Football backend.

.DESCRIPTION
  - Ensures script is running elevated (re-launches with elevation if needed).
  - Installs Visual Studio Build Tools (MSVC), CMake and Git using winget or Chocolatey if available.
  - Installs vcpkg (if missing), bootstraps it, and uses it to install Drogon, OpenSSL, jsoncpp, zlib and libuuid for the specified triplet.
  - Runs `vcpkg integrate install` so CMake can find vcpkg-installed libraries.
  - Prints the CMake command to use the vcpkg toolchain.

.NOTES
  Tested for typical Windows 10/11 environments. If you prefer to install tools manually (Visual Studio, CMake, Git), you can skip the automated installer portion.
#>

param(
  [string]$Triplet = "x64-windows",
  [string]$VcpkgDir = "$env:USERPROFILE\\vcpkg"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Is-Administrator {
  $current = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  return $current.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
}

if (-not (Is-Administrator)) {
  Write-Host "Elevating to administrator..." -ForegroundColor Yellow
  $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath)
  foreach ($kv in $MyInvocation.BoundParameters.GetEnumerator()) {
    $argList += ("-$($kv.Key)")
    $argList += ("$($kv.Value)")
  }

  # prefer pwsh (PowerShell Core), fall back to Windows PowerShell (powershell.exe)
  $elevExe = (Get-Command pwsh -ErrorAction SilentlyContinue).Path
  if (-not $elevExe) { $elevExe = (Get-Command powershell -ErrorAction SilentlyContinue).Path }

  if (-not $elevExe) {
    Write-Host "Error: neither 'pwsh' nor 'powershell' was found to re-launch the script elevated. Run this script from an elevated session or install PowerShell Core." -ForegroundColor Red
    exit 1
  }

  Start-Process -FilePath $elevExe -ArgumentList $argList -Verb RunAs -Wait
  exit
}

Write-Host "Running as administrator. Beginning dependency installation..." -ForegroundColor Green

function Run-Command($cmd, $args) {
  Write-Host "> $cmd $args"
  Start-Process -FilePath $cmd -ArgumentList $args -NoNewWindow -Wait
}

$hasWinget = (Get-Command winget -ErrorAction SilentlyContinue) -ne $null
$hasChoco  = (Get-Command choco  -ErrorAction SilentlyContinue) -ne $null

if (-not $hasWinget -and -not $hasChoco) {
  Write-Host ""
  Write-Host "Warning: neither 'winget' nor 'choco' was found. The script will still install vcpkg, but you'll need to install Visual Studio Build Tools, CMake, and Git manually." -ForegroundColor Yellow
  Write-Host "Manual install links:"
  Write-Host " - Visual Studio Build Tools: https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022"
  Write-Host " - CMake: https://cmake.org/download/"
  Write-Host " - Git: https://git-scm.com/download/win"
} else {
  if ($hasWinget) {
    Write-Host "Using winget to install Visual Studio Build Tools, CMake, and Git..."
    Run-Command winget "install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements"
    Run-Command winget "install --id Kitware.CMake -e --accept-package-agreements --accept-source-agreements"
    Run-Command winget "install --id Git.Git -e --accept-package-agreements --accept-source-agreements"
  } elseif ($hasChoco) {
    Write-Host "Using Chocolatey to install Visual Studio Build Tools, CMake, and Git..."
    Run-Command choco "install visualstudio2019buildtools -y --ignore-checksums"
    Run-Command choco "install cmake -y --installargs 'ADD_CMAKE_TO_PATH=System'"
    Run-Command choco "install git -y"
  }
}

if (-not (Get-Command msbuild -ErrorAction SilentlyContinue) -and -not (Get-Command cl -ErrorAction SilentlyContinue)) {
  Write-Host "Warning: MSVC build tools don't appear to be on PATH. Make sure Visual Studio Build Tools are installed and Developer Command Prompt or Build Tools paths are available." -ForegroundColor Yellow
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  Write-Host "Error: 'git' was not found on PATH. Install Git or ensure it is available before continuing." -ForegroundColor Red
  exit 1
}

if (-not (Test-Path -Path $VcpkgDir)) {
  Write-Host "Cloning vcpkg into $VcpkgDir..."
  git clone https://github.com/microsoft/vcpkg.git $VcpkgDir
} else {
  Write-Host "vcpkg already exists at $VcpkgDir. Pulling latest changes..."
  Push-Location $VcpkgDir
  git pull
  Pop-Location
}

$VcpkgExe = Join-Path $VcpkgDir "vcpkg.exe"
if (-not (Test-Path $VcpkgExe)) {
  Write-Host "Bootstrapping vcpkg..."
  Push-Location $VcpkgDir
  if (Test-Path ".\\bootstrap-vcpkg.bat") {
    & .\\bootstrap-vcpkg.bat
  } else {
    Write-Host "bootstrap-vcpkg.bat not found. Make sure Git clone succeeded." -ForegroundColor Red
    exit 1
  }
  Pop-Location
} else {
  Write-Host "vcpkg executable found."
}

$packages = @("drogon", "openssl", "jsoncpp", "zlib", "libuuid")
Write-Host "Installing packages via vcpkg for triplet '$Triplet': $($packages -join ', ')"
Push-Location $VcpkgDir

# build proper argument list for vcpkg install (avoid \"$:_:$Triplet\" interpolation issue)
$pkgArgs = $packages | ForEach-Object { "{0}:{1}" -f $_, $Triplet }

# invoke vcpkg
& $VcpkgExe install @pkgArgs

Write-Host "Running 'vcpkg integrate install' to make vcpkg available to CMake projects..."
& $VcpkgExe integrate install
Pop-Location

Write-Host ""
Write-Host "All done." -ForegroundColor Green
Write-Host "Next steps:"
Write-Host " 1) Configure CMake using the vcpkg toolchain (example):"
Write-Host "      cmake -S backend -B backend\\build -DDROGON_FOUND=ON -DCMAKE_TOOLCHAIN_FILE=`"$VcpkgDir\\scripts\\buildsystems\\vcpkg.cmake`" -A x64"
Write-Host " 2) Build the project (example):"
Write-Host "      cmake --build backend\\build --config Release"
Write-Host ""
Write-Host "Notes:"
Write-Host " - If you prefer using another triplet (e.g. x64-windows-static), re-run this script with -Triplet 'x64-windows-static'."
Write-Host " - If any vcpkg ports fail to build (especially drogon), check the vcpkg repo issues or try a different Visual Studio workload (Desktop development with C++)."
