# Radar USB köprüsü (Windows PowerShell 5.1, ek kurulum gerekmez)
# Uçuş verisini internetten indirir, USB seri port üzerinden Plane Radar 3D cihazına gönderir.
# Protokol: PR:ABEGIN / PR:A lat,lon,track,gs,alt,vrate,callsign,type / PR:AEND <unix>

$ErrorActionPreference = 'Stop'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
try { $Host.UI.RawUI.WindowTitle = 'Radar USB köprüsü' } catch {}

$IntervalSec = 5
$RadiusNm = 15          # yaklaşık 28 km (15 km menzil için fazlasıyla yeterli)
$Sources = @(
  @{ Name = 'adsb.fi';        Url = { param($la, $lo, $nm) "https://opendata.adsb.fi/api/v3/lat/$la/lon/$lo/dist/$nm" } },
  @{ Name = 'airplanes.live'; Url = { param($la, $lo, $nm) "https://api.airplanes.live/v2/point/$la/$lo/$nm" } },
  @{ Name = 'adsb.lol';       Url = { param($la, $lo, $nm) "https://api.adsb.lol/v2/point/$la/$lo/$nm" } }
)
$script:SrcIdx = 0
$script:Rx = ''
$Inv = [Globalization.CultureInfo]::InvariantCulture

function Find-RadarPort {
  $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '\(COM\d+\)' -and $_.Name -match 'CP210|CH34|CH910|USB-SERIAL|UART|Silicon Labs' }
  if ($dev) {
    $name = @($dev)[0].Name
    if ($name -match '\((COM\d+)\)') {
      Write-Host "Cihaz bulundu: $name" -ForegroundColor Green
      return $Matches[1]
    }
  }
  $ports = [System.IO.Ports.SerialPort]::GetPortNames()
  if ($ports.Count -eq 0) { throw 'Hiç COM portu bulunamadı. USB kablosunu ve CP210x sürücüsünü kontrol edin.' }
  Write-Host ('Bulunan portlar: ' + ($ports -join ', '))
  return (Read-Host 'Cihazın portunu yazın (ör. COM3)').Trim().ToUpper()
}

function Read-DeviceLines($sp) {
  try { $script:Rx += $sp.ReadExisting() } catch {}
  $out = @()
  while (($i = $script:Rx.IndexOf("`n")) -ge 0) {
    $out += $script:Rx.Substring(0, $i).TrimEnd("`r")
    $script:Rx = $script:Rx.Substring($i + 1)
  }
  if ($script:Rx.Length -gt 8000) { $script:Rx = '' }
  return $out
}

function Get-Num($v) {
  if ($v -is [double] -or $v -is [int] -or $v -is [long] -or $v -is [decimal] -or $v -is [single]) { return [double]$v }
  return $null
}

function Get-Traffic($lat, $lon) {
  $la = $lat.ToString('F4', $Inv); $lo = $lon.ToString('F4', $Inv)
  $lastErr = 'kaynak yok'
  for ($k = 0; $k -lt $Sources.Count; $k++) {
    $idx = ($script:SrcIdx + $k) % $Sources.Count
    $s = $Sources[$idx]
    $url = & $s.Url $la $lo $RadiusNm
    try {
      $r = Invoke-RestMethod -Uri $url -TimeoutSec 10 -UserAgent 'plane-radar-3d-usb'
      $script:SrcIdx = $idx
      $list = $r.ac
      if ($null -eq $list) { $list = $r.aircraft }
      if ($null -eq $list) { $list = @() }
      return @{ List = @($list); Name = $s.Name }
    } catch {
      $lastErr = "$($s.Name): $($_.Exception.Message)"
    }
  }
  throw $lastErr
}

function Build-Lines($list, $lat0, $lon0) {
  $items = foreach ($a in $list) {
    $la = Get-Num $a.lat; $lo = Get-Num $a.lon
    if ($null -eq $la -or $null -eq $lo) { continue }
    if ($a.alt_baro -is [string]) { continue }   # "ground"
    $alt = Get-Num $a.alt_baro; if ($null -eq $alt) { $alt = Get-Num $a.alt_geom }; if ($null -eq $alt) { $alt = 0 }
    $vr = Get-Num $a.baro_rate; if ($null -eq $vr) { $vr = Get-Num $a.geom_rate }; if ($null -eq $vr) { $vr = 0 }
    $trk = Get-Num $a.track; if ($null -eq $trk) { $trk = Get-Num $a.true_heading }; if ($null -eq $trk) { $trk = 0 }
    $gs = Get-Num $a.gs; if ($null -eq $gs) { $gs = Get-Num $a.tas }; if ($null -eq $gs) { $gs = 0 }
    $cs = ([string]$a.flight).Trim(); if (-not $cs) { $cs = [string]$a.hex }
    $cs = $cs -replace '[^A-Za-z0-9-]', ''; if ($cs.Length -gt 8) { $cs = $cs.Substring(0, 8) }
    $ty = ([string]$a.t) -replace '[^A-Za-z0-9]', ''; if ($ty.Length -gt 4) { $ty = $ty.Substring(0, 4) }
    $dx = ($lo - $lon0) * [math]::Cos($lat0 * [math]::PI / 180); $dy = $la - $lat0
    [pscustomobject]@{
      D    = $dx * $dx + $dy * $dy
      Line = [string]::Format($Inv, 'PR:A {0:F5},{1:F5},{2:F0},{3:F0},{4:F0},{5:F0},{6},{7}', $la, $lo, $trk, $gs, $alt, $vr, $cs, $ty)
    }
  }
  return @($items | Sort-Object D | Select-Object -First 64 | ForEach-Object { $_.Line })
}

Write-Host ''
Write-Host '=== Radar USB köprüsü ===' -ForegroundColor Cyan
Write-Host 'Bu pencere açık kaldığı sürece uçuş verisi cihaza gönderilir. Durdurmak için pencereyi kapatın.'
Write-Host 'Kurulum sayfası, esptool-js ve Arduino IDE kapalı olmalı.'
Write-Host ''

$sp = $null
try {
  $portName = Find-RadarPort
  $sp = New-Object System.IO.Ports.SerialPort $portName, 115200, 'None', 8, 'One'
  $sp.NewLine = "`n"
  $sp.DtrEnable = $false
  $sp.RtsEnable = $false
  $sp.WriteTimeout = 3000
  try { $sp.Open() } catch {
    throw "$portName açılamadı. Portu kullanan başka bir sayfa ya da program varsa kapatın. ($($_.Exception.Message))"
  }
  Write-Host "$portName açıldı." -ForegroundColor Green
  Start-Sleep -Milliseconds 1500

  # Cihazda kayıtlı konumu sor
  $lat = 41.0082; $lon = 28.9784
  $sp.WriteLine('PR:HELLO')
  $until = (Get-Date).AddSeconds(4)
  $gotHello = $false
  while ((Get-Date) -lt $until -and -not $gotHello) {
    foreach ($line in (Read-DeviceLines $sp)) {
      if ($line.StartsWith('PR:HELLO ')) {
        try {
          $d = $line.Substring(9) | ConvertFrom-Json
          if ($null -ne $d.lat -and $null -ne $d.lon) { $lat = [double]$d.lat; $lon = [double]$d.lon }
          Write-Host ("Cihaz yazılımı {0}, konum {1:F4}, {2:F4}" -f $d.fw, $lat, $lon) -ForegroundColor Green
        } catch {}
        $gotHello = $true
      }
    }
    Start-Sleep -Milliseconds 100
  }
  if (-not $gotHello) {
    Write-Host 'Cihaz HELLO cevabı vermedi; varsayılan konum (İstanbul) kullanılıyor. Yazılım v0.2 yüklü mü?' -ForegroundColor Yellow
  }
  Write-Host ''

  while ($true) {
    $t = (Get-Date).ToString('HH:mm:ss')
    try {
      $res = Get-Traffic $lat $lon
      $lines = Build-Lines $res.List $lat $lon
      $sp.WriteLine('PR:ABEGIN')
      for ($i = 0; $i -lt $lines.Count; $i++) {
        $sp.WriteLine($lines[$i])
        if ($i % 6 -eq 5) { Start-Sleep -Milliseconds 40 }
      }
      $sp.WriteLine('PR:AEND ' + [DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
      Start-Sleep -Milliseconds 600
      $ack = '?'
      foreach ($line in (Read-DeviceLines $sp)) {
        if ($line.StartsWith('PR:AOK ')) { try { $ack = ($line.Substring(7) | ConvertFrom-Json).n } catch {} }
      }
      Write-Host ("[{0}] {1}: {2} uçak gönderildi, cihaz aldı: {3}" -f $t, $res.Name, $lines.Count, $ack)
    } catch {
      Write-Host ("[{0}] Veri alınamadı: {1}" -f $t, $_.Exception.Message) -ForegroundColor Yellow
    }
    Start-Sleep -Seconds $IntervalSec
  }
} catch {
  Write-Host ''
  Write-Host ('HATA: ' + $_.Exception.Message) -ForegroundColor Red
} finally {
  if ($sp -and $sp.IsOpen) { $sp.Close() }
}
