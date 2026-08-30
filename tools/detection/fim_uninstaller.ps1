# fim_uninstaller.ps1 - Uninstaller Manager FIM + app security-event collector (P0, no admin)
# Usage: powershell -ExecutionPolicy Bypass -File fim_uninstaller.ps1 [-Init]
#   -Init : force rebuild the file-hash baseline (run once after deploying a new build)
# Monitors by default: D:/CZH720/tools/uninstaller-portable
# Detection mapping:
#   R1  file deviates from baseline (exe/dll hash change / new / deleted)
#   R3  cache integrity check failed (app writes to detection.log)
#   R5  out-of-bounds delete blocked (app writes to detection.log)
#   R6  registry-key delete blocked (app writes to detection.log)

param(
    [string]$TargetDir = "D:/CZH720/tools/uninstaller-portable",
    [string]$Baseline  = "D:/CZH720/tools/.fim_baseline.json",
    [string]$EventLog  = "D:/CZH720/tools/detection_result.log",
    [string]$LineState = "D:/CZH720/tools/.fim_linenum.txt",
    [switch]$Init
)

$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
function Write-Result($level, $msg) {
    $line = "$timestamp [$level] $msg"
    try { Add-Content -Path $EventLog -Value $line -ErrorAction SilentlyContinue } catch {}
    if ($level -eq "ALERT") { Write-Warning $line } else { Write-Host $line }
}

# 1) File integrity baseline (R1)
$files = Get-ChildItem $TargetDir -Recurse -Include *.exe,*.dll -ErrorAction SilentlyContinue
$cur = @{}
foreach ($f in $files) {
    try { $cur[$f.FullName] = (Get-FileHash $f.FullName -Algorithm SHA256).Hash } catch {}
}

if ($Init -or -not (Test-Path $Baseline)) {
    $cur | ConvertTo-Json | Set-Content $Baseline
    Write-Result "INFO" ("Baseline established (" + $cur.Count + " files): " + $Baseline)
} else {
    $old = Get-Content $Baseline | ConvertFrom-Json
    $oldMap = @{}
    foreach ($p in $old.PSObject.Properties) { $oldMap[$p.Name] = $p.Value }
    $alerts = @()
    foreach ($k in $cur.Keys) {
        if (-not $oldMap.ContainsKey($k)) { $alerts += ("NEW FILE: " + $k) }
        elseif ($oldMap[$k] -ne $cur[$k]) { $alerts += ("HASH CHANGED: " + $k) }
    }
    foreach ($k in $oldMap.Keys) {
        if (-not $cur.ContainsKey($k)) { $alerts += ("DELETED FILE: " + $k) }
    }
    if ($alerts.Count -eq 0) {
        Write-Result "OK" ("File integrity verified (" + $cur.Count + " files)")
    } else {
        foreach ($a in $alerts) { Write-Result "ALERT" ("FIM-R1: " + $a) }
    }
}

# 2) Collect app structured security events (R3/R5/R6)
$detLog = Join-Path $TargetDir "detection.log"
$startLine = 0
if (Test-Path $LineState) { [int]::TryParse((Get-Content $LineState -Raw), [ref]$startLine) | Out-Null }

if (Test-Path $detLog) {
    $lines = Get-Content $detLog
    $count = $lines.Count
    if ($count -lt $startLine) { $startLine = 0 }
    if ($count -gt $startLine) {
        $toProcess = $lines | Select-Object -Skip $startLine
        foreach ($raw in $toProcess) {
            $line = $raw.Trim()
            if ($line.Length -gt 0) {
                try {
                    $ev = $line | ConvertFrom-Json
                    $r = $ev.rule
                    if ($r -eq "R3" -or $r -eq "R5" -or $r -eq "R6") {
                        Write-Result "ALERT" ("DET-" + $r + " [" + $ev.event + "]: " + $ev.detail)
                    }
                } catch {}
            }
        }
        Set-Content $LineState -Value $count
    }
}

Write-Result "INFO" "Scan complete"
