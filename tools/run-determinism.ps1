<#
.SYNOPSIS
  Replays one recording twice and checks that the hash logs match and the user's save is unchanged.

.DESCRIPTION
  A third replay with -CameraBias changes only the lookAt matrix. Its non-gfx columns are compared
  with the first replay: the gfx column includes that matrix, so it is allowed to differ.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run-determinism.ps1 -Replay new.bm64rec -Out runs\det-new
#>
param(
    [Parameter(Mandatory = $true)][string]$Replay,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Exe = "build\BM64Recompiled.exe",
    [string]$CameraBias = "250,40,-80",
    [int]$TimeoutSec = 900
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Save = Join-Path $env:LOCALAPPDATA "BM64Recompiled\saves\bm64_us.bin"
if (-not (Test-Path $Save)) {
    throw "Save file not found: $Save"
}

function Invoke-Replay([string]$Name, [string]$Bias) {
    $dir = Join-Path $Out $Name
    $export = Join-Path $dir "sandbox.bin"
    & (Join-Path $PSScriptRoot "run-replay.ps1") -Replay $Replay -Out $dir -Exe $Exe -TimeoutSec $TimeoutSec -CameraBias $Bias -SandboxExport $export
    if ($LASTEXITCODE -ne 0) { throw "Replay $Name failed ($LASTEXITCODE)" }
    if (-not (Test-Path $export)) { throw "Sandbox export missing: $export" }
    return $dir
}

Push-Location $Repo
try {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $before = (Get-FileHash $Save -Algorithm SHA256).Hash
    $beforeStamp = (Get-Item $Save).LastWriteTimeUtc

    $a = Invoke-Replay "a" ""
    $mid = (Get-FileHash $Save -Algorithm SHA256).Hash
    if ($mid -ne $before) { throw "Save file changed during replay a" }

    $b = Invoke-Replay "b" ""
    $after = (Get-FileHash $Save -Algorithm SHA256).Hash
    $afterStamp = (Get-Item $Save).LastWriteTimeUtc
    if ($after -ne $before) { throw "Save file changed during replay b" }
    if ($afterStamp -ne $beforeStamp) { throw "Save file timestamp changed ($beforeStamp -> $afterStamp)" }

    Write-Host "==> comparing a and b"
    python (Join-Path $Repo "tools\re\compare_hashes.py") (Join-Path $a "hashes.csv") (Join-Path $b "hashes.csv")
    if ($LASTEXITCODE -ne 0) { throw "Replays a and b diverged" }

    $aBin = [IO.File]::ReadAllBytes((Join-Path $a "sandbox.bin"))
    $bBin = [IO.File]::ReadAllBytes((Join-Path $b "sandbox.bin"))
    if ($aBin.Length -ne $bBin.Length) { throw "Sandbox exports differ in size" }
    $sameImage = $true
    for ($i = 0; $i -lt $aBin.Length; $i++) {
        if ($aBin[$i] -ne $bBin[$i]) { $sameImage = $false; break }
    }
    if (-not $sameImage) { throw "Sandbox images differ between a and b" }
    Write-Host "Sandbox images match ($($aBin.Length) bytes). User save unchanged."

    if ($CameraBias) {
        $c = Invoke-Replay "camera" $CameraBias
        Write-Host "==> comparing a and camera"
        $cmp = python (Join-Path $Repo "tools\re\compare_hashes.py") (Join-Path $a "hashes.csv") (Join-Path $c "hashes.csv")
        $cmp | Write-Host
        $bad = @($cmp | Where-Object { $_ -match 'first differs' -and $_ -notmatch '^(total|gfx)\s' })
        if ($bad.Count -gt 0) {
            Write-Host "Camera bias changed simulation state:"
            $bad | Write-Host
            exit 2
        }
        Write-Host "Camera bias left every range other than gfx and total unchanged."
    }
}
finally {
    Pop-Location
}
