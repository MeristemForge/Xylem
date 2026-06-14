# winclient.ps1 -- Windows helper for run-net.bat.
#
# Runs one bench-client invocation pinned to a CPU-affinity mask (with a
# matching GOMAXPROCS), capturing its JSON to -OutFile, and -- for throughput
# runs -- samples per-core CPU on the server's cores while the client runs.
#
# Per-core CPU uses Win32_PerfRawData_PerfOS_Processor (language-neutral raw
# counters; PercentProcessorTime here is a PERF_100NSEC_TIMER_INV, i.e. idle
# time, so busy% = 100*(1 - dIdle/dTime)). With core-pinning the server owns
# cores 0..ServerNcpu-1 exclusively, so the system per-core figures for those
# cores are the server's. Prints the "cpu0:NN% cpu1:.." line to stdout.
param(
    [string]$Exe,
    [string]$OutFile,
    [string]$ArgString,         # client args, space-separated (has leading -flags)
    [string]$CliMaskHex = "",   # client affinity mask (hex, no 0x); "" = no pin
    [int]$Gomaxprocs = 0,       # 0 = leave default
    [int]$ServerNcpu = 0        # >0 = sample per-core CPU for cores 0..N-1
)

if ($Gomaxprocs -gt 0) { $env:GOMAXPROCS = "$Gomaxprocs" }
$ClientArgs = $ArgString.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)

function Sample-Cores($n) {
    $rows = Get-CimInstance Win32_PerfRawData_PerfOS_Processor -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+$' -and [int]$_.Name -lt $n }
    $m = @{}
    foreach ($r in $rows) { $m[[int]$r.Name] = @($r.PercentProcessorTime, $r.Timestamp_Sys100NS) }
    return $m
}

$before = $null
if ($ServerNcpu -gt 0) { $before = Sample-Cores $ServerNcpu }

$p = Start-Process -FilePath $Exe -ArgumentList $ClientArgs `
        -RedirectStandardOutput $OutFile -PassThru -WindowStyle Hidden
if ($CliMaskHex -ne "") {
    try { $p.ProcessorAffinity = [IntPtr]([Convert]::ToInt64($CliMaskHex, 16)) } catch {}
}
$p.WaitForExit()

if ($ServerNcpu -gt 0 -and $before) {
    $after = Sample-Cores $ServerNcpu
    $parts = @()
    for ($i = 0; $i -lt $ServerNcpu; $i++) {
        if ($before.ContainsKey($i) -and $after.ContainsKey($i)) {
            $dIdle = [double]($after[$i][0] - $before[$i][0])
            $dTime = [double]($after[$i][1] - $before[$i][1])
            $pct = 0
            if ($dTime -gt 0) { $pct = [math]::Round((1 - $dIdle / $dTime) * 100) }
            if ($pct -lt 0) { $pct = 0 }
            if ($pct -gt 100) { $pct = 100 }
            $parts += "cpu${i}:${pct}%"
        }
    }
    Write-Output ($parts -join " ")
}
