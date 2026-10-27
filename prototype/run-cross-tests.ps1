$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
New-Item -ItemType Directory -Force results | Out-Null
function Run-One($name,$arguments,$expected=0,$diagnostic=$false,$program='./wine-host.exe') {
    $p=New-Object System.Diagnostics.Process
    $p.StartInfo.FileName=(Resolve-Path $program).Path
    $p.StartInfo.WorkingDirectory=(Get-Location).Path
    $p.StartInfo.Arguments=$arguments
    $p.StartInfo.UseShellExecute=$false
    $p.StartInfo.RedirectStandardOutput=$true
    $p.StartInfo.RedirectStandardError=$true
    $p.StartInfo.CreateNoWindow=$true
    [void]$p.Start()
    $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
    $timedOut=-not $p.WaitForExit(10000)
    if($timedOut){$p.Kill();$p.WaitForExit()}
    $row=[pscustomobject]@{name=$name;exitCode=$p.ExitCode;expectedExit=$expected;diagnostic=$diagnostic;passed=(-not $timedOut -and $p.ExitCode -eq $expected);timedOut=$timedOut;stdout=$out.Result;stderr=$err.Result}
    $out.Result|Set-Content "results/$name.stdout.txt";$err.Result|Set-Content "results/$name.stderr.txt"
    $p.Dispose();return $row
}
$tests=@(
    @{name='custom-hello';args='guest/hello32.exe'},
    @{name='custom-files';args='guest/files32.exe'},
    @{name='custom-threads';args='guest/threads32.exe'},
    @{name='custom-stress';args='guest/stress32.exe'},
    @{name='custom-teb';args='guest/teb32.exe'},
    @{name='custom-return';args='guest/return32.exe';expected=73},
    @{name='components-hello';args='--wine-components guest/hello32.exe'},
    @{name='components-files';args='--wine-components guest/files32.exe'},
    @{name='components-file-semantics';args='--wine-components guest/file_semantics32.exe'},
    @{name='components-threads';args='--wine-components guest/threads32.exe'},
    @{name='components-teb';args='--wine-components guest/teb32.exe'},
    @{name='components-fs-recovery';args='--wine-components guest/fs_recovery32.exe'},
    @{name='components-tls';args='--wine-components guest/tls32.exe'},
    @{name='components-heap';args='--wine-components guest/heap32.exe'},
    @{name='components-concurrent';args='--wine-components guest/concurrent32.exe'},
    @{name='components-return';args='--wine-components guest/return32.exe';expected=73},
    @{name='components-crt';args='--wine-components guest/crt32.exe'},
    @{name='components-dll-bridge';args='--wine-components guest/bridge_app32.exe'},
    @{name='components-dynamic';args='--wine-components guest/dynamic32.exe'},
    @{name='components-disk-loader';args='--wine-components guest/disk_loader32.exe'}
)
$results=@();foreach($t in $tests){$expected=0;if($t.ContainsKey('expected')){$expected=$t.expected};$results+=Run-One $t.name $t.args $expected ([bool]$t.diagnostic)}
foreach($name in @('file_semantics32','heap32','threads32','tls32','teb32','crt32','dynamic32','disk_loader32','concurrent32')) {
    $results+=Run-One "windows-$name" '' 0 $false "guest/$name.exe"
}
$bad=@($results|Where-Object { $_.timedOut -or (-not $_.diagnostic -and -not $_.passed) })
$gaps=@($results|Where-Object { $_.diagnostic -and -not $_.passed })
$report=[pscustomobject]@{os=[Environment]::OSVersion.VersionString;tests=$results;unexpectedFailures=$bad.Count;remainingDiagnosticFailures=$gaps.Count}
$report|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 results/cross-report.json
$results|Select-Object name,exitCode,passed,diagnostic,timedOut|Format-Table -AutoSize
Write-Output "UNEXPECTED_FAILURES=$($bad.Count)"
Write-Output "REMAINING_DIAGNOSTIC_FAILURES=$($gaps.Count)"
if($bad.Count){exit 1}
