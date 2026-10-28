$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
New-Item -ItemType Directory -Force results | Out-Null
function Run-One($name,$guest,$native=$false,$required=$true) {
$p=New-Object System.Diagnostics.Process
$p.StartInfo.FileName=if($native){(Resolve-Path "guest/$guest.exe").Path}else{(Resolve-Path './wine-host.exe').Path}
$p.StartInfo.WorkingDirectory=(Get-Location).Path
$p.StartInfo.Arguments=if($native){''}else{"--wine-components guest/$guest.exe"}
$p.StartInfo.UseShellExecute=$false;$p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
[void]$p.Start();$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
$timeout=-not $p.WaitForExit(10000)
if($timeout){$p.Kill();$p.WaitForExit()}
$row=[pscustomobject]@{name=$name;pid=$p.Id;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;workingDirectory=$p.StartInfo.WorkingDirectory;exitCode=$p.ExitCode;timedOut=$timeout;required=$required;passed=(-not $timeout -and $p.ExitCode -eq 0 -and $out.Result -match 'passed');stdout=$out.Result;stderr=$err.Result}
$p.Dispose();return $row
}
$rows=@()
foreach($guest in @('cpp_unwind32','seh_fault32','lifecycle32')) {$rows+=Run-One "wine-$guest" $guest}
foreach($guest in @('cpp_unwind32-native','cpp_nested_return32-native','seh_fault32','lifecycle32')) {$rows+=Run-One "windows-$guest" $guest $true}
$rows+=Run-One 'wine-cpp-nested-return-diagnostic' 'cpp_nested_return32' $false $false
$report=[pscustomobject]@{tests=$rows;requiredFailures=@($rows|Where-Object {$_.required -and -not $_.passed}).Count;diagnosticFailures=@($rows|Where-Object {-not $_.required -and -not $_.passed}).Count}
$report|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 results/exception-report.json
$rows|Select-Object name,exitCode,timedOut,passed,required|Format-Table -AutoSize
if($report.requiredFailures){exit 1}
