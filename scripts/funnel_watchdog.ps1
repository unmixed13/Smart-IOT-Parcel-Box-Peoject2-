<#
  Keeps the Tailscale Funnel pointed at the Smart Parcel Box server.

  It re-applies the Funnel when
    * Funnel is not "on" or no longer proxies http://127.0.0.1:<Port>   (e.g. Tailscale restarted)
    * the PC's own IPv4 address changed                                 (e.g. you switched hotspot / Wi-Fi)
    * optionally every -ReapplyMinutes minutes (0 = off)

  It cannot see whether the Internet can really reach the Funnel (the PC resolves its own
  *.ts.net name through Tailscale), so it only repairs what it can detect locally.

  Run:  powershell -NoProfile -ExecutionPolicy Bypass -File funnel_watchdog.ps1 [-Port 8888]
  Log:  funnel_watchdog.log next to this script
#>
param(
  [int]$Port = 8888,
  [int]$IntervalSeconds = 30,
  [int]$ReapplyMinutes = 0
)

$ErrorActionPreference = 'Continue'
$LogFile = Join-Path $PSScriptRoot 'funnel_watchdog.log'

function Write-Log([string]$Message) {
  $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message
  Add-Content -Path $LogFile -Value $line
  Write-Host $line
}

# Find tailscale.exe even when it is not on PATH (scheduled tasks often have a short PATH).
$Tailscale = (Get-Command tailscale -ErrorAction SilentlyContinue).Source
if (-not $Tailscale) { $Tailscale = Join-Path $env:ProgramFiles 'Tailscale\tailscale.exe' }
if (-not (Test-Path $Tailscale)) { Write-Log "tailscale.exe not found ($Tailscale)"; exit 1 }

function Get-NetworkSignature {
  # Active IPv4 addresses, ignoring loopback, link-local and Tailscale's own 100.x address.
  $addresses = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object {
      $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and
      $_.IPAddress -notlike '100.*' -and $_.InterfaceAlias -notlike '*Tailscale*'
    } | ForEach-Object { $_.IPAddress } | Sort-Object
  return ($addresses -join ',')
}

function Test-FunnelOk {
  $text = (& $Tailscale funnel status 2>&1 | Out-String)
  return ($text -match 'Funnel on') -and ($text -match ('127\.0\.0\.1:{0}' -f $Port))
}

function Reset-Funnel([string]$Reason) {
  Write-Log "Re-applying Funnel ($Reason)"
  & $Tailscale funnel reset 2>&1 | Out-Null
  Start-Sleep -Seconds 2
  $out = (& $Tailscale funnel --bg $Port 2>&1 | Out-String).Trim()
  Write-Log ("tailscale funnel --bg {0}: {1}" -f $Port, ($out -replace '\s+', ' '))
}

$lastSignature = Get-NetworkSignature
$lastApplied = Get-Date
Write-Log "Watchdog started (port $Port, every $IntervalSeconds s). Network: $lastSignature"
if (-not (Test-FunnelOk)) { Reset-Funnel 'Funnel was not on at start'; $lastApplied = Get-Date }

while ($true) {
  Start-Sleep -Seconds $IntervalSeconds
  $signature = Get-NetworkSignature
  if ($signature -ne $lastSignature) {
    Write-Log "Network changed: '$lastSignature' -> '$signature'"
    $lastSignature = $signature
    Start-Sleep -Seconds 10          # let the new connection settle (DHCP, Tailscale reconnect)
    Reset-Funnel 'network changed'
    $lastApplied = Get-Date
    continue
  }
  if (-not (Test-FunnelOk)) {
    Reset-Funnel 'Funnel not on / wrong target'
    $lastApplied = Get-Date
    continue
  }
  if ($ReapplyMinutes -gt 0 -and ((Get-Date) - $lastApplied).TotalMinutes -ge $ReapplyMinutes) {
    Reset-Funnel "periodic ($ReapplyMinutes min)"
    $lastApplied = Get-Date
  }
}
