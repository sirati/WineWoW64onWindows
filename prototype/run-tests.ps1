$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
New-Item -ItemType Directory -Force results | Out-Null
$results=@()
function Run-One($name,$exe,$arguments) {
    $stdout="results/$name.stdout.txt"; $stderr="results/$name.stderr.txt"
    $p=New-Object System.Diagnostics.Process
    $p.StartInfo.FileName=(Resolve-Path $exe).Path
    $p.StartInfo.WorkingDirectory=(Get-Location).Path
    if($arguments) { $p.StartInfo.Arguments=$arguments }
    $p.StartInfo.UseShellExecute=$false
    $p.StartInfo.RedirectStandardOutput=$true
    $p.StartInfo.RedirectStandardError=$true
    $p.StartInfo.CreateNoWindow=$true
    [void]$p.Start()
    $outTask=$p.StandardOutput.ReadToEndAsync()
    $errTask=$p.StandardError.ReadToEndAsync()
    $timedOut=-not $p.WaitForExit(10000)
    if($timedOut) { $p.Kill();$p.WaitForExit() }
    $code=$p.ExitCode
    $outTask.Result|Set-Content $stdout
    $errTask.Result|Set-Content $stderr
    $row=[pscustomobject]@{name=$name;exitCode=$code;timedOut=$timedOut;stdout=$outTask.Result;stderr=$errTask.Result}
    $p.Dispose()
    Write-Output $row
}
foreach($test in @('hello32','stress32','teb32','tls32','threads32','files32','dynamic32','crt32')) {
    $results+=Run-One "port-$test" './wine-host.exe' "guest/$test.exe"
    if($test -ne 'stress32') { $results+=Run-One "windows-$test" "./guest/$test.exe" $null }
}
$failed=0
$repetitions=@()
for($i=0;$i -lt 30;$i++) {
    $r=Run-One "repeat-$i" './wine-host.exe' 'guest/hello32.exe'
    $repetitions+=$r
    if($r.exitCode -ne 0 -or $r.timedOut) { $failed++ }
}
$unexpected=@($results|Where-Object {
    $_.timedOut -or (($_.name -like 'windows-*' -or $_.name -in @('port-hello32','port-stress32')) -and $_.exitCode -ne 0)
})
$report=[pscustomobject]@{
    os=[Environment]::OSVersion.VersionString
    testedAtUTC=[DateTime]::UtcNow.ToString('o')
    compiler=[IO.File]::ReadAllLines("$PWD/compiler-version.txt")
    repeatRuns=30;repeatFailures=$failed
    unexpectedFailures=$unexpected.Count
    tests=$results;repetitions=$repetitions
}
$report|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 results/report.json
$report.tests|Select-Object name,exitCode,timedOut|Format-Table -AutoSize
Write-Output "REPEAT_RUNS=30 FAILURES=$failed"
if($failed -or $unexpected.Count) { exit 1 }
