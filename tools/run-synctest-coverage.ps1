<#
.SYNOPSIS
  Records a BM64_DIRECTOR session over a list of scenes, then replays it under SyncTest at each depth
  and reports PASS/FAIL plus the overlay ids the recording reached (from its hash log).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run-synctest-coverage.ps1 -Scenes "90,91,92" -Secs 30 -Out runs\st\arenas

  Writes <Out>\rec.bm64rec (from tools\run-director.ps1), <Out>\st<depth>\ for each replay, and <Out>\summary.txt.
  The recording embeds the save image, so keep it out of releases.
#>
param(
    [Parameter(Mandatory = $true)][string]$Scenes,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Secs = 30,
    [int[]]$Depths = @(1, 12),
    [string]$Seed = "",
    [switch]$SkipRecord
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $Repo
try {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $rec = Join-Path $Out "rec.bm64rec"
    if (-not $SkipRecord) {
        $director = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "run-director.ps1"), "-Scenes", $Scenes, "-Secs", "$Secs", "-Out", $Out)
        if ($Seed -ne "") {
            $director += @("-Seed", $Seed)
        }
        & powershell @director | Out-Host
    }
    if (-not (Test-Path $rec)) {
        throw "No recording at $rec"
    }

    $lines = @()
    $overlays = [ordered]@{}
    $csv = Join-Path $Out "hashes.csv"
    if (Test-Path $csv) {
        foreach ($row in (Import-Csv $csv)) {
            $id = [string]$row.overlay
            if ($overlays.Contains($id)) { $overlays[$id] = $overlays[$id] + 1 } else { $overlays.Add($id, 1) }
        }
        $lines += "overlays (id:ticks): " + (($overlays.Keys | ForEach-Object { "${_}:$($overlays[$_])" }) -join " ")
    }
    $dirLines = Select-String -Path (Join-Path $Out "stdout.txt") -Pattern "\[bm64 director\]|Exiting" -ErrorAction SilentlyContinue | ForEach-Object { $_.Line }
    $lines += $dirLines

    foreach ($d in $Depths) {
        $stOut = Join-Path $Out "st$d"
        $env:BM64_SYNCTEST = "$d"
        $env:BM64_SYNCTEST_DUMP = Join-Path (Resolve-Path $Out).Path "st$d\dumps"
        try {
            & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-replay.ps1") -Replay $rec -Out $stOut -TimeoutSec 7200 | Out-Null
        }
        finally {
            Remove-Item Env:BM64_SYNCTEST, Env:BM64_SYNCTEST_DUMP -ErrorAction SilentlyContinue
        }
        $result = Select-String -Path (Join-Path $stOut "stdout.txt") -Pattern "BM64 SYNCTEST|^  \S+ [0-9a-f]{16} [0-9a-f]{16}|window frame|speculative starts" | ForEach-Object { $_.Line }
        if (-not ($result -match "BM64 SYNCTEST (PASS|FAIL)")) {
            $result += "depth ${d}: no PASS or FAIL line (crash or timeout)"
        }
        $lines += "--- depth $d"
        $lines += $result
    }
    $lines | Set-Content (Join-Path $Out "summary.txt")
    $lines | Out-Host
}
finally {
    Pop-Location
}
