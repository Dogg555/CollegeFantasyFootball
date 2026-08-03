[CmdletBinding()]
param(
    [ValidateSet(4, 6)]
    [int]$Teams = 4,

    [ValidateRange(1, 100)]
    [int]$Iterations = 1,

    [int]$Seed = 42,

    [switch]$KeepEnvironment,
    [switch]$KeepData,
    [switch]$NoBuild,
    [switch]$SkipConcurrency
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

$ComposeArgs = @('compose', '-p', 'cff-sim', '-f', 'docker-compose.sim.yml')
$ApiPort = if ([string]::IsNullOrWhiteSpace($env:CFF_SIM_API_PORT)) { '18080' } else { $env:CFF_SIM_API_PORT }
$FrontendPort = if ([string]::IsNullOrWhiteSpace($env:CFF_SIM_FRONTEND_PORT)) { '13000' } else { $env:CFF_SIM_FRONTEND_PORT }
$ApiUrl = "http://127.0.0.1:$ApiPort"
$FrontendUrl = "http://127.0.0.1:$FrontendPort"

function Invoke-Compose {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    & docker @ComposeArgs @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose failed: $($Arguments -join ' ')"
    }
}

function Invoke-Simulator {
    $Arguments = @(
        'scripts/simulate_league.py',
        '--base-url', $ApiUrl,
        '--teams', [string]$Teams,
        '--iterations', [string]$Iterations,
        '--seed', [string]$Seed
    )
    if ($KeepData) { $Arguments += '--keep-data' }
    if ($SkipConcurrency) { $Arguments += '--skip-concurrency' }

    if (Get-Command py -ErrorAction SilentlyContinue) {
        & py -3 @Arguments
    }
    elseif (Get-Command python -ErrorAction SilentlyContinue) {
        & python @Arguments
    }
    else {
        throw 'Python 3 was not found. Install Python 3 or make py/python available on PATH.'
    }
    if ($LASTEXITCODE -ne 0) {
        throw "League simulator exited with code $LASTEXITCODE"
    }
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'Docker was not found. Install Docker Desktop and ensure it is running.'
}

$Succeeded = $false
try {
    Write-Host 'Starting disposable CFF simulation environment...' -ForegroundColor Cyan
    $UpArgs = @('up', '-d')
    if (-not $NoBuild) { $UpArgs += '--build' }
    Invoke-Compose @UpArgs

    Write-Host "Waiting for API health at $ApiUrl/health..." -ForegroundColor Cyan
    $Healthy = $false
    for ($Attempt = 1; $Attempt -le 60; $Attempt++) {
        try {
            $Health = Invoke-RestMethod -Uri "$ApiUrl/health" -TimeoutSec 4
            if ($Health.status -eq 'ok' -and $Health.database -eq 'ok') {
                $Healthy = $true
                break
            }
        }
        catch {
            Start-Sleep -Seconds 2
        }
    }
    if (-not $Healthy) {
        Invoke-Compose logs backend
        throw 'The local simulation API did not become healthy.'
    }

    Write-Host 'Seeding deterministic simulation players...' -ForegroundColor Cyan
    Invoke-Compose cp 'scripts/sim_seed.sql' 'postgres:/tmp/sim_seed.sql'
    Invoke-Compose exec -T postgres psql -U cff_sim -d cff_sim -v ON_ERROR_STOP=1 -f /tmp/sim_seed.sql

    Write-Host "Running $Teams-team lifecycle simulation ($Iterations iteration(s))..." -ForegroundColor Cyan
    Invoke-Simulator
    $Succeeded = $true

    Write-Host "Frontend: $FrontendUrl" -ForegroundColor Green
    Write-Host "API:      $ApiUrl" -ForegroundColor Green
}
finally {
    if ($KeepEnvironment) {
        Write-Host 'Simulation environment left running.' -ForegroundColor Yellow
        Write-Host 'Stop it with: docker compose -p cff-sim -f docker-compose.sim.yml down --volumes --remove-orphans'
    }
    else {
        Write-Host 'Stopping disposable simulation environment...' -ForegroundColor Cyan
        & docker @ComposeArgs down --volumes --remove-orphans
    }
}

if (-not $Succeeded) {
    exit 1
}
