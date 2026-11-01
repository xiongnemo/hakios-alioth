#requires -Version 5.1
# Redmi K40 / POCO F3 (alioth) 上的 iOS 16：一键内存启动。不刷写、不擦除任何分区；重启手机即回到 Android。
[CmdletBinding()]
# -Back：相机 App 里「后置摄像头」用哪一颗：wide 主摄（默认）、ultra 超广角、macro 微距
param([string]$Serial = "", [ValidateSet("wide", "ultra", "macro")][string]$Back = "wide")
$Image = "alioth-boot.img"
$Payload = "run-payload.bin"
$SystemImage = "system.apfs"
$OutDir = Join-Path ([IO.Path]::GetTempPath()) ("alioth-" + [guid]::NewGuid().ToString("N"))

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BootDir = Join-Path $Root "boot"
if (-not $OutDir) { $OutDir = Join-Path ([IO.Path]::GetTempPath()) ("alioth-" + [guid]::NewGuid().ToString("N")) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Stamp = Get-Date -Format "HHmmss"
$Tag = "run$Stamp"

function Say([string]$Text) {
    Write-Host ""
    Write-Host "== $Text" -ForegroundColor Cyan
}

function Resolve-Input([string]$Name) {
    if ([IO.Path]::IsPathRooted($Name)) { return $Name }
    return (Join-Path $BootDir $Name)
}

function Find-Tool([string]$Name) {
    $local = Join-Path $Root ("bin\" + $Name + ".exe")
    if (Test-Path -LiteralPath $local) { return $local }
    throw "缺少启动工具：$local"
}

# 运行外部命令并限时；超时就杀掉，避免 fastboot 在没有设备时永远打印 "< waiting for any device >"。
function Invoke-Tool {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][string[]]$Arguments,
        [int]$TimeoutSec = 30
    )
    if ($Exe -eq $Fastboot -and $Arguments[0] -ne "devices") {
        $Arguments = @("-s", $script:FastbootSerial) + $Arguments
    }
    $argLine = ($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' '
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $argLine
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    if (-not $proc.Start()) { throw "无法启动：$Exe" }
    # 异步读走两个流，避免 WaitForExit(ms) 时管道写满导致死锁。
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        try { $proc.Kill() } catch { }
        throw "命令超时 ($TimeoutSec s): $(Split-Path -Leaf $Exe) $($Arguments -join ' ')"
    }
    $text = "$($outTask.Result)$($errTask.Result)"
    if ($proc.ExitCode -ne 0) {
        throw "命令失败 (exit $($proc.ExitCode)): $(Split-Path -Leaf $Exe) $($Arguments -join ' '):`n$($text.Trim())"
    }
    return $text
}

# apps\ 里的 IPA：iOS 每次启动后，这台电脑在局域网里找到手机，把它们推过去装上（手机和电脑要在同一个网络里）。
# 只由电脑往外连手机，不需要防火墙放行。手机的数据都在内存里，重启后应用会消失，所以每次启动都重新装一遍。
# 手机那头：系统镜像里的 ipainstall -d（端口 48620），只认这次启动时放进内核命令行的口令 apps_token。
$AppsPort = 48620

function New-AppsPush {
    $dir = Join-Path $Root "apps"
    # 从小到大装：地图、工具这些小应用先装上，几百 MB 的游戏最后装（数据盘在内存里，满了也不会挤掉小应用）
    $ipas = @(Get-ChildItem -LiteralPath $dir -Filter "*.ipa" -File -ErrorAction SilentlyContinue | Sort-Object Length)
    if ($ipas.Count -eq 0) { return $null }
    # 本机在默认路由那块网卡上的 IPv4 地址和网段（手机连的应该就是这个网络）
    $route = Get-NetRoute -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue |
        Sort-Object { $_.RouteMetric + $_.InterfaceMetric } | Select-Object -First 1
    $addr = $null
    if ($route) {
        $addr = Get-NetIPAddress -InterfaceIndex $route.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -notmatch '^(127\.|169\.254\.)' } | Select-Object -First 1
    }
    if (-not $addr) { Write-Host "apps\ 里有 IPA，但找不到这台电脑的局域网地址，这次不装。" -ForegroundColor Yellow; return $null }
    # 要找的地址：本机所在的 /24（网段更大时也只找这一段，免得太慢）
    $octets = $addr.IPAddress.Split('.')
    $prefix = "$($octets[0]).$($octets[1]).$($octets[2])."
    $hosts = @(1..254 | ForEach-Object { "$prefix$_" } | Where-Object { $_ -ne $addr.IPAddress })
    $bytes = New-Object byte[] 16
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $token = -join ($bytes | ForEach-Object { $_.ToString("x2") })
    Write-Host "apps\ 里的 $($ipas.Count) 个 IPA：$(($ipas | ForEach-Object Name) -join ', ')（iOS 启动后从 $($addr.IPAddress) 推到手机上）"
    return @{ Files = $ipas; Token = $token; Hosts = $hosts }
}

# 同时连每个地址的 48620 端口，回 "hakios-apps 1" 的就是手机
function Find-Phone($push) {
    $tries = foreach ($h in $push.Hosts) {
        $c = New-Object System.Net.Sockets.TcpClient
        @{ Host = $h; Client = $c; Task = $c.ConnectAsync($h, $AppsPort) }
    }
    try { [void][Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($tries | ForEach-Object { $_.Task }), 1500) } catch { }
    $found = $null
    foreach ($t in $tries) {
        if (-not $found -and $t.Task.Status -eq 'RanToCompletion' -and $t.Client.Connected) {
            try {
                $t.Client.ReceiveTimeout = 3000
                $reader = New-Object IO.StreamReader($t.Client.GetStream(), [Text.Encoding]::UTF8, $false, 4096, $true)
                if ($reader.ReadLine() -eq "hakios-apps 1") { $found = @{ Host = $t.Host; Client = $t.Client; Reader = $reader }; continue }
            } catch { }
        }
        $t.Client.Close()
    }
    return $found
}

function Send-Line($stream, [string]$text) {
    $b = [Text.Encoding]::UTF8.GetBytes($text + "`n")
    $stream.Write($b, 0, $b.Length)
}

function Send-Apps($push, [int]$TimeoutSec = 600) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $phone = $null
    Write-Host "等手机启动、连上 Wi-Fi…"
    while (-not $phone -and (Get-Date) -lt $deadline) {
        $phone = Find-Phone $push
        if (-not $phone) { Start-Sleep -Seconds 10 }
    }
    if (-not $phone) {
        Write-Host "等了 $([int]($TimeoutSec / 60)) 分钟没在网络里找到手机（手机连上 Wi-Fi 了吗？和电脑在同一个网络吗？），apps\ 这次没装。" -ForegroundColor Yellow
        Write-Host "也可以在手机上用「IPA 安装」App 装。"
        return
    }
    Write-Host "找到手机：$($phone.Host)"
    $installed = 0
    try {
        $phone.Client.ReceiveTimeout = 300000
        $stream = $phone.Client.GetStream()
        Send-Line $stream "TOKEN $($push.Token)"
        if ($phone.Reader.ReadLine() -ne "OK") { throw "手机不认这次的口令" }
        foreach ($f in $push.Files) {
            Write-Host ("  {0}（{1:N1} MB）…" -f $f.Name, ($f.Length / 1MB))
            Send-Line $stream "FILE $($f.Length) $($f.Name)"
            $in = [IO.File]::OpenRead($f.FullName)
            try { $in.CopyTo($stream) } finally { $in.Dispose() }
            $stream.Flush()
            while (($line = $phone.Reader.ReadLine()) -ne $null) {
                if ($line -like "DONE *") { break }
                Write-Host "    $($line.TrimStart('|', ' '))" -ForegroundColor DarkGray
            }
            if ($line -eq "DONE 1") { $installed++; Write-Host "    装好了" -ForegroundColor Green }
            else { Write-Host "    没装上（见上面手机说的原因）" -ForegroundColor Yellow }
        }
        Send-Line $stream "END"
    } catch {
        Write-Host "和手机的连接出错：$($_.Exception.Message)" -ForegroundColor Yellow
    } finally { $phone.Client.Close() }
    Write-Host "apps\ 里 $($push.Files.Count) 个 IPA 装上了 $installed 个（在主屏幕最后一页）。" -ForegroundColor Green
}

$Adb = Find-Tool "adb"
$Fastboot = Find-Tool "fastboot"

$ImagePath = Resolve-Input $Image
$PayloadPath = Resolve-Input $Payload
$SystemPath = Resolve-Input $SystemImage

Say "检查启动文件"
if (-not (Test-Path -LiteralPath $ImagePath)) { throw "缺少 loader 镜像：$ImagePath" }
if (-not (Test-Path -LiteralPath $PayloadPath)) { throw "缺少载荷：$PayloadPath" }
$UseSystem = $true
if ($UseSystem -and -not (Test-Path -LiteralPath $SystemPath)) {
    throw "缺少系统镜像，无法启动完整 iOS：$SystemPath"

}
if ($UseSystem) {
    Write-Host "镜像：$([IO.Path]::GetFileName($ImagePath)), $([IO.Path]::GetFileName($PayloadPath)), $([IO.Path]::GetFileName($SystemPath)) —— 无需下载任何固件"
} else {
    Write-Host "镜像：$([IO.Path]::GetFileName($ImagePath)), $([IO.Path]::GetFileName($PayloadPath)) —— 仅内核（不 stage 系统卷）"
}

Say "检查手机连接"
$devicesOut = Invoke-Tool -Exe $Adb -Arguments @("devices") -TimeoutSec 30
Write-Host $devicesOut.Trim()
if (-not $Serial) {
    $serials = @($devicesOut -split "`r?`n" | Where-Object { $_ -match '\s+device\s*$' } | ForEach-Object { ($_ -split '\s+')[0] })
    # 电脑上的模拟器（emulator-*、127.0.0.1:*、localhost:*）一概不碰，连型号都不去问
    $serials = @($serials | Where-Object { $_ -notmatch '^(emulator-\d+|127\.0\.0\.1:\d+|localhost:\d+)$' })
    # 只要 alioth（模拟器等其他 adb 设备不算）
    $aliothSerials = @($serials | Where-Object {
        try { (Invoke-Tool -Exe $Adb -Arguments @("-s", $_, "shell", "getprop", "ro.product.device") -TimeoutSec 10).Trim() -match '^alioth(in)?$' } catch { $false }
    })
    if ($aliothSerials.Count -ne 1) {
        throw "期望恰好一台 alioth（K40 / POCO F3），实际 $($aliothSerials.Count) 台（全部 adb 设备：$($serials -join ', ')）；可用 -Serial 指定。"
    }
    $Serial = $aliothSerials[0]
}
Write-Host "serial: $Serial"
$devTag = @("-s", $Serial)
$AndroidReady = $false
for ($i = 0; $i -lt 90; $i++) {
    try { $bootState = (Invoke-Tool -Exe $Adb -Arguments ($devTag + @("shell", "getprop", "sys.boot_completed")) -TimeoutSec 10).Trim() } catch { $bootState = "" }
    if ($bootState -eq "1") { $AndroidReady = $true; break }
    Start-Sleep -Seconds 1
}
if (-not $AndroidReady) { throw "Android 尚未完成启动，请进入 Android 桌面后重新运行。" }

$device = (Invoke-Tool -Exe $Adb -Arguments ($devTag + @("shell", "getprop", "ro.product.device"))).Trim()
if ($device -notmatch '^alioth(in)?$') { throw "此启动包只支持 alioth，当前设备：$device" }
if ((Invoke-Tool -Exe $Fastboot -Arguments @("devices")).Trim()) { throw "请断开其他 fastboot 设备后重新运行。" }
Say "进入临时启动模式"
try { Invoke-Tool -Exe $Adb -Arguments ($devTag + @("reboot", "bootloader")) -TimeoutSec 30 | Out-Null } catch { }
$fbDev = $false
for ($i = 0; $i -lt 30; $i++) {
    try { $text = Invoke-Tool -Exe $Fastboot -Arguments @("devices") -TimeoutSec 10 } catch { $text = "" }
    $connected = @($text -split "`r?`n" | Where-Object { $_ -match '\s+fastboot\s*$' } | ForEach-Object { ($_ -split '\s+')[0] })
    if ($connected.Count -gt 1) { throw "连接了多台 fastboot 设备。" }
    if ($connected.Count -eq 1) { $script:FastbootSerial = $connected[0]; $fbDev = $true; break }
    Start-Sleep -Seconds 1
}
if (-not $fbDev) { throw "$Serial 没有进入 bootloader（还停在 Android）；不继续等待。" }

Say "加载启动程序（仅内存启动）"
$null = Invoke-Tool -Exe $Fastboot -Arguments @("boot", $ImagePath) -TimeoutSec 60
# 这个 loader 的 USB 序列号就是手机自己的（旧版 loader 叫 "xnuloader"）；两个都认，只要它报 product: xnuloader。
$BootloaderSerial = $script:FastbootSerial
$loaderUp = $false
for ($i = 0; $i -lt 40 -and -not $loaderUp; $i++) {
    foreach ($candidate in @($BootloaderSerial, "xnuloader")) {
        $script:FastbootSerial = $candidate
        try { $text = Invoke-Tool -Exe $Fastboot -Arguments @("getvar", "product") -TimeoutSec 10 } catch { $text = "" }
        if ($text -match 'product:\s*xnuloader') { $loaderUp = $true; break }
    }
    if (-not $loaderUp) { Start-Sleep -Seconds 1 }
}
if (-not $loaderUp) { throw "loader 没有起来（现在还是 bootloader 吗？）" }
Write-Host "内存启动程序已就绪"

if ($UseSystem) {
    Say "传输系统，请保持 USB 连接"
    $null = Invoke-Tool -Exe $Fastboot -Arguments @("oem", "ramdisk") -TimeoutSec 30
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $null = Invoke-Tool -Exe $Fastboot -Arguments @("stage", $SystemPath) -TimeoutSec 1800
    $sw.Stop()
    Write-Host ("system image staged in {0:N1} s" -f $sw.Elapsed.TotalSeconds)
} else {
    Say "no system image: the payload's ramdisk is the root"
    $null = Invoke-Tool -Exe $Fastboot -Arguments @("oem", "ramdisk", "none") -TimeoutSec 30
}

Say "传输内核"
$StagePayloadPath = $PayloadPath
if ($true) {
    # XNUP entries: name[16], uint32 length/reserved, data padded to 16 bytes.
# Rebuild only the command line; preserve the original payload for rollback.
function Update-XnuPayloadTime {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$InputPath,
        [Parameter(Mandatory)][string]$OutputPath,
        [long]$Epoch = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds(),
        [string]$CamBack = "wide",
        [string]$AppsToken = ""
    )
    if ($Epoch -lt 1577836800 -or $Epoch -gt 4102444800) { throw "Clock is outside 2020-2100: $Epoch" }
    if ([IO.Path]::GetFullPath($InputPath) -eq [IO.Path]::GetFullPath($OutputPath)) {
        throw "The timed payload must have a separate output path"
    }
    $data = [IO.File]::ReadAllBytes($InputPath)
    if ($data.Length -lt 32 -or [Text.Encoding]::ASCII.GetString($data, 0, 4) -ne 'XNUP' -or
        [BitConverter]::ToUInt32($data, 4) -ne $data.Length - 8) { throw 'Invalid XNUP header' }
    $body = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($body)
    $offset = 8L
    $found = $false
    $ended = $false
    try {
        while ($offset + 24 -le $data.Length) {
            $name = [Text.Encoding]::ASCII.GetString($data, [int]$offset, 16).TrimEnd([char]0)
            $length = [long][BitConverter]::ToUInt32($data, [int]$offset + 16)
            $reserved = [BitConverter]::ToUInt32($data, [int]$offset + 20)
            if (-not $name) {
                if ($length -ne 0 -or $offset + 24 -ne $data.Length) { throw 'Invalid XNUP terminator' }
                $writer.Write((New-Object byte[] 24))
                $ended = $true
                break
            }
            $padded = $length + ((16 - $length % 16) % 16)
            if ($offset + 24 + $padded -gt $data.Length) { throw "Truncated XNUP entry: $name" }
            if ($name -eq 'cmdline') {
                if ($found -or $length -eq 0 -or $data[[int]($offset + 23 + $length)] -ne 0) {
                    throw 'Invalid or duplicate XNUP command line'
                }
                $found = $true
                $line = [Text.Encoding]::UTF8.GetString($data, [int]$offset + 24, [int]$length - 1)
                $line = ([regex]::Replace($line, '(?<!\S)(epoch|cam_back|apps_url|apps_token)=\S+', '')).Trim()
                $line = "$line epoch=$Epoch".Trim()
                if ($CamBack -ne "wide") { $line = "$line cam_back=$CamBack" }
                if ($AppsToken) { $line = "$line apps_token=$AppsToken" }
                $replacement = [Text.Encoding]::UTF8.GetBytes($line + [char]0)
                $writer.Write($data, [int]$offset, 16)
                $writer.Write([uint32]$replacement.Length)
                $writer.Write([uint32]$reserved)
                $writer.Write($replacement)
                $writer.Write((New-Object byte[] ((16 - $replacement.Length % 16) % 16)))
            } else {
                $writer.Write($data, [int]$offset, [int](24 + $padded))
            }
            $offset += 24 + $padded
        }
        if (-not $found -or -not $ended) { throw 'XNUP command line or terminator missing' }
        $output = New-Object IO.MemoryStream
        $header = New-Object IO.BinaryWriter($output)
        try {
            $header.Write([uint32]0x50554e58)
            $header.Write([uint32]$body.Length)
            $header.Write($body.ToArray())
            [IO.File]::WriteAllBytes($OutputPath, $output.ToArray())
        } finally { $header.Dispose(); $output.Dispose() }
    } finally { $writer.Dispose(); $body.Dispose() }
}

    $StagePayloadPath = Join-Path $OutDir "$Tag.timed-payload.bin"
    $BootEpoch = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $AppsPush = New-AppsPush
    $AppsToken = if ($AppsPush) { $AppsPush.Token } else { "" }
    Update-XnuPayloadTime -InputPath $PayloadPath -OutputPath $StagePayloadPath -Epoch $BootEpoch -CamBack $Back -AppsToken $AppsToken
    Write-Host "时间已与电脑同步"
    if ($Back -ne "wide") { Write-Host "相机 App 的后置摄像头：$(@{ ultra = '超广角（IMX355）'; macro = '微距（S5K5E9）' }[$Back])" }
}
$null = Invoke-Tool -Exe $Fastboot -Arguments @("stage", $StagePayloadPath) -TimeoutSec 300
if ($StagePayloadPath -ne $PayloadPath) { Remove-Item -LiteralPath $StagePayloadPath -Force }

Say "启动 iOS"
$null = Invoke-Tool -Exe $Fastboot -Arguments @("oem", "run") -TimeoutSec 30

Write-Host "启动命令已发送，请等待桌面出现。重启手机即可返回 Android。" -ForegroundColor Green
if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Force }
if ($AppsPush) {
    Say "安装 apps\ 里的应用（窗口先别关）"
    Send-Apps $AppsPush
}
