param(
    [string]$BackendUrl = "",
    [string]$FrontendUrl = "",
    [string]$CfbdApiKey = "",
    [string]$ResendApiKey = "",
    [string]$EmailFrom = "College Fantasy <no-reply@example.com>",
    [switch]$RequireEmailVerification,
    [switch]$RunSmokeTests,
    [switch]$WriteEnvChecklist
)

$ErrorActionPreference = "Stop"

function Require-File {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        throw "Required file not found: $Path"
    }
}

function Require-Text {
    param([string]$Path, [string]$Pattern, [string]$Message)
    $content = Get-Content $Path -Raw
    if ($content -notmatch $Pattern) {
        throw $Message
    }
}

function Normalize-Origin {
    param([string]$Url)
    if ([string]::IsNullOrWhiteSpace($Url)) {
        return ""
    }
    return $Url.Trim().TrimEnd("/")
}

function Normalize-ApiBase {
    param([string]$Url)
    $clean = Normalize-Origin $Url
    if ([string]::IsNullOrWhiteSpace($clean)) {
        return ""
    }
    if ($clean -notmatch "/api$") {
        return "$clean/api"
    }
    return $clean
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $repoRoot

Require-File "render.yaml"
Require-File "backend/Dockerfile"
Require-File "scripts/render-build-frontend.sh"
Require-File "scripts/api_smoke_tests.py"

Require-Text "render.yaml" "name:\s*college-ff-api" "render.yaml is missing the college-ff-api service."
Require-Text "render.yaml" "name:\s*college-ff-frontend" "render.yaml is missing the college-ff-frontend service."
Require-Text "render.yaml" "name:\s*college-ff-db" "render.yaml is missing the college-ff-db database."
Require-Text "render.yaml" "dockerContext:\s*\./backend" "render.yaml should set dockerContext: ./backend for the API Docker build."
Require-Text "render.yaml" "staticPublishPath:\s*\./frontend-dist" "render.yaml should publish frontend-dist for the static frontend."

$backendOrigin = Normalize-Origin $BackendUrl
$frontendOrigin = Normalize-Origin $FrontendUrl
$frontendApiBase = Normalize-ApiBase $BackendUrl
$emailVerification = if ($RequireEmailVerification) { "true" } else { "false" }

Write-Host ""
Write-Host "Render blueprint services expected from render.yaml:"
Write-Host "  - college-ff-api       Docker web service"
Write-Host "  - college-ff-frontend  Static web service"
Write-Host "  - college-ff-db        Managed Postgres"

Write-Host ""
Write-Host "Before creating/updating the Blueprint:"
Write-Host "  1. Commit and push the current branch to GitHub."
Write-Host "  2. In Render, create or update the Blueprint from this repo."
Write-Host "  3. After Render shows service URLs, run this script again with -BackendUrl and -FrontendUrl."

if ([string]::IsNullOrWhiteSpace($backendOrigin) -or [string]::IsNullOrWhiteSpace($frontendOrigin)) {
    Write-Host ""
    Write-Host "Example after services exist:"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts/render-deploy-helper.ps1 ``"
    Write-Host "    -BackendUrl https://college-ff-api.onrender.com ``"
    Write-Host "    -FrontendUrl https://college-ff-frontend.onrender.com ``"
    Write-Host "    -WriteEnvChecklist"
    Write-Host ""
    Write-Host "No URLs provided, so env checklist generation is skipped."
    exit 0
}

$backendEnv = [ordered]@{
    "ALLOWED_ORIGINS" = $frontendOrigin
    "CFBD_API_KEY" = $CfbdApiKey
    "RESEND_API_KEY" = $ResendApiKey
    "CFF_EMAIL_FROM" = $EmailFrom
    "CFF_FRONTEND_BASE_URL" = $frontendOrigin
    "CFF_REQUIRE_DB" = "true"
    "CFF_ALLOW_SHARED_SECRET_AUTH" = "false"
    "CFF_REQUIRE_EMAIL_VERIFICATION" = $emailVerification
    "CFF_EXPOSE_AUTH_TOKENS" = "false"
    "CFF_LOG_AUTH_TOKENS" = "false"
}

$frontendEnv = [ordered]@{
    "CFF_API_BASE" = $frontendApiBase
    "CFF_ALLOW_LOCAL_DEMO" = "false"
}

Write-Host ""
Write-Host "Set these on Render service: college-ff-api"
foreach ($item in $backendEnv.GetEnumerator()) {
    if ($item.Key -match "KEY$" -and [string]::IsNullOrWhiteSpace($item.Value)) {
        Write-Host ("  {0}=<set in Render dashboard>" -f $item.Key)
    } else {
        Write-Host ("  {0}={1}" -f $item.Key, $item.Value)
    }
}

Write-Host ""
Write-Host "Set these on Render service: college-ff-frontend"
foreach ($item in $frontendEnv.GetEnumerator()) {
    Write-Host ("  {0}={1}" -f $item.Key, $item.Value)
}

if ($WriteEnvChecklist) {
    $outPath = Join-Path $repoRoot "render-env-checklist.local.txt"
    $lines = @()
    $lines += "college-ff-api"
    foreach ($item in $backendEnv.GetEnumerator()) {
        $value = if ($item.Key -match "KEY$" -and [string]::IsNullOrWhiteSpace($item.Value)) { "<set in Render dashboard>" } else { $item.Value }
        $lines += ("{0}={1}" -f $item.Key, $value)
    }
    $lines += ""
    $lines += "college-ff-frontend"
    foreach ($item in $frontendEnv.GetEnumerator()) {
        $lines += ("{0}={1}" -f $item.Key, $item.Value)
    }
    Set-Content -Path $outPath -Value $lines
    Write-Host ""
    Write-Host "Wrote local checklist: render-env-checklist.local.txt"
}

Write-Host ""
Write-Host "After setting env vars, redeploy both services in Render."
Write-Host "Backend health checks:"
Write-Host "  $backendOrigin/health"
Write-Host "  $backendOrigin/api/health"
Write-Host "Frontend:"
Write-Host "  $frontendOrigin"

if ($RunSmokeTests) {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        $python = Get-Command py -ErrorAction SilentlyContinue
    }
    if (-not $python) {
        throw "Python was not found. Install Python or run scripts/api_smoke_tests.py from another machine."
    }
    $env:CFF_API_BASE_URL = $backendOrigin
    & $python.Source scripts/api_smoke_tests.py
}
