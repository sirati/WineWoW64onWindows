[CmdletBinding()]
param(
    [string[]]$Tests=@('ntdll-rtlbitmap','ntdll-large_int','ntdll-error','ntdll-rtlstr','ntdll-string','ntdll-time','ntdll-env','msvcrt-cpp'),
    [string]$UpstreamRoot='',
    [int]$TimeoutSeconds=60,
    [string]$ReportName='upstream-wine-tests'
)
$ErrorActionPreference='Stop'
if(-not $UpstreamRoot){$UpstreamRoot=Join-Path $PSScriptRoot 'out/upstream-tests'}
$runtime=(Resolve-Path "$PSScriptRoot/out").Path
$upstream=(Resolve-Path $UpstreamRoot).Path
$manifest=Get-Content -Raw "$upstream/manifest.json"|ConvertFrom-Json
New-Item -ItemType Directory -Force "$runtime/results/$ReportName"|Out-Null
$results="$runtime/results/$ReportName"
$rows=@()
foreach($name in $Tests) {
    $test=@($manifest.tests|Where-Object {$_.program -eq "$name.exe"})
    if($test.Count -ne 1){throw "Unknown upstream test: $name"}
    $test=$test[0]
    $executable=(Resolve-Path "$upstream/$($test.program)").Path
    if((Get-FileHash -Algorithm SHA256 $executable).Hash -ne $test.sha256){throw "Upstream test hash mismatch: $name"}
    foreach($native in @($true,$false)) {
        $mode=if($native){'native'}else{'wine'}
        $p=New-Object System.Diagnostics.Process
        $p.StartInfo.FileName=if($native){$executable}else{"$runtime/wine-host.exe"}
        $p.StartInfo.Arguments=if($native){$test.test}else{"--wine-components `"$executable`" $($test.test)"}
        $p.StartInfo.WorkingDirectory=$runtime
        $p.StartInfo.UseShellExecute=$false
        $p.StartInfo.RedirectStandardOutput=$true
        $p.StartInfo.RedirectStandardError=$true
        $p.StartInfo.EnvironmentVariables['WINETEST_PLATFORM']=if($native){'windows'}else{'wine'}
        $p.StartInfo.EnvironmentVariables['WINETEST_INTERACTIVE']='0'
        $p.StartInfo.EnvironmentVariables['WINETEST_COLOR']='0'
        $p.StartInfo.EnvironmentVariables['WINETEST_DEBUG']='1'
        $p.StartInfo.EnvironmentVariables.Remove('WINE_NATIVE_BRIDGE')
        $p.StartInfo.EnvironmentVariables.Remove('WINE_PROBE_TRACE')
        foreach($variable in @('WINE_TEST_OCCUPY_BOOTSTRAP_IMAGE','WINE_TEST_OCCUPY_APPLICATION_IMAGE','WINE_TEST_OCCUPY_APPLICATION_CANONICAL_IMAGE','WINE_TEST_OCCUPY_GATE_BASE')) {
            $p.StartInfo.EnvironmentVariables.Remove($variable)
        }
        [void]$p.Start()
        $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
        $timeout=-not $p.WaitForExit($TimeoutSeconds*1000)
        if($timeout){
            # Child tests can inherit these output pipes. Stop the entire
            # process tree so a child cannot keep ReadToEndAsync blocked.
            & "$env:SystemRoot/System32/taskkill.exe" /PID $p.Id /T /F | Out-Null
            if(-not $p.HasExited){$p.Kill()}
            $p.WaitForExit()
        }
        $stdout=$out.Result;$stderr=$err.Result
        # Subprocesses may print summaries for the same test name. Require
        # the launched process's PID rather than accepting a child's result.
        $testPid='{0:x4}' -f $p.Id
        $pattern='(?im)^'+$testPid+':'+ [regex]::Escape($test.test) + ':.*? (?<executed>\d+) tests executed \((?<todo>\d+) marked as todo, (?<flaky>\d+) as flaky, (?<failures>\d+) failures?\), (?<skipped>\d+) skipped\.'
        $summary=[regex]::Match($stdout,$pattern)
        $counts=$null
        if($summary.Success){
            $counts=[pscustomobject]@{executed=[int]$summary.Groups['executed'].Value;todo=[int]$summary.Groups['todo'].Value;flaky=[int]$summary.Groups['flaky'].Value;failures=[int]$summary.Groups['failures'].Value;skipped=[int]$summary.Groups['skipped'].Value}
        }
        $loaded=$native -or $stdout -match 'WINE_LOAD_APPLICATION status=00000000'
        $completed=-not $timeout -and $summary.Success -and $loaded
        $rows+=[pscustomobject]@{name=$name;native=$native;pid=$p.Id;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;platform=$p.StartInfo.EnvironmentVariables['WINETEST_PLATFORM'];exitCode=$p.ExitCode;timedOut=$timeout;completed=$completed;passed=($completed -and $p.ExitCode -eq 0 -and $counts.failures -eq 0);counts=$counts;unsupportedServices=@([regex]::Matches($stderr,'UNIMPLEMENTED NT service [0-9a-fA-F]+')|ForEach-Object {$_.Value}|Sort-Object -Unique);stdoutPath="$mode-$name.stdout.txt";stderrPath="$mode-$name.stderr.txt"}
        [IO.File]::WriteAllText("$results/$mode-$name.stdout.txt",$stdout)
        [IO.File]::WriteAllText("$results/$mode-$name.stderr.txt",$stderr)
        $p.Dispose()
    }
}
$report=[pscustomobject]@{os=[Environment]::OSVersion.VersionString;wineRevision=$manifest.wineRevision;manifestSHA256=(Get-FileHash -Algorithm SHA256 "$upstream/manifest.json").Hash;tests=$rows}
$report|ConvertTo-Json -Depth 8|Set-Content -Encoding UTF8 "$results/report.json"
$rows|Select-Object name,native,exitCode,timedOut,completed,passed,counts|Format-Table -AutoSize
if(@($rows|Where-Object {-not $_.passed}).Count){exit 1}
