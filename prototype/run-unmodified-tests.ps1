$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
Remove-Item Env:WINE_PROBE_TRACE -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force results/unmodified,results/unmodified/input,results/unmodified/extracted | Out-Null
function Run-One($name,$arguments,$expected=0,$required=$true,$program='./wine-host.exe',$pattern='') {
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
    $outputMatched=($pattern -eq '' -or $out.Result -match $pattern)
    $sha256=[Security.Cryptography.SHA256]::Create()
    $stdoutHash=([BitConverter]::ToString($sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($out.Result)))).Replace('-','')
    $stderrHash=([BitConverter]::ToString($sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($err.Result)))).Replace('-','')
    $sha256.Dispose()
    $row=[pscustomobject]@{name=$name;pid=$p.Id;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;workingDirectory=$p.StartInfo.WorkingDirectory;exitCode=$p.ExitCode;expectedExit=$expected;required=$required;outputPattern=$pattern;outputMatched=$outputMatched;passed=(-not $timedOut -and $p.ExitCode -eq $expected -and $outputMatched);timedOut=$timedOut;stdoutUtf8SHA256=$stdoutHash;stderrUtf8SHA256=$stderrHash;stdout=$out.Result;stderr=$err.Result}
    $out.Result|Set-Content "results/unmodified/$name.stdout.txt";$err.Result|Set-Content "results/unmodified/$name.stderr.txt"
    $p.Dispose();return $row
}
$seven='../unmodified/7za920/7za.exe'
$prefix="--wine-components $seven"
'Wine WoW64 cross-built on Linux. Original 7za.exe runs on Windows.'|Set-Content -Encoding UTF8 results/unmodified/input/message.txt
[byte[]]$bytes=0..255
$payload=New-Object byte[] 65536
for($i=0;$i -lt $payload.Length;$i++){$payload[$i]=$bytes[$i%256]}
[IO.File]::WriteAllBytes((Join-Path (Get-Location) 'results/unmodified/input/payload.bin'),$payload)
Remove-Item results/unmodified/data.zip -ErrorAction SilentlyContinue
Remove-Item results/unmodified/extracted -Recurse -Force
New-Item -ItemType Directory -Force results/unmodified/extracted | Out-Null
$results=@()
$results+=Run-One 'windows-7za-help' '' 0 $true $seven
$results+=Run-One 'wine-7za-help' $prefix
$results+=Run-One 'wine-7za-create' "$prefix a -tzip -mx=1 -mmt=off results/unmodified/data.zip ./results/unmodified/input/*"
$results+=Run-One 'wine-7za-list' "$prefix l results/unmodified/data.zip"
$results+=Run-One 'wine-7za-test' "$prefix t results/unmodified/data.zip"
$results+=Run-One 'wine-7za-extract' "$prefix x -y -oresults/unmodified/extracted results/unmodified/data.zip"
$hashes=@();foreach($name in @('message.txt','payload.bin')) {
    $source=(Get-FileHash "results/unmodified/input/$name" -Algorithm SHA256).Hash
    $target=if(Test-Path "results/unmodified/extracted/$name"){(Get-FileHash "results/unmodified/extracted/$name" -Algorithm SHA256).Hash}else{''}
    $hashes+=[pscustomobject]@{file=$name;sourceSHA256=$source;extractedSHA256=$target;passed=($source -eq $target)}
}
$results+=Run-One 'wine-lua-eval' '--wine-components ../unmodified/lua542/lua54.exe -e "print(6*7)"' 0 $true './wine-host.exe' '(?m)^42\r?$'
$results+=Run-One 'wine-lua-unwind' '--wine-components ../unmodified/lua542/lua54.exe -e "local ok,err=pcall(function() error(123) end); assert(not ok); print(42)"' 0 $true './wine-host.exe' '(?m)^42\r?$'
$results+=Run-One 'wine-7zr-help' '--wine-components ../unmodified/7zr.exe' 0 $true './wine-host.exe' '7-Zip \(r\) 26.04'
$results+=Run-One 'wine-busybox-help' '--wine-components ../unmodified/busybox.exe --help' 0 $false
$results+=Run-One 'wine-sqlite-version' '--wine-components ../unmodified/sqlite343/sqlite-tools-win32-x86-3430200/sqlite3.exe -version' 0 $true './wine-host.exe' '(?m)^3\.43\.2 '
$results+=Run-One 'wine-sqlite-query' '--wine-components ../unmodified/sqlite343/sqlite-tools-win32-x86-3430200/sqlite3.exe :memory: "create table t(x); insert into t values(6),(7); select sum(x)+29 from t;"' 0 $true './wine-host.exe' '(?m)^42\r?$'
Remove-Item results/unmodified/data.db -ErrorAction SilentlyContinue
$results+=Run-One 'wine-sqlite-database' '--wine-components ../unmodified/sqlite343/sqlite-tools-win32-x86-3430200/sqlite3.exe results/unmodified/data.db "create table t(x); insert into t values(42); select x from t;"' 0 $true './wine-host.exe' '(?m)^42\r?$'
$results+=Run-One 'wine-sqlite-reopen' '--wine-components ../unmodified/sqlite343/sqlite-tools-win32-x86-3430200/sqlite3.exe results/unmodified/data.db "pragma integrity_check; select x from t;"' 0 $true './wine-host.exe' '(?m)^ok\r?\n42\r?$'
$results+=Run-One 'windows-sqlite-reopen' 'results/unmodified/data.db "pragma integrity_check; select x from t;"' 0 $true '../unmodified/sqlite343/sqlite-tools-win32-x86-3430200/sqlite3.exe' '(?m)^ok\r?\n42\r?$'
$results+=Run-One 'wine-7za-overwrite' "$prefix x -y -oresults/unmodified/extracted results/unmodified/data.zip" 0 $true
$failures=@($results|Where-Object {$_.required -and -not $_.passed}).Count+@($hashes|Where-Object {-not $_.passed}).Count
$report=[pscustomobject]@{os=[Environment]::OSVersion.VersionString;tests=$results;roundTripHashes=$hashes;requiredFailures=$failures}
$report|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 results/unmodified/report.json
$results|Select-Object name,exitCode,passed,required,timedOut|Format-Table -AutoSize
$hashes|Format-Table -AutoSize
Write-Output "REQUIRED_FAILURES=$failures"
if($failures){exit 1}
