#requires -Version 5.1
<#
Capture the same channel over UDP and SRT, without remuxing its MPEG-TS.
Requires ffmpeg and ffprobe in PATH (or supply their executable paths).
SRT URL latency is in MICROSECONDS in FFmpeg: 1500000 means 1500 ms.
Use -Ethernet to select a single connected wired IPv4 adapter automatically.
With several wired addresses, select an actual PC address using -InterfaceIP.

Example:
  .\compare_udp_srt.ps1 -UdpUrl 'udp://@239.10.10.1:1234' `
    -SrtUrl 'srt://192.0.2.10:34000?mode=caller&latency=1500000'

Offline analysis:
  .\compare_udp_srt.ps1 -UdpFile .\udp.ts -SrtFile .\srt.ts
#>
[CmdletBinding(DefaultParameterSetName = 'Capture')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Capture')]
    [ValidatePattern('^udp://')][string]$UdpUrl,
    [Parameter(Mandatory = $true, ParameterSetName = 'Capture')]
    [ValidatePattern('^srt://')][string]$SrtUrl,
    [Parameter(Mandatory = $true, ParameterSetName = 'Files')][string]$UdpFile,
    [Parameter(Mandatory = $true, ParameterSetName = 'Files')][string]$SrtFile,
    [string]$InterfaceIP = '',
    [Parameter(ParameterSetName = 'Capture')][switch]$Ethernet,
    [ValidateRange(5, 3600)][int]$DurationSeconds = 90,
    [string]$OutputDirectory = '',
    [string]$Ffmpeg = 'ffmpeg',
    [string]$Ffprobe = 'ffprobe',
    [switch]$SkipDecode
)
$ErrorActionPreference = 'Stop'
$encoding = New-Object System.Text.UTF8Encoding($false)
$selectedInterfaces = @()
if ($PSCmdlet.ParameterSetName -eq 'Capture') {
    $localInterfaces = @(
        foreach ($adapter in [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
            foreach ($unicast in $adapter.GetIPProperties().UnicastAddresses) {
                if ($unicast.Address.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork) {
                    [pscustomobject]@{
                        Name = $adapter.Name; IPAddress = $unicast.Address.ToString()
                        Type = $adapter.NetworkInterfaceType.ToString(); Status = $adapter.OperationalStatus.ToString()
                    }
                }
            }
        }
    )
    if ($Ethernet -and $InterfaceIP) { throw 'Use either -Ethernet or -InterfaceIP, not both.' }
    if ($Ethernet) {
        $candidates = @($localInterfaces | Where-Object {
            $_.Status -eq 'Up' -and $_.Type -in @('Ethernet', 'GigabitEthernet', 'FastEthernetT', 'FastEthernetFx') -and
            !$_.IPAddress.StartsWith('169.254.')
        })
        if ($candidates.Count -ne 1) {
            $details = ($candidates | ForEach-Object { $_.Name + ': ' + $_.IPAddress }) -join '; '
            throw "Expected one connected Ethernet IPv4 address; found $($candidates.Count). Connect the cable or select an address with -InterfaceIP. Candidates: $details"
        }
        $InterfaceIP = $candidates[0].IPAddress
    } elseif (!$InterfaceIP) {
        $urlAddress = [regex]::Match($UdpUrl, '(?:\?|&)localaddr=([^&]*)')
        if ($urlAddress.Success) { $InterfaceIP = [Uri]::UnescapeDataString($urlAddress.Groups[1].Value) }
    }
    if ($InterfaceIP) {
        $address = [Net.IPAddress]::Parse($InterfaceIP)
        if ($address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) { throw 'InterfaceIP must be an IPv4 address.' }
        $InterfaceIP = $address.ToString()
        $selectedInterfaces = @($localInterfaces | Where-Object { $_.IPAddress -eq $InterfaceIP -and $_.Status -eq 'Up' })
        if (!$selectedInterfaces.Count) {
            $details = ($localInterfaces | Where-Object { $_.Status -eq 'Up' } | ForEach-Object { $_.Name + ': ' + $_.IPAddress }) -join '; '
            throw "InterfaceIP $InterfaceIP is not assigned to a connected local adapter. Use the PC's adapter address, not a subnet or server address. Local IPv4 addresses: $details"
        }
        Write-Host ("UDP interface: " + $selectedInterfaces[0].Name + ' (' + $InterfaceIP + ')')
    }
}
$ffmpegPath = (Get-Command $Ffmpeg -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$ffprobePath = (Get-Command $Ffprobe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path (Get-Location) ('udp-srt-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 6))
}
if (Test-Path $OutputDirectory) { throw 'OutputDirectory already exists; choose a new directory to preserve earlier captures.' }
$outDir = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
if ($PSCmdlet.ParameterSetName -eq 'Capture') {
    [pscustomobject]@{ InterfaceIP = $InterfaceIP; EthernetRequested = [bool]$Ethernet; Interfaces = $selectedInterfaces; RequestedSeconds = $DurationSeconds } |
        ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $outDir 'capture-interface.json')
}

function Quote-Argument([string]$Value) {
    # ProcessStartInfo.Arguments uses native argument quoting, not shell syntax.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}
function Start-Tool([string]$Executable, [string[]]$Arguments, [string]$Stem) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.Arguments = ($Arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    [void]$process.Start()
    return [pscustomobject]@{
        Process = $process; Stdout = $process.StandardOutput.ReadToEndAsync()
        Stderr = $process.StandardError.ReadToEndAsync(); Stem = $Stem
    }
}
function Complete-Tool($Tool, [int]$TimeoutMs = 180000) {
    $timedOut = !$Tool.Process.WaitForExit($TimeoutMs)
    if ($timedOut) { $Tool.Process.Kill(); $Tool.Process.WaitForExit() }
    $stdout = $Tool.Stdout.GetAwaiter().GetResult()
    $stderr = $Tool.Stderr.GetAwaiter().GetResult()
    # Do not include URL credentials in the diagnostic logs.
    $stderr = [regex]::Replace($stderr, '(?i)(passphrase|password|token|streamid)=([^&\s'']+)', '$1=<redacted>')
    [IO.File]::WriteAllText((Join-Path $outDir ($Tool.Stem + '.stdout.txt')), $stdout, $encoding)
    [IO.File]::WriteAllText((Join-Path $outDir ($Tool.Stem + '.log')), $stderr, $encoding)
    $result = [pscustomobject]@{ ExitCode = $Tool.Process.ExitCode; TimedOut = $timedOut; Stdout = $stdout }
    $Tool.Process.Dispose()
    return $result
}
function Add-UrlOption([string]$Url, [string]$Name, [string]$Value) {
    if ($Url -match ('(?:\?|&)' + [regex]::Escape($Name) + '=')) { return $Url }
    $separator = '?'
    if ($Url.Contains('?')) { $separator = '&' }
    return $Url + $separator + $Name + '=' + $Value
}

if ($PSCmdlet.ParameterSetName -eq 'Capture') {
    $UdpUrl = $UdpUrl -replace '^udp://@', 'udp://'
    if ($InterfaceIP) {
        if ($UdpUrl -match '(?:\?|&)localaddr=') {
            $UdpUrl = [regex]::Replace($UdpUrl, '([?&])localaddr=[^&]*', '${1}localaddr=' + $InterfaceIP)
        } else { $UdpUrl = Add-UrlOption $UdpUrl 'localaddr' $InterfaceIP }
    }
    $UdpUrl = Add-UrlOption $UdpUrl 'buffer_size' '4194304'
    $UdpUrl = Add-UrlOption $UdpUrl 'fifo_size' '65536'
    # Fail visibly if this computer overruns its receive FIFO.
    $UdpUrl = Add-UrlOption $UdpUrl 'overrun_nonfatal' '0'
    if ($SrtUrl -match '(?:\?|&)latency=1500(?:&|$)') {
        Write-Warning 'FFmpeg latency=1500 is 1.5 ms. For 1500 ms use latency=1500000.'
    }
    $UdpFile = Join-Path $outDir 'udp.ts'
    $SrtFile = Join-Path $outDir 'srt.ts'
    $jobs = @()
    try {
        foreach ($source in @(@('udp', $UdpUrl, $UdpFile), @('srt', $SrtUrl, $SrtFile))) {
            $jobs += Start-Tool $ffmpegPath @('-hide_banner', '-loglevel', 'warning', '-nostats', '-y',
                '-f', 'data', '-raw_packet_size', '1316', '-i', $source[1],
                '-map', '0:0', '-c', 'copy', '-f', 'data', '-flush_packets', '1', $source[2]) ($source[0] + '-capture')
        }
        Write-Host "Recording both original transports for $DurationSeconds seconds..."
        $watch = [Diagnostics.Stopwatch]::StartNew()
        while ($watch.Elapsed.TotalSeconds -lt $DurationSeconds) {
            $ended = @($jobs | Where-Object { $_.Process.HasExited })
            if ($ended.Count) {
                Write-Warning ($ended[0].Stem + ' ended before the recording timer; stopping both captures. Inspect its log.')
                break
            }
            Write-Progress -Activity 'UDP + SRT capture' -Status ('{0:N0} / {1} s' -f $watch.Elapsed.TotalSeconds, $DurationSeconds) -PercentComplete (100 * $watch.Elapsed.TotalSeconds / $DurationSeconds)
            Start-Sleep -Milliseconds 200
        }
    } finally {
        Write-Progress -Activity 'UDP + SRT capture' -Completed
        foreach ($job in $jobs) {
            if (!$job.Process.HasExited) { try { $job.Process.StandardInput.WriteLine('q') } catch {} }
        }
        foreach ($job in $jobs) {
            $result = Complete-Tool $job 3000
            if ($result.ExitCode -ne 0 -or $result.TimedOut) {
                Write-Warning ($job.Stem + ' ended with an error or was stopped while waiting for input; inspect its log.')
            }
        }
    }
}

# Small native parser keeps per-PID CC/PCR/PTS inspection fast in Windows PS 5.1.
if (!('DvbCompareTsReader' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Collections.Generic;
public sealed class DvbComparePid {
    public int Pid;
    public string PidHex { get { return "0x" + Pid.ToString("X4"); } }
    public long Packets, Tei, Scrambled, HeaderErrors, CcErrors, ExactDuplicates, Discontinuities;
    public long PcrCount, PcrBackwards, PesStarts, PtsCount, PtsChanges;
    public double PcrSpanMs, PcrGapMaxMs, PtsSpanMs;
    internal int Cc = -1;
    internal byte[] LastPacket;
    internal long PcrLast = -1, PtsLast = -1, PtsPosition, PtsMin, PtsMax;
}
public sealed class DvbCompareTsResult {
    public long Bytes, Packets, SkippedBytes, TrailingBytes;
    public DvbComparePid[] Pids;
}
public static class DvbCompareTsReader {
    const long PcrWrap = (1L << 33) * 300, PtsWrap = 1L << 33;
    public static DvbCompareTsResult Read(string path) {
        var result = new DvbCompareTsResult();
        var pids = new SortedDictionary<int, DvbComparePid>();
        var b = new byte[188];
        using (var f = File.OpenRead(path)) {
            result.Bytes = f.Length;
            while (f.Position < f.Length) {
                long offset = f.Position;
                int have = 0, n;
                while (have < 188 && (n = f.Read(b, have, 188 - have)) > 0) have += n;
                if (have < 188) { result.TrailingBytes += have; break; }
                if (b[0] != 0x47) {
                    long candidate = offset + 1;
                    for (; candidate + 376 < f.Length; ++candidate) {
                        f.Position = candidate;
                        if (f.ReadByte() != 0x47) continue;
                        f.Position = candidate + 188;
                        if (f.ReadByte() != 0x47) continue;
                        f.Position = candidate + 376;
                        if (f.ReadByte() == 0x47) break;
                    }
                    if (candidate + 376 >= f.Length) { result.SkippedBytes += f.Length - offset; break; }
                    result.SkippedBytes += candidate - offset;
                    f.Position = candidate;
                    continue;
                }
                int pid = ((b[1] & 31) << 8) | b[2], afc = (b[3] >> 4) & 3;
                DvbComparePid s;
                if (!pids.TryGetValue(pid, out s)) { s = new DvbComparePid(); s.Pid = pid; pids.Add(pid, s); }
                ++result.Packets; ++s.Packets;
                bool tei = (b[1] & 128) != 0, scrambled = (b[3] & 192) != 0;
                if (tei) ++s.Tei;
                if (scrambled) ++s.Scrambled;
                if (afc == 0) { ++s.HeaderErrors; continue; }
                int payload = 4;
                bool reset = false;
                if ((afc & 2) != 0) {
                    int length = b[4];
                    if (length > 183 || (afc == 3 && length > 182)) { ++s.HeaderErrors; continue; }
                    payload = 5 + length;
                    reset = length > 0 && (b[5] & 128) != 0;
                    if (reset) { ++s.Discontinuities; s.Cc = -1; s.PcrLast = -1; }
                    if (length >= 7 && (b[5] & 16) != 0 && !tei) {
                        long ticks = (((long)b[6] << 25) | ((long)b[7] << 17) | ((long)b[8] << 9) | ((long)b[9] << 1) | (uint)(b[10] >> 7)) * 300 + ((b[10] & 1) << 8) + b[11];
                        ++s.PcrCount;
                        if (s.PcrLast >= 0) {
                            long delta = (ticks + PcrWrap - s.PcrLast) % PcrWrap;
                            if (delta > PcrWrap / 2) ++s.PcrBackwards;
                            else { s.PcrSpanMs += delta / 27000.0; s.PcrGapMaxMs = Math.Max(s.PcrGapMaxMs, delta / 27000.0); }
                        }
                        s.PcrLast = ticks;
                    }
                }
                if ((afc & 1) == 0 || pid == 8191) continue;
                int cc = b[3] & 15;
                bool duplicate = false;
                if (s.Cc >= 0 && cc != ((s.Cc + 1) & 15)) {
                    duplicate = cc == s.Cc && s.LastPacket != null;
                    if (duplicate) for (int i = 0; i < 188; ++i) if (s.LastPacket[i] != b[i]) { duplicate = false; break; }
                    if (duplicate) ++s.ExactDuplicates; else ++s.CcErrors;
                }
                s.Cc = cc;
                if (s.LastPacket == null) s.LastPacket = new byte[188];
                Buffer.BlockCopy(b, 0, s.LastPacket, 0, 188);
                if (duplicate || tei || scrambled || (b[1] & 64) == 0 || payload + 9 > 188) continue;
                if (b[payload] != 0 || b[payload + 1] != 0 || b[payload + 2] != 1) continue;
                int sid = b[payload + 3];
                if (sid != 0xbd && (sid < 0xc0 || sid > 0xef)) continue;
                ++s.PesStarts;
                if ((b[payload + 6] & 192) != 128 || (b[payload + 7] & 128) == 0 || b[payload + 8] < 5 || payload + 14 > 188) continue;
                int p = payload + 9;
                if ((b[p] & 1) == 0 || (b[p + 2] & 1) == 0 || (b[p + 4] & 1) == 0) continue;
                long pts = ((long)(b[p] & 14) << 29) | ((long)b[p + 1] << 22) | ((long)(b[p + 2] & 254) << 14) | ((long)b[p + 3] << 7) | (uint)(b[p + 4] >> 1);
                ++s.PtsCount;
                if (s.PtsLast >= 0) {
                    long delta = (pts + PtsWrap - s.PtsLast) % PtsWrap;
                    if (delta > PtsWrap / 2) delta -= PtsWrap;
                    if (delta != 0) ++s.PtsChanges;
                    s.PtsPosition += delta;
                    s.PtsMin = Math.Min(s.PtsMin, s.PtsPosition); s.PtsMax = Math.Max(s.PtsMax, s.PtsPosition);
                    s.PtsSpanMs = (s.PtsMax - s.PtsMin) / 90.0;
                }
                s.PtsLast = pts;
            }
        }
        result.Pids = new List<DvbComparePid>(pids.Values).ToArray();
        return result;
    }
}
'@
}

$summary = @()
$clocks = @()
foreach ($entry in @(@('udp', $UdpFile), @('srt', $SrtFile))) {
    $name = $entry[0]
    if (!(Test-Path -LiteralPath $entry[1])) { Write-Warning "$name capture file is missing; inspect capture log."; continue }
    $file = (Resolve-Path -LiteralPath $entry[1]).Path
    if ((Get-Item -LiteralPath $file).Length -eq 0) { Write-Warning "$name capture is empty; inspect capture log."; continue }
    Write-Host "Analysing $name..."
    $transport = [DvbCompareTsReader]::Read($file)
    $transport.Pids | Export-Csv -NoTypeInformation -Encoding UTF8 -Path (Join-Path $outDir ($name + '-pids.csv'))
    foreach ($clockPid in @($transport.Pids | Where-Object { $_.PcrCount -gt 0 })) {
        $clocks += [pscustomobject]@{
            Source = $name; PID = $clockPid.PidHex; PCR = $clockPid.PcrCount
            PcrSpanSeconds = [math]::Round($clockPid.PcrSpanMs / 1000, 3)
            PcrGapMaxMs = [math]::Round($clockPid.PcrGapMaxMs, 3); Backwards = $clockPid.PcrBackwards
        }
    }
    $probe = Start-Tool $ffprobePath @('-hide_banner', '-v', 'warning', '-show_entries',
        'stream=index,id,codec_type,codec_name,width,height,avg_frame_rate:format=duration,size,bit_rate', '-of', 'json', $file) ($name + '-probe')
    $probeResult = Complete-Tool $probe
    $streams = @()
    try { $streams = @(($probeResult.Stdout | ConvertFrom-Json).streams) } catch { Write-Warning "${name}: ffprobe could not identify streams." }
    $firstVideo = $streams | Where-Object { $_.codec_type -eq 'video' } | Select-Object -First 1
    $decodedFrames = $null
    $decodeExit = $null
    if (!$SkipDecode) {
        $decode = Start-Tool $ffmpegPath @('-hide_banner', '-nostdin', '-loglevel', 'warning', '-nostats', '-threads', '2',
            '-i', $file, '-map', '0:v:0?', '-map', '0:a:0?', '-progress', (Join-Path $outDir ($name + '-decode-progress.txt')),
            '-f', 'null', '-') ($name + '-decode')
        $decodeResult = Complete-Tool $decode
        $decodeExit = $decodeResult.ExitCode
        $progressFile = Join-Path $outDir ($name + '-decode-progress.txt')
        if (Test-Path -LiteralPath $progressFile) {
            $frameMatches = [regex]::Matches([IO.File]::ReadAllText($progressFile), '(?m)^frame=(\d+)\s*$')
            if ($frameMatches.Count) { $decodedFrames = [long]$frameMatches[$frameMatches.Count - 1].Groups[1].Value }
        }
        if ($decodeResult.TimedOut) { Write-Warning "$name decode exceeded 180 seconds; its report is incomplete." }
    }
    foreach ($stream in $streams) {
        if ($stream.codec_type -ne 'video' -and $stream.codec_type -ne 'audio') { continue }
        $transportPid = 0
        try { $transportPid = [Convert]::ToInt32([string]$stream.id, 16) } catch { continue }
        $stats = @($transport.Pids | Where-Object { $_.Pid -eq $transportPid })
        if (!$stats.Count) { continue }
        $s = $stats[0]
        $summary += [pscustomobject]@{
            Source = $name; Type = $stream.codec_type; Codec = $stream.codec_name; PID = $s.PidHex
            Packets = $s.Packets; CC = $s.CcErrors; TEI = $s.Tei; Scrambled = $s.Scrambled
            PES = $s.PesStarts; PTS = $s.PtsCount; PtsChanges = $s.PtsChanges
            PtsSpanSeconds = [math]::Round($s.PtsSpanMs / 1000, 3)
            DecodedFrames = $(if ($firstVideo -and $stream.index -eq $firstVideo.index) { $decodedFrames } else { $null })
            DecodeExit = $decodeExit
        }
    }
    $transport | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $outDir ($name + '-transport.json'))
}
if ($summary.Count) {
    $summary | Export-Csv -NoTypeInformation -Encoding UTF8 -Path (Join-Path $outDir 'summary.csv')
    $summary | Format-Table Source, Type, PID, CC, TEI, PtsChanges, PtsSpanSeconds -AutoSize | Out-Host
} else { Write-Warning 'No audio/video was identified in either capture. Check both capture logs and the multicast interface.' }
if ($clocks.Count) {
    $clocks | Export-Csv -NoTypeInformation -Encoding UTF8 -Path (Join-Path $outDir 'clocks.csv')
    $clocks | Format-Table -AutoSize | Out-Host
}
Write-Host "Reports and original TS files: $outDir"
$reportFiles = @(Get-ChildItem -LiteralPath $outDir -File | Where-Object { $_.Extension -ne '.ts' })
if ($reportFiles.Count) {
    Compress-Archive -LiteralPath $reportFiles.FullName -DestinationPath (Join-Path $outDir 'report.zip')
    Write-Host 'Send report.zip first. It contains reports and logs, without the large TS captures.'
}
Write-Host 'PCR/PTS spans describe media time, not network jitter. Different capture windows can have different packet counts.'
if (!@($summary | Where-Object { $_.Source -eq 'udp' }).Count -or !@($summary | Where-Object { $_.Source -eq 'srt' }).Count) {
    Write-Warning 'Comparison incomplete: audio/video was not identified on both UDP and SRT. Inspect the capture logs.'
    exit 2
}
