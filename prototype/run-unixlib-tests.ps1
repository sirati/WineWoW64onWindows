[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$EchoRoot,
    [Parameter(Mandatory=$true)][string]$NegativeRuntimeRoot,
    [int]$Repeats=5,
    [string]$ReportName='unixlib-report'
)
$ErrorActionPreference='Stop'
$runtime=(Resolve-Path "$PSScriptRoot/out").Path
$echo=(Resolve-Path $EchoRoot).Path
$negative=(Resolve-Path $NegativeRuntimeRoot).Path
$results=Join-Path $runtime "results/$ReportName"
New-Item -ItemType Directory -Force $results|Out-Null
$utf8=New-Object System.Text.UTF8Encoding($false)
$rows=New-Object 'System.Collections.Generic.List[object]'
$oldBridge=$env:WINE_NATIVE_BRIDGE
$oldPath=$env:WINEDLLPATH
$oldOverrides=$env:WINEDLLOVERRIDES
Remove-Item Env:WINE_NATIVE_BRIDGE -ErrorAction SilentlyContinue
$env:WINEDLLOVERRIDES='wine_unixlib_echo=b'
function Invoke-Control([string]$name,[string]$program,[string]$arguments,[string]$cwd,[string]$paths,[int]$expectedExit,[string]$marker,[bool]$success,[string]$buildDirectory='') {
    $env:WINEDLLPATH=$paths
    $p=New-Object System.Diagnostics.Process
    try {
        $p.StartInfo.FileName=$program;$p.StartInfo.Arguments=$arguments
        $p.StartInfo.WorkingDirectory=$cwd;$p.StartInfo.UseShellExecute=$false
        $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
        $p.StartInfo.EnvironmentVariables.Remove('WINEBUILDDIR')
        if($buildDirectory){$p.StartInfo.EnvironmentVariables['WINEBUILDDIR']=$buildDirectory}
        [void]$p.Start();$pidValue=$p.Id
        $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
        $timeout=-not $p.WaitForExit(30000)
        if($timeout){$p.Kill();$p.WaitForExit()}
        $stdout=[string]$out.GetAwaiter().GetResult();$stderr=[string]$err.GetAwaiter().GetResult()
        $exit=[int]$p.ExitCode
        $markerMatched=$stdout -match $marker
        $loaded=$stdout -match 'WINE_LOAD_APPLICATION status=00000000'
        $passed=-not $timeout -and $exit -eq $expectedExit -and $markerMatched -and $loaded
        if($success){$passed=$passed -and $stdout -match '(?m)^GUEST_EXIT code=0 .*HOST_RETURNED\r?$' -and $stdout -notmatch 'ECHO_FAILED'}
        $row=[pscustomobject]@{name=$name;program=$program;arguments=$arguments;workingDirectory=$cwd;pid=$pidValue;wineDllPath=$paths;wineBuildDirectory=$buildDirectory;wineDllOverrides=$env:WINEDLLOVERRIDES;expectedExit=$expectedExit;exitCode=$exit;timedOut=$timeout;marker=$marker;markerMatched=$markerMatched;wineLoadMarker=$loaded;passed=$passed;stdout=$stdout;stderr=$stderr}
        $rows.Add($row)
        [IO.File]::WriteAllText((Join-Path $results "$name.json"),(ConvertTo-Json -InputObject $row -Depth 5),$utf8)
        [IO.File]::WriteAllText((Join-Path $results "$name.stdout.txt"),$stdout,$utf8)
        [IO.File]::WriteAllText((Join-Path $results "$name.stderr.txt"),$stderr,$utf8)
        Write-Output "UNIXLIB_PROCESS name=$name pid=$pidValue exit=$exit timedOut=$timeout passed=$passed"
    } finally {$p.Dispose()}
}
try {
    $app=Join-Path $echo 'wine-unixlib-echo-app.exe'
    $split=Join-Path $results 'split'
    New-Item -ItemType Directory -Force "$split/pe/i386-windows","$split/native/x86_64-native"|Out-Null
    Copy-Item -LiteralPath "$echo/i386-windows/wine_unixlib_echo.dll" -Destination "$split/pe/i386-windows/"
    Copy-Item -LiteralPath "$echo/x86_64-native/wine_unixlib_echo.dll" -Destination "$split/native/x86_64-native/"
    $build=Join-Path $results 'build'
    $buildModule=Join-Path $build 'dlls/wine_unixlib_echo'
    $buildApp=Join-Path $results 'build-native-app'
    New-Item -ItemType Directory -Force "$buildModule/i386-windows","$buildModule/x86_64-native",$buildApp|Out-Null
    Copy-Item -LiteralPath "$echo/i386-windows/wine_unixlib_echo.dll" -Destination "$buildModule/i386-windows/"
    Copy-Item -LiteralPath "$echo/x86_64-native/wine_unixlib_echo.dll" -Destination "$buildModule/x86_64-native/"
    Copy-Item -LiteralPath $app -Destination $buildApp
    Copy-Item -LiteralPath "$echo/i386-windows/wine_unixlib_echo.dll" -Destination $buildApp
    $buildArguments='--wine-components "'+(Join-Path $buildApp 'wine-unixlib-echo-app.exe')+'"'
    Invoke-Control 'negative-hello' "$negative/wine-host.exe" '--wine-components guest/hello32.exe' $negative '' 0 '(?m)^hello32: stdout, process ID, 64-bit DLL call, memory and sleep passed\r?$' $true
    # The control runtime uses the candidate's platform fixes, with generic
    # unixlib lookup disabled at build time. Its manifest identifies that
    # host explicitly; it is not an unchanged historical runtime.
    $negativeApp=Join-Path $negative 'unixlib-negative'
    New-Item -ItemType Directory -Force $negativeApp|Out-Null
    Copy-Item -LiteralPath $app -Destination $negativeApp
    Copy-Item -LiteralPath "$echo/i386-windows/wine_unixlib_echo.dll" -Destination $negativeApp
    $negativeArguments='--wine-components "'+(Join-Path $negativeApp 'wine-unixlib-echo-app.exe')+'"'
    Invoke-Control 'unixlib-disabled' "$negative/wine-host.exe" $negativeArguments $negative $echo 2 '(?m)^ECHO_FAILED load status=0xc0000135\r?$' $false
    $arguments='--wine-components "'+$app+'"'
    for($i=0;$i -lt $Repeats;$i++) {
        Invoke-Control "root-$i" "$runtime/wine-host.exe" $arguments $runtime $echo 0 '(?m)^ECHO_OK calls=20\r?$' $true
        Invoke-Control "sibling-$i" "$runtime/wine-host.exe" $arguments $runtime "$echo/i386-windows" 0 '(?m)^ECHO_OK calls=20\r?$' $true
        Invoke-Control "split-$i" "$runtime/wine-host.exe" $arguments $runtime "$split/pe;$split/native" 0 '(?m)^ECHO_OK calls=20\r?$' $true
        Invoke-Control "build-$i" "$runtime/wine-host.exe" $arguments $runtime '' 0 '(?m)^ECHO_OK calls=20\r?$' $true $build
        Invoke-Control "build-native-$i" "$runtime/wine-host.exe" $buildArguments $runtime '' 0 '(?m)^ECHO_OK calls=20\r?$' $true $build
    }
    Invoke-Control 'missing-native' "$runtime/wine-host.exe" $arguments $runtime "$split/pe" 2 '(?m)^ECHO_FAILED load status=0xc0000135\r?$' $false
    $runtimeNativeDirectory=Join-Path $runtime 'x86_64-native'
    $createdNativeDirectory=-not (Test-Path -LiteralPath $runtimeNativeDirectory)
    $runtimeNative=Join-Path $runtimeNativeDirectory 'wine_unixlib_echo.dll'
    if(Test-Path -LiteralPath $runtimeNative){throw 'Preserve the existing runtime unixlib'}
    New-Item -ItemType Directory -Force $runtimeNativeDirectory|Out-Null
    try {
        Copy-Item -LiteralPath "$echo/x86_64-native/wine_unixlib_echo.dll" -Destination $runtimeNative
        if((Get-FileHash -Algorithm SHA256 -LiteralPath $runtimeNative).Hash -ne
           (Get-FileHash -Algorithm SHA256 -LiteralPath "$echo/x86_64-native/wine_unixlib_echo.dll").Hash){throw 'Runtime unixlib copy changed'}
        for($i=0;$i -lt $Repeats;$i++) {
            Invoke-Control "runtime-$i" "$runtime/wine-host.exe" $buildArguments $runtime '' 0 '(?m)^ECHO_OK calls=20\r?$' $true
        }
    } finally {
        Remove-Item -LiteralPath $runtimeNative -ErrorAction SilentlyContinue
        if($createdNativeDirectory){Remove-Item -LiteralPath $runtimeNativeDirectory -ErrorAction SilentlyContinue}
    }
    [IO.File]::WriteAllText((Join-Path $results 'processes.json'),(ConvertTo-Json -InputObject @($rows.ToArray()) -Depth 6),$utf8)
    $failures=@($rows|Where-Object {-not $_.passed}).Count
    Write-Output "UNIXLIB_FAILURES=$failures"
    if($failures){exit 1}
} finally {
    $env:WINE_NATIVE_BRIDGE=$oldBridge;$env:WINEDLLPATH=$oldPath;$env:WINEDLLOVERRIDES=$oldOverrides
}
