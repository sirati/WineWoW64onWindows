$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
Remove-Item Env:WINE_PROBE_TRACE -ErrorAction SilentlyContinue
$cases=@(
    @{name='7za-help';args='../unmodified/7za920/7za.exe';pattern='7-Zip \(A\) 9.20'},
    @{name='7za-overwrite';args='../unmodified/7za920/7za.exe x -y -oresults/unmodified/extracted results/unmodified/data.zip';pattern='Everything is Ok'},
    @{name='lua-protected-error';args='../unmodified/lua542/lua54.exe -e "local ok=pcall(function() error(123) end); assert(not ok); print(42)"';pattern='(?m)^42\r?$'},
    @{name='sqlite-query';args='../unmodified/sqlite343/sqlite-tools-win32-x86-3430200/sqlite3.exe results/unmodified/data.db "pragma integrity_check; select x from t;"';pattern='(?m)^ok\r?\n42\r?$'},
    @{name='disk-loader';args='guest/disk_loader32.exe';pattern='32 DLL load/call/unload cycles and API-set lookup passed'}
)
$rows=@()
foreach($case in $cases){for($i=0;$i -lt 10;$i++){
    $p=New-Object System.Diagnostics.Process
    $p.StartInfo.FileName=(Resolve-Path './wine-host.exe').Path
    $p.StartInfo.WorkingDirectory=(Get-Location).Path
    $p.StartInfo.Arguments="--wine-components $($case.args)"
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    [void]$p.Start();$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
    $timeout=-not $p.WaitForExit(10000)
    if($timeout){$p.Kill();$p.WaitForExit()}
    $rows+=[pscustomobject]@{name=$case.name;iteration=$i;exitCode=$p.ExitCode;timedOut=$timeout;passed=(-not $timeout -and $p.ExitCode -eq 0 -and $out.Result -match $case.pattern);stdout=$out.Result;stderr=$err.Result}
    $p.Dispose()
}}
$rows|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 results/unmodified/repeat.json
$rows|Group-Object name|ForEach-Object {Write-Output "$($_.Name): $($_.Count) runs, $(@($_.Group|Where-Object {-not $_.passed}).Count) failures"}
if(@($rows|Where-Object {-not $_.passed}).Count){exit 1}
