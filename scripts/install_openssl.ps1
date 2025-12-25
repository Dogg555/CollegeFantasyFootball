$ErrorActionPreference = 'Stop'

function Install-WithWinget {
  $winget = Get-Command winget -ErrorAction SilentlyContinue
  if (-not $winget) {
    return $false
  }
  Write-Host 'Installing OpenSSL with winget...' -ForegroundColor Cyan
  & $winget.Path install --id ShiningLight.OpenSSL.Light -e --accept-source-agreements --accept-package-agreements
  return $true
}

function Install-WithChoco {
  $choco = Get-Command choco -ErrorAction SilentlyContinue
  if (-not $choco) {
    return $false
  }
  Write-Host 'Installing OpenSSL with Chocolatey...' -ForegroundColor Cyan
  & $choco.Path install openssl.light -y
  return $true
}

if (Get-Command openssl -ErrorAction SilentlyContinue) {
  Write-Host 'OpenSSL already installed.' -ForegroundColor Green
  exit 0
}

if (Install-WithWinget) {
  Write-Host 'OpenSSL install complete.' -ForegroundColor Green
  exit 0
}

if (Install-WithChoco) {
  Write-Host 'OpenSSL install complete.' -ForegroundColor Green
  exit 0
}

Write-Error 'OpenSSL install failed. Install winget or Chocolatey, or install OpenSSL manually and ensure it is on PATH.'
