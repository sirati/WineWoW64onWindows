[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GameRoot,
    [Parameter(Mandatory=$true)][string]$RendererRoot,
    [string]$RuntimeRoot='',
    [int]$StartSeconds=300,
    [int]$MenuSeconds=60,
    [int]$CloseSeconds=60,
    [string]$ReportName='civ3-renderer',
    [string]$WineDebug='err,fixme',
    [string]$GuestDebug='err+all,fixme+all'
)
# Civ3 with the civ3-renderer bridge (IFC23.dll) and its x64 unixlib over the
# game copy. The guest keeps Wine's null display driver; the renderer opens
# its own native window. Pass: the bridge log's adapter, art and viewer lines,
# a desktop screenshot, then the process ends after the viewer is closed.
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$runtime=if($RuntimeRoot){(Resolve-Path $RuntimeRoot).Path}else{(Resolve-Path (Join-Path $root 'out')).Path}
$renderer=(Resolve-Path $RendererRoot).Path
$conquests=Join-Path (Resolve-Path $GameRoot).Path 'Conquests'
$application=Join-Path $conquests 'Civ3Conquests.exe'
if(-not (Test-Path -LiteralPath $application)){throw "Missing Civ3Conquests.exe: $application"}
$results=Join-Path $runtime "results/$ReportName"
New-Item -ItemType Directory -Force $results|Out-Null
$utf8=New-Object System.Text.UTF8Encoding($false)
$steps=New-Object System.Collections.Generic.List[string]
# Progress goes to the console directly so functions can return values.
function Note([string]$text){$line="$([DateTime]::UtcNow.ToString('o')) $text";$steps.Add($line);[Console]::Out.WriteLine($line)}

# Install over the copy: keep the game's IFC23.dll as ifc23_orig.dll.
$original=Join-Path $conquests 'ifc23_orig.dll'
if(-not (Test-Path -LiteralPath $original)){Move-Item -LiteralPath (Join-Path $conquests 'IFC23.dll') -Destination $original}
Copy-Item -LiteralPath (Join-Path $renderer 'game/IFC23.dll') -Destination (Join-Path $conquests 'IFC23.dll') -Force
Copy-Item -LiteralPath (Join-Path $renderer 'game/civ3r_native.dll') -Destination (Join-Path $conquests 'civ3r_native.dll') -Force
$bridgeLog=Join-Path $conquests 'civ3-bridge.log'
Remove-Item -LiteralPath $bridgeLog -ErrorAction SilentlyContinue
$settings=Join-Path $results 'settings'
New-Item -ItemType Directory -Force $settings|Out-Null
$installed=foreach($name in 'IFC23.dll','ifc23_orig.dll','civ3r_native.dll'){
    $path=Join-Path $conquests $name
    [pscustomobject]@{path=$path;sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash;size=(Get-Item -LiteralPath $path).Length}
}

Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class ProbeWindow {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wparam, IntPtr lparam);
}
"@
function Save-Screenshot([string]$name){
    $bounds=[System.Windows.Forms.SystemInformation]::VirtualScreen
    $bitmap=New-Object System.Drawing.Bitmap $bounds.Width,$bounds.Height
    $graphics=[System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($bounds.Location,[System.Drawing.Point]::Empty,$bounds.Size)
        $path=Join-Path $results $name
        $bitmap.Save($path,[System.Drawing.Imaging.ImageFormat]::Png)
        Note "SCREENSHOT $path $($bounds.Width)x$($bounds.Height)"
        return $path
    } finally {$graphics.Dispose();$bitmap.Dispose()}
}
function Read-BridgeLog{ if(Test-Path -LiteralPath $bridgeLog){[IO.File]::ReadAllText($bridgeLog)}else{''} }
$markers=[ordered]@{
    adapter='using adapter Microsoft Basic Render Driver \(Dx12\)'
    art='civ3r-native: art 1: .*sprites compiled in .* ms'
    viewer='civ3r-native: viewer started'
}

$program=Join-Path $runtime 'wine-host.exe'
$arguments='--wine-components "'+$application+'"'
$p=New-Object System.Diagnostics.Process
try {
    $p.StartInfo.FileName=$program
    $p.StartInfo.Arguments=$arguments
    $p.StartInfo.WorkingDirectory=$conquests
    $p.StartInfo.UseShellExecute=$false
    $p.StartInfo.RedirectStandardOutput=$true
    $p.StartInfo.RedirectStandardError=$true
    $environment=[ordered]@{
        WINEDLLPATH=(Join-Path $renderer 'dllpath'); WINEDLLOVERRIDES='civ3r_native=b'
        CIV3R_VIEWER='native'; CIV3R_GAME_RENDER='off'
        CIV3R_FONT_CACHE=(Join-Path $renderer 'fonts'); CIV3R_SETTINGS=(Join-Path $settings 'settings.toml')
        RUST_BACKTRACE='1'; WGPU_BACKEND='dx12'; TILE_RENDER_ADAPTER='basic render'
        PROBE_WINE_DEBUG=$WineDebug; WINEDEBUG=$GuestDebug
    }
    foreach($key in $environment.Keys){if($environment[$key]){$p.StartInfo.EnvironmentVariables[$key]=[string]$environment[$key]}}
    $started=[DateTime]::UtcNow
    [void]$p.Start();$processId=$p.Id;$sessionId=$p.SessionId
    Note "STARTED pid=$processId session=$sessionId"
    $outTask=$p.StandardOutput.ReadToEndAsync();$errTask=$p.StandardError.ReadToEndAsync()
    $found=[ordered]@{};foreach($key in $markers.Keys){$found[$key]=$false}
    while(-not $p.HasExited -and ([DateTime]::UtcNow-$started).TotalSeconds -lt $StartSeconds){
        $log=Read-BridgeLog
        foreach($key in $markers.Keys){if(-not $found[$key] -and $log -match $markers[$key]){$found[$key]=$true;Note "MARKER $key"}}
        if($found['viewer']){break}
        Start-Sleep -Seconds 2
    }
    $screenshots=@()
    $window=[ProbeWindow]::FindWindow($null,'Civ III (native renderer)')
    Note "VIEWER_WINDOW handle=$window exited=$($p.HasExited)"
    if(-not $p.HasExited){
        $screenshots+=Save-Screenshot "$ReportName-viewer-started.png"
        # WARP renders the first menu slowly.
        $deadline=[DateTime]::UtcNow.AddSeconds($MenuSeconds)
        while(-not $p.HasExited -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Seconds 2}
        if(-not $p.HasExited){$screenshots+=Save-Screenshot "$ReportName-menu.png"}
    }
    $closeSent=$false;$closedAt=$null
    $window=[ProbeWindow]::FindWindow($null,'Civ III (native renderer)')
    if(-not $p.HasExited -and $window -ne [IntPtr]::Zero){
        $closeSent=[ProbeWindow]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
        $closedAt=[DateTime]::UtcNow
        Note "WM_CLOSE posted=$closeSent"
    }
    $exitedAfterClose=$false
    if($closeSent){$exitedAfterClose=$p.WaitForExit($CloseSeconds*1000)}
    $timedOut=-not $p.HasExited
    if($timedOut){
        & "$env:SystemRoot/System32/taskkill.exe" /PID $processId /T /F|Out-Null
        if(-not $p.HasExited){$p.Kill()}
        $p.WaitForExit()
    }
    $exitCode=[int]$p.ExitCode
    $stdout=[string]$outTask.GetAwaiter().GetResult()
    $stderr=[string]$errTask.GetAwaiter().GetResult()
    $log=Read-BridgeLog
    foreach($key in $markers.Keys){$found[$key]=$log -match $markers[$key]}
    if(Test-Path -LiteralPath $bridgeLog){Copy-Item -LiteralPath $bridgeLog -Destination (Join-Path $results 'civ3-bridge.log') -Force}
    $errLines=@($stderr -split "`r?`n"|Where-Object {$_ -match ':err:'}|Select-Object -First 40)
    $row=[pscustomobject]@{
        name=$ReportName;program=$program;arguments=$arguments;workingDirectory=$conquests
        environment=$environment;installed=$installed
        pid=$processId;sessionId=$sessionId;userInteractive=[Environment]::UserInteractive
        exitCode=$exitCode;exitCodeHex=('0x{0:X8}' -f $exitCode)
        killedAfterTimeout=$timedOut;closeSent=$closeSent;exitedAfterClose=$exitedAfterClose
        secondsToCloseExit=if($closedAt -and $exitedAfterClose){([DateTime]::UtcNow-$closedAt).TotalSeconds}else{$null}
        markers=$found;screenshots=$screenshots
        wineLoadMarker=($stdout -match 'WINE_LOAD_APPLICATION status=00000000')
        wineAttachMarker=($stdout -match 'WINE_ATTACH status=00000000')
        firstErrLines=$errLines;steps=$steps
        passed=($found['adapter'] -and $found['art'] -and $found['viewer'] -and $screenshots.Count -gt 0 -and $exitedAfterClose)
        stdout=$stdout;stderr=$stderr
    }
    [IO.File]::WriteAllText((Join-Path $results "$ReportName.json"),(ConvertTo-Json $row -Depth 6),$utf8)
    [IO.File]::WriteAllText((Join-Path $results "$ReportName.stdout.txt"),$stdout,$utf8)
    [IO.File]::WriteAllText((Join-Path $results "$ReportName.stderr.txt"),$stderr,$utf8)
    Write-Output "CIV3_RENDERER pid=$processId session=$sessionId exit=$exitCode markers=$(($found.Values) -join ',') screenshots=$($screenshots.Count) closed=$exitedAfterClose passed=$($row.passed)"
    if($row.passed){exit 0}
    exit 1
} finally {$p.Dispose()}
