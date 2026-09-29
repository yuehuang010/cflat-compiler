<#
.SYNOPSIS
  Compile-time parity harness: cflat (import cpp) vs clang++ -std=c++20 on the test_libs RUN-mode cases.

.DESCRIPTION
  For each selected case test_libs/<lib>/<case>.cb that has a sibling <case>.cpp, measures two baselines.

  Link baseline (-Mode Link):
    clang++ : clang++ -std=c++20 -O0 -fms-runtime-lib=<dll|static per lib> -fuse-ld=lld  (compile + link to an exe)
    cold    : cflat <case>.cb -o exe, with a fresh EMPTY CFLAT_CACHE_DIR that only got an untimed `cflat --init`
              (core bitcode present; no C++ header / request / companion cache entries)
    warm    : a second cflat compile with the SAME cache and a DIFFERENT -o name

  Front-end baseline (-Mode Syntax):
    clang++ : clang++ -std=c++20 -fsyntax-only (same -I flags; no codegen, no link, nothing to run)
    cold    : cflat <case>.cb --check with a fresh --init'd cache (its own cache dir, not shared with the Link cold)
    warm    : a second cflat --check on the same cache
    --check binds the headers and runs the C++ type requests, but runs in batch mode: it skips the
    demand-companion definition rounds, the optimizer, and object emission / linking.

  -Mode Both (default) runs both baselines. Every timed run is a fresh process; this script pins itself to the
  performance cores (default mask 0x55 = CPUs 0/2/4/6 on the Ryzen AI 9 365) and children inherit the affinity.
  Median of -N samples; every cold sample gets its own fresh cache. Every produced exe is run and must exit 0.
  cflat default optimization for -o is -O0 (main.cpp: "-O0 No optimization (default)"), so clang++ uses -O0.
  Both tools run inside the MSVC environment (vcvars64) so STL headers/libs resolve the same way.

.PARAMETER Filter   Substrings/wildcards on "<lib>/<case>"; default = all cases with a .cpp twin.
.PARAMETER N        Samples per measurement (median reported). Default 3.
.PARAMETER Trace    Add -ftime-trace to the first cold cflat -o run; the trace is kept in out\parity\trace\.
.PARAMETER Affinity Processor affinity mask. Default 0x55.
.PARAMETER Config   Release (default), Debug, or another x64\<Config> tree holding cflat.exe (e.g. RelWithDebInfo).
.PARAMETER Mode     Both (default), Link (clang++ compile+link vs cflat -o) or Syntax (clang++ -fsyntax-only vs cflat --check).

.EXAMPLE
  pwsh.exe -NoProfile -File .\test_libs_parity.ps1 -N 3
  pwsh.exe -NoProfile -File .\test_libs_parity.ps1 json/json_01 -N 1 -Trace -Mode Link
#>
param(
    [string[]]$Filter = @(),
    [int]$N = 3,
    [switch]$Trace,
    [int]$Affinity = 0x55,
    [string]$Config = 'Release',
    [ValidateSet('Both', 'Link', 'Syntax')][string]$Mode = 'Both'
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$deps = Join-Path $env:USERPROFILE '.cflat-compiler-deps'
$clangxx = Join-Path $deps 'llvm-23.1.0\bin\clang++.exe'
$cflat = Join-Path $repo "x64\$Config\cflat.exe"
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$outRoot = Join-Path $repo 'out\parity'
$scratch = Join-Path $repo 'scratch\parity'
$staticRoot = Join-Path $deps 'vcpkg_installed\x64-windows-static'
$fmtRoot = Join-Path $repo 'test_libs\vcpkg_installed\x64-windows'
# libtorch has its own manifest (test_libs\torch\vcpkg.json); CFLAT_TESTLIB_LIBTORCH wins, as in test_libs.bat.
$torchRoot = if ($env:CFLAT_TESTLIB_LIBTORCH) { $env:CFLAT_TESTLIB_LIBTORCH } else { Join-Path $repo 'test_libs\torch\vcpkg_installed\x64-windows' }

foreach ($p in @($clangxx, $cflat, $vcvars)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "missing: $p" }
}

# Import the MSVC environment into this process (children inherit it).
$envDump = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    $eq = $line.IndexOf('=')
    if ($eq -gt 0) { Set-Item -LiteralPath "env:$($line.Substring(0, $eq))" -Value $line.Substring($eq + 1) }
}
(Get-Process -Id $PID).ProcessorAffinity = [IntPtr]$Affinity

# Per-library settings, mirroring test_libs/<lib>/lib.cfg as resolved by test_libs.bat (root_win + include + lib_win).
# Crt = clang++ -fms-runtime-lib value: what the prebuilt lib needs (simdjson.lib is /MT, fmt.dll is /MD;
# header-only json follows cflat, which builds its C++ with the dynamic CRT).
$libs = @{
    json     = @{ Root = $staticRoot; Include = @('include'); Lib = @();                 RunPath = $null; Crt = 'dll' }
    simdjson = @{ Root = $staticRoot; Include = @('include'); Lib = @('lib\simdjson.lib'); RunPath = $null; Crt = 'static' }
    fmt      = @{ Root = $fmtRoot;    Include = @('include'); Lib = @('lib\fmt.lib');     RunPath = 'bin'; Crt = 'dll' }
    torch    = @{ Root = $torchRoot;  Include = @('include', 'include\torch\csrc\api\include')
                  Lib = @('lib\torch.lib', 'lib\torch_cpu.lib', 'lib\c10.lib'); RunPath = 'bin'; Crt = 'dll' }
}

New-Item -ItemType Directory -Force -Path $outRoot, $scratch, (Join-Path $outRoot 'trace') | Out-Null

function Get-Median([double[]]$v) {
    $s = $v | Sort-Object
    $s[[int][math]::Floor(($s.Count - 1) / 2)]
}

# Runs a process with output discarded; returns elapsed seconds and the exit code.
function Invoke-Timed([string]$exe, [string[]]$argv, [string]$log) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $exe @argv *> $log
    $code = $LASTEXITCODE
    $sw.Stop()
    [pscustomobject]@{ Sec = $sw.Elapsed.TotalSeconds; Code = $code }
}

function Assert-ExeRuns([string]$exe, [string]$runPath, [string]$what) {
    $savedPath = $env:PATH
    if ($runPath) { $env:PATH = "$runPath;$savedPath" }
    try {
        & $exe *> $null
        if ($LASTEXITCODE -ne 0) { throw "$what : exe exited $LASTEXITCODE ($exe)" }
    } finally { $env:PATH = $savedPath }
}

function New-FreshCache([string]$dir) {
    if (Test-Path -LiteralPath $dir) { Remove-Item -Recurse -Force -LiteralPath $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $env:CFLAT_CACHE_DIR = $dir
    & $cflat --init *> (Join-Path $scratch 'init.log')
    if ($LASTEXITCODE -ne 0) { throw "cflat --init failed for $dir" }
}

function Get-CacheStats([string]$dir) {
    $files = @(Get-ChildItem -Recurse -File -LiteralPath $dir)
    $cxx = @($files | Where-Object { $_.FullName -match '\\cheaders\\' -or $_.Name -match 'cxx|companion' })
    $cheaders = @($files | Where-Object { $_.FullName -match '\\cheaders\\' })
    [pscustomobject]@{
        Files = $files.Count; CxxEntries = $cxx.Count; HasCheaders = (Test-Path (Join-Path $dir 'cheaders'))
        Bytes = [long](($files | Measure-Object Length -Sum).Sum)
        CheaderFiles = $cheaders.Count; CheaderBytes = [long](($cheaders | Measure-Object Length -Sum).Sum)
    }
}

# Fresh cold cache for one sample; throws if it already holds C++ entries.
function New-ColdCache([string]$dir, [string]$caseId) {
    New-FreshCache $dir
    $stats = Get-CacheStats $dir
    if ($stats.CxxEntries -ne 0 -or $stats.HasCheaders) { throw "cold cache not empty of C++ entries: $caseId" }
    $stats
}

$cases = foreach ($lib in $libs.Keys | Sort-Object) {
    foreach ($cpp in Get-ChildItem (Join-Path $repo "test_libs\$lib\*.cpp") | Sort-Object Name) {
        $cb = [IO.Path]::ChangeExtension($cpp.FullName, '.cb')
        if (-not (Test-Path -LiteralPath $cb)) { continue }
        $id = "$lib/$($cpp.BaseName)"
        if ($Filter.Count -gt 0 -and -not ($Filter | Where-Object { $id -like "*$_*" })) { continue }
        [pscustomobject]@{ Lib = $lib; Name = $cpp.BaseName; Id = $id; Cpp = $cpp.FullName; Cb = $cb }
    }
}
if (-not $cases) { throw 'no cases selected' }

$rows = @()
foreach ($c in $cases) {
    $cfg = $libs[$c.Lib]
    $work = Join-Path $scratch $c.Name
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $runPath = if ($cfg.RunPath) { Join-Path $cfg.Root $cfg.RunPath } else { $null }
    $incs = $cfg.Include | ForEach-Object { Join-Path $cfg.Root $_ }
    $libFiles = $cfg.Lib | ForEach-Object { Join-Path $cfg.Root $_ }

    $cflatFlags = @()
    foreach ($inc in $incs) { $cflatFlags += @('--c-include', $inc) }
    foreach ($l in $libFiles) { $cflatFlags += @('--c-lib', $l) }
    $row = [ordered]@{ Case = $c.Id }

    if ($Mode -ne 'Syntax') {
        # clang++ (compile + link)
        $clangTimes = @()
        for ($i = 0; $i -lt $N; $i++) {
            $exe = Join-Path $work "clangxx_$i.exe"
            $argv = @('-std=c++20', '-O0', "-fms-runtime-lib=$($cfg.Crt)", '-fuse-ld=lld', '-w') +
                    ($incs | ForEach-Object { "-I$_" }) + @($c.Cpp, '-o', $exe) + $libFiles
            $r = Invoke-Timed $clangxx $argv (Join-Path $work 'clangxx.log')
            if ($r.Code -ne 0) { Get-Content (Join-Path $work 'clangxx.log') -Tail 15; throw "clang++ failed: $($c.Id)" }
            Assert-ExeRuns $exe $runPath "clang++ $($c.Id)"
            $clangTimes += $r.Sec
        }

        # cflat -o cold / warm
        $coldTimes = @(); $warmTimes = @(); $coldStart = $null; $coldEnd = $null
        for ($i = 0; $i -lt $N; $i++) {
            $cache = Join-Path $scratch "cache_$($c.Name)"
            $stats = New-ColdCache $cache $c.Id
            if ($i -eq 0) { $coldStart = $stats }

            $exeCold = Join-Path $work "cold_$i.exe"
            $argv = @($c.Cb) + $cflatFlags + @('-o', $exeCold, '--nologo')
            if ($Trace -and $i -eq 0) { $argv += '-ftime-trace' }
            $r = Invoke-Timed $cflat $argv (Join-Path $work 'cold.log')
            if ($r.Code -ne 0) { Get-Content (Join-Path $work 'cold.log') -Tail 15; throw "cflat cold failed: $($c.Id)" }
            Assert-ExeRuns $exeCold $runPath "cflat cold $($c.Id)"
            $coldTimes += $r.Sec
            if ($i -eq 0) { $coldEnd = Get-CacheStats $cache }
            # cflat writes the trace to the current directory as <input basename>.time-trace.json.
            $traceSrc = Join-Path (Get-Location).Path "$($c.Name).time-trace.json"
            if (Test-Path -LiteralPath $traceSrc) {
                Move-Item -Force -LiteralPath $traceSrc (Join-Path $outRoot "trace\$($c.Name).time-trace.json")
            }

            $exeWarm = Join-Path $work "warm_$i.exe"
            $argv = @($c.Cb) + $cflatFlags + @('-o', $exeWarm, '--nologo')
            $r = Invoke-Timed $cflat $argv (Join-Path $work 'warm.log')
            if ($r.Code -ne 0) { Get-Content (Join-Path $work 'warm.log') -Tail 15; throw "cflat warm failed: $($c.Id)" }
            Assert-ExeRuns $exeWarm $runPath "cflat warm $($c.Id)"
            $warmTimes += $r.Sec
        }
        $clangMed = Get-Median $clangTimes
        $coldMed = Get-Median $coldTimes
        $warmMed = Get-Median $warmTimes
        $row.Clang = [math]::Round($clangMed, 2)
        $row.Cold = [math]::Round($coldMed, 2)
        $row.ColdRatio = [math]::Round($coldMed / $clangMed, 2)
        $row.Warm = [math]::Round($warmMed, 2)
        $row.WarmRatio = [math]::Round($warmMed / $clangMed, 2)
        $row.ColdCacheFiles = $coldStart.Files
        $row.EndCacheFiles = $coldEnd.Files
        $row.EndCacheMB = [math]::Round($coldEnd.Bytes / 1MB, 1)
        $row.CheaderFiles = $coldEnd.CheaderFiles
        $row.CheaderMB = [math]::Round($coldEnd.CheaderBytes / 1MB, 1)
    }

    if ($Mode -ne 'Link') {
        # clang++ -fsyntax-only
        $synTimes = @()
        for ($i = 0; $i -lt $N; $i++) {
            $argv = @('-std=c++20', '-fsyntax-only', '-w') + ($incs | ForEach-Object { "-I$_" }) + @($c.Cpp)
            $r = Invoke-Timed $clangxx $argv (Join-Path $work 'clangxx_syn.log')
            if ($r.Code -ne 0) { Get-Content (Join-Path $work 'clangxx_syn.log') -Tail 15; throw "clang++ -fsyntax-only failed: $($c.Id)" }
            $synTimes += $r.Sec
        }

        # cflat --check cold / warm
        $chkColdTimes = @(); $chkWarmTimes = @(); $chkEnd = $null
        $chkArgv = @($c.Cb) + $cflatFlags + @('--check', '--nologo')
        for ($i = 0; $i -lt $N; $i++) {
            $cache = Join-Path $scratch "cache_syn_$($c.Name)"
            New-ColdCache $cache $c.Id | Out-Null

            $r = Invoke-Timed $cflat $chkArgv (Join-Path $work 'check_cold.log')
            if ($r.Code -ne 0) { Get-Content (Join-Path $work 'check_cold.log') -Tail 15; throw "cflat --check cold failed: $($c.Id)" }
            $chkColdTimes += $r.Sec
            if ($i -eq 0) { $chkEnd = Get-CacheStats $cache }

            $r = Invoke-Timed $cflat $chkArgv (Join-Path $work 'check_warm.log')
            if ($r.Code -ne 0) { Get-Content (Join-Path $work 'check_warm.log') -Tail 15; throw "cflat --check warm failed: $($c.Id)" }
            $chkWarmTimes += $r.Sec
        }
        $synMed = Get-Median $synTimes
        $chkColdMed = Get-Median $chkColdTimes
        $chkWarmMed = Get-Median $chkWarmTimes
        $row.SynClang = [math]::Round($synMed, 2)
        $row.ChkCold = [math]::Round($chkColdMed, 2)
        $row.ChkColdRatio = [math]::Round($chkColdMed / $synMed, 2)
        $row.ChkWarm = [math]::Round($chkWarmMed, 2)
        $row.ChkWarmRatio = [math]::Round($chkWarmMed / $synMed, 2)
        $row.ChkCacheMB = [math]::Round($chkEnd.Bytes / 1MB, 1)
    }

    $row = [pscustomobject]$row
    $rows += $row
    $line = '{0,-24}' -f $row.Case
    if ($Mode -ne 'Syntax') {
        $line += (' | link: clang++ {0,5:N2}s cold {1,5:N2}s ({2:N2}x) warm {3,5:N2}s ({4:N2}x)' -f `
            $row.Clang, $row.Cold, $row.ColdRatio, $row.Warm, $row.WarmRatio)
    }
    if ($Mode -ne 'Link') {
        $line += (' | syntax: clang++ {0,5:N2}s check cold {1,5:N2}s ({2:N2}x) warm {3,5:N2}s ({4:N2}x)' -f `
            $row.SynClang, $row.ChkCold, $row.ChkColdRatio, $row.ChkWarm, $row.ChkWarmRatio)
    }
    Write-Host $line
}

$csv = Join-Path $outRoot ("parity_{0:yyyyMMdd_HHmmss}.csv" -f (Get-Date))
$rows | Export-Csv -NoTypeInformation -Path $csv
Write-Host ''
$rows | Format-Table -AutoSize | Out-String -Width 250 | Write-Host
Write-Host "N=$N mode=$Mode config=$Config affinity=0x$('{0:X}' -f $Affinity) csv=$csv"
