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
$marker=switch -Regex ($guest) {
    '^cpp_unwind32' {'cpp_unwind32: main catch/rethrow and worker typed catches/destructor order passed'}
    '^cpp_nested_return32' {'cpp_unwind32: typed catches, rethrow and destructor order on main and workers passed'}
    '^seh_fault32$' {'seh_fault32: access violation, divide by zero and finally unwinding passed'}
    '^lifecycle32$' {'lifecycle32: independent static TLS, DLL/TLS notifications, FLS and ExitThread cleanup passed'}
    default {throw "Unknown exception fixture: $guest"}
}
[void]$p.Start();$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
$timeout=-not $p.WaitForExit(10000)
if($timeout){$p.Kill();$p.WaitForExit()}
$markerMatches=$out.Result -match ('(?m)^'+[regex]::Escape($marker)+'\r?$')
$load=$native -or $out.Result -match 'WINE_LOAD_APPLICATION status=00000000'
$returned=$native -or $out.Result -match '(?m)^GUEST_EXIT code=0 .*HOST_RETURNED\r?$'
$row=[pscustomobject]@{name=$name;pid=$p.Id;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;workingDirectory=$p.StartInfo.WorkingDirectory;exitCode=$p.ExitCode;timedOut=$timeout;required=$required;marker=$marker;markerMatches=$markerMatches;wineLoadMarker=$load;hostReturnMarker=$returned;passed=(-not $timeout -and $p.ExitCode -eq 0 -and $markerMatches -and $load -and $returned);stdout=$out.Result;stderr=$err.Result}
$p.Dispose();return $row
}
$rows=@()
foreach($guest in @('cpp_unwind32','seh_fault32','lifecycle32')) {$rows+=Run-One "wine-$guest" $guest}
foreach($guest in @('cpp_unwind32-native','cpp_nested_return32-native','seh_fault32','lifecycle32')) {$rows+=Run-One "windows-$guest" $guest $true}
$rows+=Run-One 'wine-cpp-nested-return-diagnostic' 'cpp_nested_return32'
$report=[pscustomobject]@{tests=$rows;requiredFailures=@($rows|Where-Object {$_.required -and -not $_.passed}).Count;diagnosticFailures=@($rows|Where-Object {-not $_.required -and -not $_.passed}).Count}
$report|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 results/exception-report.json
$rows|Select-Object name,exitCode,timedOut,passed,required|Format-Table -AutoSize
if($report.requiredFailures){exit 1}
