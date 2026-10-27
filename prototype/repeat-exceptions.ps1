$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
$rows=@()
foreach($guest in @('cpp_unwind32','seh_fault32','lifecycle32')) {
    for($i=0;$i -lt 30;$i++) {
        $p=New-Object System.Diagnostics.Process
        $p.StartInfo.FileName=(Resolve-Path './wine-host.exe').Path
        $p.StartInfo.WorkingDirectory=(Get-Location).Path
        $p.StartInfo.Arguments="--wine-components guest/$guest.exe"
        $p.StartInfo.UseShellExecute=$false;$p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
        [void]$p.Start();$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
        $timeout=-not $p.WaitForExit(10000)
        if($timeout){$p.Kill();$p.WaitForExit()}
        $rows+=[pscustomobject]@{guest=$guest;iteration=$i;exitCode=$p.ExitCode;timedOut=$timeout;passed=(-not $timeout -and $p.ExitCode -eq 0 -and $out.Result -match 'passed');stdout=$out.Result;stderr=$err.Result}
        $p.Dispose()
    }
}
$rows|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 results/exception-repeat.json
$rows|Group-Object guest|ForEach-Object {Write-Output "$($_.Name): $($_.Count) runs, $(@($_.Group|Where-Object {-not $_.passed}).Count) failures"}
if(@($rows|Where-Object {-not $_.passed}).Count){exit 1}
