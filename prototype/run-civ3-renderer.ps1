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
    [string]$GuestDebug='err+all,fixme+all',
    # Play Quick Start, found a city, end a turn, save and exit through the
    # game's menu with input to the viewer window (civ3-viewer-input.ps1).
    [switch]$Play,
    [int]$StepSeconds=300
)
# Civ3 with the civ3-renderer bridge (IFC23.dll) and its x64 unixlib over the
# game copy. The guest keeps Wine's null display driver; the renderer opens
# its own native window. Pass: the bridge log's adapter, art and viewer lines,
# a desktop screenshot, then the process ends after the viewer is closed.
# wine-host.exe writes its output straight to the result files, so they
# survive this runner. A native control first loads the x64 unixlib alone.
# With -Play the pass also needs the turn to advance, the save file and an
# exit code 0 after the game's own Exit.
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
    // PowerShell passes $null as "" to string parameters; take a NULL class pointer.
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(IntPtr className, string title);
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
# The bridge keeps its log open for writing; read it without a lock.
function Read-Shared([string]$path){
    try {
        $stream=New-Object IO.FileStream($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite, Delete')
        try {(New-Object IO.StreamReader($stream,$utf8)).ReadToEnd()} finally {$stream.Dispose()}
    } catch {''}
}
function Read-BridgeLog{Read-Shared $bridgeLog}
# Natively loaded civ3r_native modules of wine-host.exe; the guest's PE is
# mapped by Wine's loader and is not in this list.
function Get-NativeUnixlibModules([int]$id){
    try {@((Get-Process -Id $id).Modules|Where-Object {$_.ModuleName -like 'civ3r_native*'}|ForEach-Object {$_.FileName})}
    catch {@("error: $($_.Exception.Message)")}
}
if($Play){. (Join-Path $root 'civ3-viewer-input.ps1')}
$playSteps=New-Object System.Collections.Generic.List[object]
$script:bridgeSeen=0;$script:stderrSeen=0
# ERROR/WARN bridge lines and error stderr lines since the previous step.
function Get-NewLines{
    $bridge=Read-BridgeLog;$err=Read-Shared $stderrPath
    $newBridge=if($bridge.Length -gt $script:bridgeSeen){$bridge.Substring($script:bridgeSeen)}else{''}
    $newErr=if($err.Length -gt $script:stderrSeen){$err.Substring($script:stderrSeen)}else{''}
    $script:bridgeSeen=$bridge.Length;$script:stderrSeen=$err.Length
    [pscustomobject]@{
        bridge=@($newBridge -split "`r?`n"|Where-Object {$_ -match '\b(ERROR|WARN)\b'})
        stderr=@($newErr -split "`r?`n"|Where-Object {$_ -match ':err:|USER_CALLBACK_FAILED|NATIVE_|UNIMPLEMENTED|SHADOWS'})
    }
}
# Wait until two captures 2 s apart differ in under 2% of pixels.
function Wait-Viewer([int]$minimum,[int]$maximum){
    $began=[DateTime]::UtcNow;Start-Sleep -Seconds $minimum
    $difference=1.0;$settled=$false
    $client=[ViewerInput]::Client($script:viewer)
    if($client){
        $before=[ViewerInput]::Capture($client)
        try {
            while(-not $p.HasExited -and ([DateTime]::UtcNow-$began).TotalSeconds -lt $maximum){
                Start-Sleep -Seconds 2
                $client=[ViewerInput]::Client($script:viewer);if(-not $client){break}
                $after=[ViewerInput]::Capture($client)
                $difference=[ViewerInput]::Difference($before,$after);$before.Dispose();$before=$after
                if($difference -lt 0.02){$settled=$true;break}
            }
        } finally {$before.Dispose()}
    }
    [pscustomobject]@{seconds=[Math]::Round(([DateTime]::UtcNow-$began).TotalSeconds,1);difference=[Math]::Round($difference,4);settled=$settled}
}
function Save-ViewerStep([string]$name){
    $client=[ViewerInput]::Client($script:viewer)
    if(-not $client){return [pscustomobject]@{screenshot=$null;client=$null;lines=@();text=@()}}
    $bitmap=[ViewerInput]::Capture($client)
    try {
        $path=Join-Path $results "$ReportName-$name.png"
        $bitmap.Save($path,[System.Drawing.Imaging.ImageFormat]::Png)
        $scaled=Join-Path $results "ocr-$name.png"
        $lines=@(Read-ViewerText $bitmap $scaled)
        Remove-Item -LiteralPath $scaled -ErrorAction SilentlyContinue
        [pscustomobject]@{screenshot=$path;client=$client;lines=$lines;text=@($lines|ForEach-Object {$_.text})}
    } finally {$bitmap.Dispose()}
}
function Add-PlayStep([string]$name,$action,$settle,$shot,[hashtable]$extra=@{}){
    $new=Get-NewLines
    $row=[ordered]@{step=$name;finishedUTC=[DateTime]::UtcNow.ToString('o');action=$action;settle=$settle
        screenshot=$shot.screenshot;client=$shot.client;text=$shot.text;bridgeErrors=$new.bridge;stderrErrors=$new.stderr;exited=$p.HasExited}
    foreach($key in $extra.Keys){$row[$key]=$extra[$key]}
    $playSteps.Add([pscustomobject]$row)
    Note "STEP $name settled=$($settle.settled) seconds=$($settle.seconds) bridgeErrors=$($new.bridge.Count) stderrErrors=$($new.stderr.Count) exited=$($p.HasExited)"
}
function Invoke-ViewerClick([int]$x,[int]$y){
    $client=[ViewerInput]::Client($script:viewer)
    $sent=[ViewerInput]::Click($client[0]+$x,$client[1]+$y)
    [pscustomobject]@{clientX=$x;clientY=$y;screenX=$client[0]+$x;screenY=$client[1]+$y;inputs=$sent}
}
$keyCodes=@{Enter=0x0D;Escape=0x1B;Space=0x20;Backspace=0x08;Shift=0x10;Ctrl=0x11}
# Space-separated keys; '+' holds modifiers, e.g. 'Shift+Enter', 'Ctrl+S'.
function Send-Keys([string]$sequence){
    $sent=0;$foreground=[ViewerInput]::Activate($script:viewer)
    if(-not $foreground){
        # A click on the caption activates the viewer without game input.
        $client=[ViewerInput]::Client($script:viewer)
        $sent+=[ViewerInput]::Click($client[0]+[int]($client[2]/2),$client[1]-12)
        $foreground=[ViewerInput]::GetForegroundWindow() -eq $script:viewer
    }
    foreach($token in ($sequence -split ' ')){
        $codes=@(foreach($part in ($token -split '\+')){
            if($keyCodes.ContainsKey($part)){$keyCodes[$part]}
            elseif($part -cmatch '^[A-Z0-9]$'){[int][char]$part}
            else{throw "Unknown key: $part"}
        })
        $modifiers=[uint16[]]@(if($codes.Count -gt 1){$codes[0..($codes.Count-2)]})
        $sent+=[ViewerInput]::Tap([uint16]$codes[-1],$modifiers)
    }
    [pscustomobject]@{keys=$sequence;foreground=$foreground;inputs=$sent}
}
function Get-HudDate($lines){
    foreach($line in $lines){if($line.text -match '\d{1,4}\s*(BC|AD)\b'){return $matches[0]}}
    return $null
}
function Find-FirstPhrase($lines,[string[]]$phrases){
    foreach($phrase in $phrases){$found=Find-ViewerPhrase $lines $phrase;if($found){return $found}}
    return $null
}
function Invoke-Play([datetime]$viewerSeen){
    $saves=Join-Path $conquests 'Saves'
    # The bridge's first-run Preferences clicks come within about 20 s.
    while(-not $p.HasExited -and ([DateTime]::UtcNow-$viewerSeen).TotalSeconds -lt 25){Start-Sleep -Seconds 1}
    $settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '00-title'
    $target=Find-ViewerPhrase $shot.lines 'Quick Start';$closed=@()
    for($round=1;-not $target -and $round -le 3 -and -not $p.HasExited;$round++){
        $button=Find-FirstPhrase $shot.lines @('Cancel','Close','OK','Accept','Done')
        if(-not $button){break}
        $closed+=Invoke-ViewerClick $button.x $button.y
        $settle=Wait-Viewer 3 60;$shot=Save-ViewerStep "00-title-$round"
        $target=Find-ViewerPhrase $shot.lines 'Quick Start'
    }
    Add-PlayStep '00-title' @{closedDialogs=$closed} $settle $shot @{ocrAvailable=[bool]$script:ocrEngine;ocrError=$script:ocrError}
    # 1. Quick Start, then Enter for a dialog or advisor popup at the start.
    $point=if($target){$target}else{[pscustomobject]@{x=233;y=342;line='renderer coordinates'}}
    $action=Invoke-ViewerClick $point.x $point.y
    $clicked=[DateTime]::UtcNow
    $settle=Wait-Viewer 10 $StepSeconds;$shot=Save-ViewerStep '01a-quick-start'
    # A still loading screen also settles; the map has the HUD date.
    while($script:ocrEngine -and -not (Get-HudDate $shot.lines) -and -not $p.HasExited -and
          ([DateTime]::UtcNow-$clicked).TotalSeconds -lt $StepSeconds){
        $settle=Wait-Viewer 5 60;$shot=Save-ViewerStep '01a-quick-start'
    }
    Add-PlayStep '01a-quick-start' $action $settle $shot @{target=$point}
    $action=Send-Keys 'Enter';$settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '01b-enter'
    Add-PlayStep '01b-enter' $action $settle $shot
    # 2. Build a city, accept its name, leave the city screen if it opened.
    $action=Send-Keys 'B';$settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '02a-build'
    Add-PlayStep '02a-build' $action $settle $shot
    $action=Send-Keys 'Enter';$settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '02b-name'
    Add-PlayStep '02b-name' $action $settle $shot
    $cityScreen=Find-FirstPhrase $shot.lines @('Production','Shields','Citizens','Corruption')
    if($cityScreen){
        $action=Send-Keys 'Escape';$settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '02c-escape'
        Add-PlayStep '02c-escape' $action $settle $shot @{cityScreen=$cityScreen.line}
    }
    # 3. End the turn until the HUD date changes.
    $dateBefore=Get-HudDate $shot.lines;$turned=$false;$attempt=0
    foreach($keys in 'Enter','Shift+Enter','Space Enter','Space Enter','Shift+Enter'){
        $attempt++
        $action=Send-Keys $keys;$settle=Wait-Viewer 5 180;$shot=Save-ViewerStep "03-end-turn-$attempt"
        $dateAfter=Get-HudDate $shot.lines
        $turned=[bool]($dateBefore -and $dateAfter -and $dateAfter -ne $dateBefore)
        Add-PlayStep "03-end-turn-$attempt" $action $settle $shot @{dateBefore=$dateBefore;dateAfter=$dateAfter;turnAdvanced=$turned}
        if($turned -or $p.HasExited){break}
    }
    # 4. Save as wow64test.
    $action=Send-Keys 'Ctrl+S';$settle=Wait-Viewer 3 60;$shot=Save-ViewerStep '04a-save'
    Add-PlayStep '04a-save' $action $settle $shot
    $action=Send-Keys ((@('Backspace')*30+@('W','O','W','6','4','T','E','S','T')) -join ' ')
    $settle=Wait-Viewer 2 30;$shot=Save-ViewerStep '04b-name'
    Add-PlayStep '04b-name' $action $settle $shot
    $action=Send-Keys 'Enter';$settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '04c-saved'
    $saveFile=Join-Path $saves 'wow64test.SAV'
    $saved=@(Get-ChildItem -LiteralPath $saves -Filter 'wow64test*' -ErrorAction SilentlyContinue|ForEach-Object {
        [pscustomobject]@{name=$_.Name;length=$_.Length;writtenUTC=$_.LastWriteTimeUtc.ToString('o')}})
    $saveExists=Test-Path -LiteralPath $saveFile
    Add-PlayStep '04c-saved' $action $settle $shot @{saveFile=$saveFile;saveExists=$saveExists;saved=$saved}
    # 5. Escape, then Exit/Quit and its confirmation from the game's menu.
    $action=Send-Keys 'Escape';$settle=Wait-Viewer 3 60;$shot=Save-ViewerStep '05a-escape'
    Add-PlayStep '05a-escape' $action $settle $shot
    $confirmedAt=$null;$exitedAt=$null
    for($round=1;$round -le 3 -and -not $p.HasExited;$round++){
        $choice=Find-FirstPhrase $shot.lines @('Yes','Quit','Exit')
        if(-not $choice){break}
        $action=Invoke-ViewerClick $choice.x $choice.y;$confirmedAt=[DateTime]::UtcNow
        $deadline=$confirmedAt.AddSeconds(15)
        while(-not $p.HasExited -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100}
        if($p.HasExited){$exitedAt=[DateTime]::UtcNow;Add-PlayStep "05b-choose-$round" $action $null ([pscustomobject]@{screenshot=$null;client=$null;text=@()}) @{choice=$choice};break}
        $settle=Wait-Viewer 3 60;$shot=Save-ViewerStep "05b-choose-$round"
        Add-PlayStep "05b-choose-$round" $action $settle $shot @{choice=$choice}
    }
    if($confirmedAt -and -not $exitedAt){
        $deadline=[DateTime]::UtcNow.AddSeconds(60)
        while(-not $p.HasExited -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100}
        if($p.HasExited){$exitedAt=[DateTime]::UtcNow}
    }
    [pscustomobject]@{
        ocrAvailable=[bool]$script:ocrEngine;ocrError=$script:ocrError
        turnAdvanced=$turned;saveExists=$saveExists;exitedThroughGame=[bool]$exitedAt
        secondsConfirmToExit=if($confirmedAt -and $exitedAt){[Math]::Round(($exitedAt-$confirmedAt).TotalSeconds,2)}else{$null}
    }
}
$markers=[ordered]@{
    adapter='using adapter Microsoft Basic Render Driver \(Dx12\)'
    art='civ3r-native: art 1: .*sprites compiled in .* ms'
    viewer='civ3r-native: viewer started'
}

# The x64 unixlib loads natively; the i686 builtin must not (negative control).
function Invoke-NativeControl([string]$path){
    $control=New-Object System.Diagnostics.Process
    try {
        $control.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
        $control.StartInfo.Arguments='-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+(Join-Path $root 'load-native-unixlib.ps1')+'" -Path "'+$path+'"'
        $control.StartInfo.UseShellExecute=$false
        $control.StartInfo.RedirectStandardOutput=$true;$control.StartInfo.RedirectStandardError=$true
        [void]$control.Start()
        $controlOut=$control.StandardOutput.ReadToEndAsync();$controlErr=$control.StandardError.ReadToEndAsync()
        $controlTimedOut=-not $control.WaitForExit(60000)
        if($controlTimedOut){$control.Kill();$control.WaitForExit()}
        $result=[pscustomobject]@{
            program=$control.StartInfo.FileName;arguments=$control.StartInfo.Arguments
            exitCode=[int]$control.ExitCode;timedOut=$controlTimedOut
            stdout=[string]$controlOut.GetAwaiter().GetResult();stderr=[string]$controlErr.GetAwaiter().GetResult()
        }
    } finally {$control.Dispose()}
    Note "NATIVE_CONTROL exit=$($result.exitCode) $($result.stdout.Trim())"
    return $result
}
$nativeControl=Invoke-NativeControl (Join-Path $renderer 'dllpath/x86_64-native/civ3r_native.dll')
$negativeControl=Invoke-NativeControl (Join-Path $renderer 'dllpath/i386-windows/civ3r_native.dll')

$program=Join-Path $runtime 'wine-host.exe'
$arguments='--wine-components "'+$application+'"'
$stdoutPath=Join-Path $results "$ReportName.stdout.txt"
$stderrPath=Join-Path $results "$ReportName.stderr.txt"
$p=$null;$viewerSeen=$null
try {
    $environment=[ordered]@{
        WINEDLLPATH=(Join-Path $renderer 'dllpath'); WINEDLLOVERRIDES='civ3r_native=b'
        CIV3R_VIEWER='native'; CIV3R_GAME_RENDER='off'
        CIV3R_FONT_CACHE=(Join-Path $renderer 'fonts'); CIV3R_SETTINGS=(Join-Path $settings 'settings.toml')
        RUST_BACKTRACE='1'; WGPU_BACKEND='dx12'; TILE_RENDER_ADAPTER='basic render'
        PROBE_WINE_DEBUG=$WineDebug; WINEDEBUG=$GuestDebug
    }
    # This runner is a child process; its environment goes to wine-host.exe.
    foreach($key in $environment.Keys){if($environment[$key]){Set-Item -LiteralPath "env:$key" -Value ([string]$environment[$key])}}
    $started=[DateTime]::UtcNow
    $p=Start-Process -FilePath $program -ArgumentList $arguments -WorkingDirectory $conquests -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    # Keep the process handle so ExitCode stays readable after exit.
    $null=$p.Handle
    $processId=$p.Id;$sessionId=$p.SessionId
    Note "STARTED pid=$processId session=$sessionId"
    $found=[ordered]@{};foreach($key in $markers.Keys){$found[$key]=$false}
    while(-not $p.HasExited -and ([DateTime]::UtcNow-$started).TotalSeconds -lt $StartSeconds){
        $log=Read-BridgeLog
        foreach($key in $markers.Keys){if(-not $found[$key] -and $log -match $markers[$key]){$found[$key]=$true;Note "MARKER $key"}}
        if($found['viewer']){$viewerSeen=[DateTime]::UtcNow;break}
        Start-Sleep -Seconds 2
    }
    $screenshots=@()
    $window=[ProbeWindow]::FindWindow([IntPtr]::Zero,'Civ III (native renderer)')
    Note "VIEWER_WINDOW handle=$window exited=$($p.HasExited)"
    $nativeModules=if(-not $p.HasExited){Get-NativeUnixlibModules $processId}else{@()}
    Note "NATIVE_MODULES $($nativeModules -join ';')"
    $play=$null
    if($Play -and -not $p.HasExited -and $window -ne [IntPtr]::Zero){
        $screenshots+=Save-Screenshot "$ReportName-viewer-started.png"
        $script:viewer=$window
        if(-not $viewerSeen){$viewerSeen=[DateTime]::UtcNow}
        $play=Invoke-Play $viewerSeen
        if(-not $p.HasExited){$screenshots+=Save-Screenshot "$ReportName-after-play.png"}
    }
    elseif(-not $p.HasExited){
        $screenshots+=Save-Screenshot "$ReportName-viewer-started.png"
        # WARP renders the first menu slowly.
        $deadline=[DateTime]::UtcNow.AddSeconds($MenuSeconds)
        while(-not $p.HasExited -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Seconds 2}
        if(-not $p.HasExited){$screenshots+=Save-Screenshot "$ReportName-menu.png"}
    }
    $closeSent=$false;$closedAt=$null
    $window=[ProbeWindow]::FindWindow([IntPtr]::Zero,'Civ III (native renderer)')
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
    $stdout=Read-Shared $stdoutPath
    $stderr=Read-Shared $stderrPath
    $log=Read-BridgeLog
    foreach($key in $markers.Keys){$found[$key]=$log -match $markers[$key]}
    if(Test-Path -LiteralPath $bridgeLog){Copy-Item -LiteralPath $bridgeLog -Destination (Join-Path $results 'civ3-bridge.log') -Force}
    $errLines=@($stderr -split "`r?`n"|Where-Object {$_ -match ':err:'}|Select-Object -First 40)
    $unixlibLines=@($stderr -split "`r?`n"|Where-Object {$_ -match 'MISSING_NATIVE_UNIXLIB|NATIVE_UNIXLIB_LOAD_FAILED'})
    $row=[pscustomobject]@{
        name=$ReportName;program=$program;arguments=$arguments;workingDirectory=$conquests
        environment=$environment;installed=$installed
        nativeControl=$nativeControl;negativeControl=$negativeControl
        pid=$processId;sessionId=$sessionId;userInteractive=[Environment]::UserInteractive
        exitCode=$exitCode;exitCodeHex=('0x{0:X8}' -f $exitCode)
        killedAfterTimeout=$timedOut;closeSent=$closeSent;exitedAfterClose=$exitedAfterClose
        secondsToCloseExit=if($closedAt -and $exitedAfterClose){([DateTime]::UtcNow-$closedAt).TotalSeconds}else{$null}
        markers=$found;screenshots=$screenshots;nativeModules=$nativeModules;play=$play;playSteps=$playSteps
        wineLoadMarker=($stdout -match 'WINE_LOAD_APPLICATION status=00000000')
        wineAttachMarker=($stdout -match 'WINE_ATTACH status=00000000')
        firstErrLines=$errLines;unixlibLines=$unixlibLines;steps=$steps
        passed=if($Play){[bool]($found['adapter'] -and $found['art'] -and $found['viewer'] -and $play -and $play.turnAdvanced -and $play.saveExists -and $play.exitedThroughGame -and $exitCode -eq 0)}
               else{[bool]($found['adapter'] -and $found['art'] -and $found['viewer'] -and $screenshots.Count -gt 0 -and $exitedAfterClose)}
        stdout=$stdout;stderr=$stderr
    }
    [IO.File]::WriteAllText((Join-Path $results "$ReportName.json"),(ConvertTo-Json $row -Depth 8),$utf8)
    Write-Output "CIV3_RENDERER pid=$processId session=$sessionId exit=$exitCode markers=$(($found.Values) -join ',') screenshots=$($screenshots.Count) closed=$exitedAfterClose passed=$($row.passed)"
    if($row.passed){exit 0}
    exit 1
} finally {
    # wine-host.exe inherits this runner's output pipes; a runner that stops
    # early must not leave the game holding them open.
    if($p){
        if(-not $p.HasExited){& "$env:SystemRoot/System32/taskkill.exe" /PID $p.Id /T /F|Out-Null}
        $p.Dispose()
    }
}
