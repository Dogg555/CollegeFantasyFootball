# Installs PostgreSQL and pgAdmin 4 on Windows 10/11 using winget.
# Run from an elevated PowerShell prompt:
#   powershell -ExecutionPolicy Bypass -File .\scripts\install_postgres_pgadmin.ps1

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Require-Admin {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($currentUser)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "This script must be run from an elevated PowerShell session (Run as Administrator)." -ForegroundColor Red
        Write-Host "Right-click PowerShell, choose 'Run as administrator', then rerun:" -ForegroundColor Yellow
        Write-Host "  powershell -ExecutionPolicy Bypass -File .\\scripts\\install_postgres_pgadmin.ps1" -ForegroundColor Yellow
        exit 1
    }
}

function Install-PackageIfNeeded {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][string]$Name
    )

    Write-Host "Installing $Name..."
    winget install --exact --id $Id --silent --accept-source-agreements --accept-package-agreements | Out-Null
    Write-Host "$Name installation attempted. If winget prompts appeared, rerun after approving them." -ForegroundColor Yellow
}

Require-Admin

Write-Host "`n[install] Installing PostgreSQL..." -ForegroundColor Cyan
Install-PackageIfNeeded -Id "PostgreSQL.PostgreSQL" -Name "PostgreSQL"

Write-Host "`n[install] Installing pgAdmin 4..." -ForegroundColor Cyan
Install-PackageIfNeeded -Id "pgAdmin.pgAdmin4" -Name "pgAdmin 4"

Write-Host "`n[install] Complete." -ForegroundColor Green
Write-Host "PostgreSQL service can be managed via Services (postgresql-x64-*) or 'services.msc'."
Write-Host "Launch pgAdmin 4 from the Start menu and set a master password on first run."
