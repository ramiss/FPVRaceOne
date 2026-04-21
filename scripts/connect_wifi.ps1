$nodes = @(
    @{ Profile = "FPVRaceOne_90FEFF"; Adapter = "Wi-Fi" }
    @{ Profile = "FPVRaceOne_8FFEFF"; Adapter = "Wi-Fi 7" }
)

$TimeoutSeconds = 30
$PollInterval   = 2

function Wait-ForSSID {
    param([string]$SSID, [string]$Adapter)

    Write-Host "  Waiting for '$SSID' to appear on $Adapter..."
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

    while ((Get-Date) -lt $deadline) {
        $networks = netsh wlan show networks interface=$Adapter 2>$null
        if ($networks -match [regex]::Escape($SSID)) {
            Write-Host "  '$SSID' detected."
            return $true
        }
        Start-Sleep -Seconds $PollInterval
    }

    Write-Host "  TIMEOUT: '$SSID' not found on $Adapter after $TimeoutSeconds seconds."
    return $false
}

Write-Host "Connecting to FPV nodes..."
foreach ($n in $nodes) {
    if (-not (Wait-ForSSID -SSID $n.Profile -Adapter $n.Adapter)) {
        continue
    }
    try {
        $out = netsh wlan connect name=$($n.Profile) interface=$($n.Adapter) 2>&1
        if ($LASTEXITCODE -ne 0) { throw $out }
        Write-Host "  [OK] Connected to $($n.Profile)"
    } catch {
        Write-Host "  [ERROR] $_"
    }
}
Write-Host "`nDone."
