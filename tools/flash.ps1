<#
.SYNOPSIS
  Flash the beacon firmware to an ESP32-C6/C3 attached to this machine.

.DESCRIPTION
  Works from a local console or a remote desktop session: nothing is interactive.

  Two modes:
    1. -Url <url>       download a merged single image (offset 0x0) and flash it
    2. (default)        flash the four images found in -BinDir

.EXAMPLE
  # one-liner for a fresh board, no build needed
  .\tools\flash.ps1 -Url https://github.com/refreshcoder/ESP-BLE-AVA/raw/main/firmware/beacon_gw-c6-board2-full.bin

.EXAMPLE
  # flash a local `idf.py build` output
  .\tools\flash.ps1

.EXAMPLE
  # name the port explicitly, and a C3 target
  .\tools\flash.ps1 -Port COM7 -Chip esp32c3
#>
param(
    [string]$Port,
    [string]$BinDir = "build",
    [string]$Chip   = "esp32c6",
    [string]$Url
)

$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"   # much faster Invoke-WebRequest

function Find-EspPort {
    # Native USB-JTAG enumerates as CDC: VID_303A&PID_1001. Fall back to any COM port.
    $dev = Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue |
           Where-Object { $_.InstanceId -match 'VID_303A' } | Select-Object -First 1
    if (-not $dev) {
        $dev = Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue |
               Where-Object { $_.FriendlyName -match 'USB Serial|Espressif|USB 串行' } |
               Select-Object -First 1
    }
    if ($dev -and ($dev.FriendlyName -match '(COM\d+)')) { return $Matches[1] }
    return $null
}

# --- esptool present? ---
$py = $null
foreach ($candidate in @("py", "python", "python3")) {
    if (Get-Command $candidate -ErrorAction SilentlyContinue) { $py = $candidate; break }
}
if (-not $py) { throw "Python not found. Install it from python.org, then: py -m pip install -U esptool" }
& $py -m esptool version *> $null
if ($LASTEXITCODE -ne 0) { throw "esptool missing. Run: py -m pip install -U esptool" }

# --- port ---
if (-not $Port) {
    $Port = Find-EspPort
    if (-not $Port) {
        throw "No ESP device found. Plug the board in, check 'Get-PnpDevice -Class Ports', or pass -Port COMx."
    }
    Write-Host "auto-detected port: $Port"
} else {
    Write-Host "using port: $Port"
}

# --- payload ---
$tmp = Join-Path $env:TEMP "beacon_gw"
if ($Url) {
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $img = Join-Path $tmp "merged.bin"
    Write-Host "downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $img -UseBasicParsing
    $size = (Get-Item $img).Length
    Write-Host ("downloaded {0:N0} bytes" -f $size)
    $flashArgs = @("0x0", $img)
} else {
    $files = @{
        "0x0"     = Join-Path $BinDir "bootloader\bootloader.bin"
        "0x8000"  = Join-Path $BinDir "partition_table\partition-table.bin"
        "0xf000"  = Join-Path $BinDir "ota_data_initial.bin"
        "0x20000" = Join-Path $BinDir "beacon_gw.bin"
    }
    foreach ($k in $files.Keys) {
        if (-not (Test-Path $files[$k])) { throw "missing $($files[$k]) - run 'idf.py build' first, or pass -Url" }
    }
    $flashArgs = @()
    foreach ($k in @("0x0", "0x8000", "0xf000", "0x20000")) {
        $flashArgs += $k
        $flashArgs += $files[$k]
    }
}

# --- flash ---
# 460800 is fine on USB-JTAG; drop to 115200 if the link is unhappy.
$args = @("-m", "esptool", "--chip", $Chip, "-p", $Port, "-b", "460800",
          "--before", "default_reset", "--after", "hard_reset",
          "write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
          "--flash_size", "4MB") + $flashArgs

Write-Host ("{0} {1}" -f $py, ($args -join " "))
& $py @args
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Flash failed. Usual causes:" -ForegroundColor Yellow
    Write-Host "  - another program holds $Port (close Arduino IDE / PuTTY / serial monitor)"
    Write-Host "  - cable is charge-only or too long"
    Write-Host "  - hold BOOT while replugging, then retry"
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "done. The board now raises BeaconGW-XXXX (password beaconXXXX)." -ForegroundColor Green
Write-Host "UI: http://192.168.4.1/  (log in with the SoftAP password)"
