<#
.SYNOPSIS
  Cross-platform dependency bootstrapper for the College Fantasy Football backend.

.DESCRIPTION
  - Works on Windows, macOS, and Linux (PowerShell 7 recommended on non-Windows).
  - Ensures vcpkg is present and bootstrapped.
  - Installs common backend deps via vcpkg (drogon, openssl, jsoncpp, zlib)
  - Adds libuuid automatically ONLY on platforms where vcpkg supports it (typically Linux).
  - On Windows only: can (optionally) ensure CMake / Git exist (via PATH, winget, choco).
  - Logs output/errors and pauses at the end on Windows so the window doesn’t instantly close.
  - Fixes Git "detected dubious ownership" by adding vcpkg dir to git safe.directory when needed.
  - Runs vcpkg install with LIVE output so you can see download/build progress.

.NOTES
  - On macOS/Linux you’ll generally install compilers/CMake/Git via your system package manager.
  - This script does not assume you have any particular package manager on macOS/Linux.
#>

param(
  [string]$Triplet = "",
  [string]$VcpkgDir = "",
  [string]$LogDir = ""
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

Write-Host "Platform: $osDesc" -ForegroundColor Cyan
Write-Host "Arch    : $arch" -ForegroundColor Cyan
Write-Host "vcpkg dir: $VcpkgDir" -ForegroundColor Cyan
Write-Host "Triplet : $Triplet" -ForegroundColor Cyan
Write-Host "Log     : $LogFile" -ForegroundColor Cyan

# ----------------- Command runner (captures output) -----------------
function Run-Command {
  param(
    [Parameter(Mandatory=$true)][string]$Cmd,
    [Parameter(Mandatory=$true)][string[]]$Args,
    [switch]$AllowNonZeroExit
  )

  $argsOneLine = ($Args -join " ")
  Write-Host ""
  Write-Host "> $Cmd $argsOneLine" -ForegroundColor Gray

  $tmpOut = Join-Path ([System.IO.Path]::GetTempPath()) ("bootstrap_out_{0}.txt" -f ([guid]::NewGuid().ToString("N")))
  $tmpErr = Join-Path ([System.IO.Path]::GetTempPath()) ("bootstrap_err_{0}.txt" -f ([guid]::NewGuid().ToString("N")))

  $p = Start-Process -FilePath $Cmd -ArgumentList $Args -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $tmpOut -RedirectStandardError $tmpErr

  $outText = ""
  $errText = ""
  if (Test-Path $tmpOut) { $outText = Get-Content $tmpOut -Raw -ErrorAction SilentlyContinue }
  if (Test-Path $tmpErr) { $errText = Get-Content $tmpErr -Raw -ErrorAction SilentlyContinue }

  if ($outText) { Write-Host $outText }
  if ($errText) { Write-Host $errText -ForegroundColor DarkRed }

  Remove-Item $tmpOut, $tmpErr -ErrorAction SilentlyContinue

  if ($p.ExitCode -ne 0 -and -not $AllowNonZeroExit) {
    throw "Command failed with exit code $($p.ExitCode): $Cmd $argsOneLine"
  }

  return @{
    ExitCode = $p.ExitCode
    StdOut   = $outText
    StdErr   = $errText
  }
}

# ----------------- LIVE command runner (no output redirection) -----------------
function Run-CommandLive {
  param(
    [Parameter(Mandatory=$true)][string]$Cmd,
    [Parameter(Mandatory=$true)][string[]]$Args
  )
  $argsOneLine = ($Args -join " ")
  Write-Host ""
  Write-Host "> $Cmd $argsOneLine" -ForegroundColor Gray

  $p = Start-Process -FilePath $Cmd -ArgumentList $Args -NoNewWindow -Wait -PassThru
  if ($p.ExitCode -ne 0) {
    throw "Command failed with exit code $($p.ExitCode): $Cmd $argsOneLine"
  }
}

# ----------------- Git safe.directory helper -----------------
function Ensure-GitSafeDirectory {
  param([Parameter(Mandatory=$true)][string]$RepoPath)

  if (-not (Test-Path (Join-Path $RepoPath ".git"))) { return }

  $p = $RepoPath -replace '\\','/'

  $res = Run-Command -Cmd "git" -Args @("config","--global","--get-all","safe.directory") -AllowNonZeroExit
  $existing = ($res.StdOut + "`n" + $res.StdErr)

  if ($existing -match [regex]::Escape($p)) { return }

  Write-Host "Git safety: adding safe.directory for $p" -ForegroundColor Yellow
  Run-Command -Cmd "git" -Args @("config","--global","--add","safe.directory",$p)
}

# ----------------- Windows-only: optional tooling checks -----------------
function Ensure-ToolOnPath {
  param(
    [Parameter(Mandatory=$true)][string]$Tool,
    [string]$WingetId = "",
    [string]$ChocoId  = ""
  )

  if (Get-Command $Tool -ErrorAction SilentlyContinue) {
    Write-Host "$Tool already available on PATH. Skipping." -ForegroundColor Yellow
    return
  }

  if (-not $IsWin) {
    Write-Host "Warning: '$Tool' not found. Install it via your package manager then re-run." -ForegroundColor Yellow
    return
  }

  $hasWinget = (Get-Command winget -ErrorAction SilentlyContinue) -ne $null
  $hasChoco  = (Get-Command choco  -ErrorAction SilentlyContinue) -ne $null

  if ($hasWinget -and $WingetId) {
    $list = Run-Command -Cmd "winget" -Args @("list","--id",$WingetId,"-e") -AllowNonZeroExit
    if (($list.StdOut + "`n" + $list.StdErr) -match [regex]::Escape($WingetId)) {
      Write-Host "$Tool appears installed (winget list). You may need a new terminal for PATH." -ForegroundColor Yellow
      return
    }
    $install = Run-Command -Cmd "winget" -Args @("install","--id",$WingetId,"-e","--accept-package-agreements","--accept-source-agreements") -AllowNonZeroExit
    if ($install.ExitCode -ne 0) {
      Write-Host "winget install for $Tool failed. You can install it manually and re-run." -ForegroundColor Yellow
    }
  } elseif ($hasChoco -and $ChocoId) {
    Run-Command -Cmd "choco" -Args @("install",$ChocoId,"-y")
  } else {
    Write-Host "Warning: '$Tool' not found and no installer tool available. Install manually and re-run." -ForegroundColor Yellow
  }
}

if ($IsWin) {
  Ensure-ToolOnPath -Tool "git"   -WingetId "Git.Git"       -ChocoId "git"
  Ensure-ToolOnPath -Tool "cmake" -WingetId "Kitware.CMake" -ChocoId "cmake"

  if (-not (Get-Command cl -ErrorAction SilentlyContinue) -and -not (Get-Command msbuild -ErrorAction SilentlyContinue)) {
    Write-Host "Note: MSVC tools not detected on PATH. vcpkg may still work if Build Tools are installed (Developer Command Prompt helps)." -ForegroundColor Yellow
  }
} else {
  Write-Host "Non-Windows: Ensure compiler toolchain + CMake + Git are installed (e.g., build-essential/clang, cmake, git)." -ForegroundColor Yellow
}

# ----------------- Ensure git exists before cloning vcpkg -----------------
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  throw "git not found on PATH. Install Git and re-run."
}

# ----------------- vcpkg: clone/update -----------------
if (-not (Test-Path -Path $VcpkgDir)) {
  Write-Host "Cloning vcpkg into $VcpkgDir..."
  Run-CommandLive -Cmd "git" -Args @("clone","https://github.com/microsoft/vcpkg.git",$VcpkgDir)
  Ensure-GitSafeDirectory -RepoPath $VcpkgDir
} else {
  Ensure-GitSafeDirectory -RepoPath $VcpkgDir
  Write-Host "vcpkg already exists at $VcpkgDir. Pulling latest changes..."
  Push-Location $VcpkgDir
  Run-CommandLive -Cmd "git" -Args @("pull")
  Pop-Location
}

# ----------------- vcpkg: bootstrap -----------------
Push-Location $VcpkgDir

if ($IsWin) {
  $VcpkgExe = Join-Path $VcpkgDir "vcpkg.exe"
  if (-not (Test-Path $VcpkgExe)) {
    Write-Host "Bootstrapping vcpkg (Windows)..."
    if (Test-Path ".\bootstrap-vcpkg.bat") {
      Run-CommandLive -Cmd ".\bootstrap-vcpkg.bat" -Args @()
    } else {
      throw "bootstrap-vcpkg.bat not found. Make sure vcpkg clone succeeded."
    }
  } else {
    Write-Host "vcpkg executable found."
  }
} else {
  $VcpkgExe = Join-Path $VcpkgDir "vcpkg"
  if (-not (Test-Path $VcpkgExe)) {
    Write-Host "Bootstrapping vcpkg (macOS/Linux)..."
    if (Test-Path "./bootstrap-vcpkg.sh") {
      Run-CommandLive -Cmd "bash" -Args @("./bootstrap-vcpkg.sh")
    } else {
      throw "bootstrap-vcpkg.sh not found. Make sure vcpkg clone succeeded."
    }
  } else {
    Write-Host "vcpkg executable found."
  }
}

Pop-Location

# ----------------- vcpkg packages (cross-platform) -----------------
$packages = @("drogon", "openssl", "jsoncpp", "zlib")
if ($IsLinux) { $packages += "libuuid" }

Write-Host "Installing packages via vcpkg for triplet '$Triplet': $($packages -join ', ')"
Push-Location $VcpkgDir

# LIVE output here so you see progress
$pkgArgs = @("install") + ($packages | ForEach-Object { "{0}:{1}" -f $_, $Triplet })
Run-CommandLive -Cmd $VcpkgExe -Args $pkgArgs

Write-Host "Running 'vcpkg integrate install'..."
# integrate is useful on Windows; on mac/linux it's optional but harmless
Run-CommandLive -Cmd $VcpkgExe -Args @("integrate","install")

Pop-Location

Write-Host ""
Write-Host "All done." -ForegroundColor Green
Write-Host "Log saved to: $LogFile" -ForegroundColor Cyan
Write-Host ""
Write-Host "Next steps (CMake example):"
Write-Host "  cmake -S backend -B backend/build -DCMAKE_TOOLCHAIN_FILE=`"$VcpkgDir/scripts/buildsystems/vcpkg.cmake`""
Write-Host "  cmake --build backend/build"
Write-Host ""
Write-Host "Notes:"
Write-Host " - If you're on Apple Silicon, default triplet will be arm64-osx automatically."
Write-Host " - If a port fails, check vcpkg output in the log."

try { Stop-Transcript | Out-Null } catch {}
Pause-ForUser "Press Enter to exit..."
exit 0
