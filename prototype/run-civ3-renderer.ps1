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
    [int]$StepSeconds=300,
    # A settings.toml the bridge wrote in an earlier run; its first_run
    # section keeps Preferences from opening over the title menu.
    [string]$SettingsFrom=''
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
$settingsCopied=$false
if($SettingsFrom -and (Test-Path -LiteralPath $SettingsFrom)){
    Copy-Item -LiteralPath $SettingsFrom -Destination (Join-Path $settings 'settings.toml') -Force;$settingsCopied=$true
}
$installed=foreach($name in 'IFC23.dll','ifc23_orig.dll','civ3r_native.dll'){
    $path=Join-Path $conquests $name
    [pscustomobject]@{path=$path;sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash;size=(Get-Item -LiteralPath $path).Length}
}

Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class ProbeWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    // PowerShell passes $null as "" to string parameters; take a NULL class pointer.
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(IntPtr className, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
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
# The viewer's outer rectangle against the desktop work area (above the taskbar).
function Get-ViewerPlacement([IntPtr]$window){
    $rect=New-Object ProbeWindow+RECT
    if($window -eq [IntPtr]::Zero -or -not [ProbeWindow]::GetWindowRect($window,[ref]$rect)){return $null}
    $work=[System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    [pscustomobject]@{
        window=@($rect.Left,$rect.Top,$rect.Right,$rect.Bottom)
        workArea=@($work.Left,$work.Top,$work.Right,$work.Bottom)
        insideWorkArea=($rect.Left -ge $work.Left -and $rect.Top -ge $work.Top -and $rect.Right -le $work.Right -and $rect.Bottom -le $work.Bottom)
    }
}
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
    if(-not $client){return [pscustomobject]@{screenshot=$null;client=$null}}
    $bitmap=[ViewerInput]::Capture($client)
    try {
        $path=Join-Path $results "$ReportName-$name.png"
        $bitmap.Save($path,[System.Drawing.Imaging.ImageFormat]::Png)
        [pscustomobject]@{screenshot=$path;client=$client}
    } finally {$bitmap.Dispose()}
}
# The bridge's text trace (CIV3R_TEXT_TRACE=1): its latest block names the
# screen and lists each visible text run at its pen x and baseline y, in
# the game's 1024x768 coordinates.
function Get-TextTrace{
    $lines=@((Read-BridgeLog) -split "`r?`n")
    for($start=$lines.Count-1;$start -ge 0;$start--){if($lines[$start] -match 'text trace: (.*?), forms'){break}}
    if($start -lt 0){return [pscustomobject]@{screen=$null;runs=@()}}
    $screen=$matches[1]
    $runs=@(for($i=$start+1;$i -lt $lines.Count;$i++){
        if($lines[$i] -match '"[^"]*"/(-?\d+) .*? at \((-?\d+),\s*(-?\d+)\).*? text "(.*)"\s*$'){
            [pscustomobject]@{text=$matches[4];x=[int]$matches[2];y=[int]$matches[3];height=[Math]::Abs([int]$matches[1])}
        }
    })
    [pscustomobject]@{screen=$screen;runs=$runs}
}
function Get-TraceTexts($trace){@($trace.runs|ForEach-Object {$_.text})}
function Get-TraceDate($trace){
    foreach($run in $trace.runs){if($run.text -match '^\s*\d{1,4}\s*(BC|AD)\s*$'){return $run.text.Trim()}}
    return $null
}
# Interface text maps to the client area as (ox, oy) + game * N/1024, from
# the bridge's latest "window at (ox, oy) ...; menus N/1024" line.
function Get-ViewerMapping{
    $found=[regex]::Matches((Read-BridgeLog),'window at \((-?\d+), (-?\d+)\)[^\r\n]*?menus (\d+)/(\d+)')
    if($found.Count -eq 0){return $null}
    $m=$found[$found.Count-1]
    [pscustomobject]@{ox=[int]$m.Groups[1].Value;oy=[int]$m.Groups[2].Value;numerator=[int]$m.Groups[3].Value;denominator=[int]$m.Groups[4].Value}
}
# The first run matching a pattern, with a client point inside its label:
# a little right of the pen and half a line above the baseline.
function Find-TraceText($trace,[string]$pattern){
    $mapping=Get-ViewerMapping
    foreach($run in $trace.runs){
        if($run.text -notmatch $pattern -or -not $mapping){continue}
        $gameX=$run.x+[Math]::Min(20,[int]($run.height*0.25*$run.text.Length));$gameY=$run.y-[int]($run.height/2)
        $scale=$mapping.numerator/$mapping.denominator
        return [pscustomobject]@{text=$run.text;gameX=$gameX;gameY=$gameY;x=[int]($mapping.ox+$gameX*$scale);y=[int]($mapping.oy+$gameY*$scale);mapping=$mapping}
    }
    return $null
}
function Add-PlayStep([string]$name,$action,$settle,$shot,[hashtable]$extra=@{}){
    $new=Get-NewLines;$trace=Get-TextTrace
    $row=[ordered]@{step=$name;finishedUTC=[DateTime]::UtcNow.ToString('o');action=$action;settle=$settle
        screenshot=$shot.screenshot;client=$shot.client;screen=$trace.screen;texts=(Get-TraceTexts $trace)
        bridgeErrors=$new.bridge;stderrErrors=$new.stderr;exited=$p.HasExited}
    foreach($key in $extra.Keys){$row[$key]=$extra[$key]}
    $playSteps.Add([pscustomobject]$row)
    # Kept current so a timed-out case still has every finished step.
    [IO.File]::WriteAllText((Join-Path $results "$ReportName-play-steps.json"),(ConvertTo-Json -InputObject @($playSteps.ToArray()) -Depth 8),$utf8)
    Note "STEP $name screen=$($trace.screen) settled=$($settle.settled) seconds=$($settle.seconds) bridgeErrors=$($new.bridge.Count) stderrErrors=$($new.stderr.Count) exited=$($p.HasExited)"
}
# Move, hover 1 s, click; the first click after a jump may only hover, so
# click again if the viewer did not change. The viewer polls the pointer,
# so it arrives from a few pixels away: a repeat at the same point moves too.
function Invoke-ViewerClick([int]$x,[int]$y){
    $client=[ViewerInput]::Client($script:viewer)
    if(-not $client){return [pscustomobject]@{clientX=$x;clientY=$y;error='no viewer window'}}
    $screenX=$client[0]+$x;$screenY=$client[1]+$y
    $sent=[ViewerInput]::Move($screenX+6,$screenY+4);Start-Sleep -Milliseconds 200
    $sent+=[ViewerInput]::Move($screenX,$screenY);Start-Sleep -Milliseconds 1000
    $before=[ViewerInput]::Capture($client);$presses=1
    try {
        $pressedAt=[DateTime]::UtcNow;$sent+=[ViewerInput]::Press($screenX,$screenY)
        Start-Sleep -Seconds 2
        $client=[ViewerInput]::Client($script:viewer)
        if($client -and -not $p.HasExited){
            $after=[ViewerInput]::Capture($client)
            try {$difference=[ViewerInput]::Difference($before,$after)} finally {$after.Dispose()}
            if($difference -lt 0.01){$sent+=[ViewerInput]::Press($screenX,$screenY);$presses=2}
        }
    } finally {$before.Dispose()}
    [pscustomobject]@{clientX=$x;clientY=$y;screenX=$screenX;screenY=$screenY;presses=$presses;inputs=$sent;pressedUTC=$pressedAt.ToString('o')}
}
$keyCodes=@{Enter=0x0D;Escape=0x1B;Space=0x20;Backspace=0x08;Shift=0x10;Ctrl=0x11}
# Space-separated keys; '+' holds modifiers, e.g. 'Ctrl+S'.
function Send-Keys([string]$sequence){
    $sent=0;$foreground=[ViewerInput]::Activate($script:viewer)
    if(-not $foreground){
        # A click on the caption activates the viewer without game input.
        $client=[ViewerInput]::Client($script:viewer)
        if($client){$sent+=[ViewerInput]::Click($client[0]+[int]($client[2]/2),$client[1]-12)}
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
# WARP can take minutes to present the first frame; the trace lists the
# title's text before the viewer shows it.
function Wait-ViewerContent([int]$maximum){
    $began=[DateTime]::UtcNow;$uniformity=1.0
    while(-not $p.HasExited -and ([DateTime]::UtcNow-$began).TotalSeconds -lt $maximum){
        $client=[ViewerInput]::Client($script:viewer);if(-not $client){break}
        $bitmap=[ViewerInput]::Capture($client)
        try {$uniformity=[ViewerInput]::Uniformity($bitmap)} finally {$bitmap.Dispose()}
        if($uniformity -lt 0.9){break}
        Start-Sleep -Seconds 2
    }
    [pscustomobject]@{seconds=[Math]::Round(([DateTime]::UtcNow-$began).TotalSeconds,1);uniformity=[Math]::Round($uniformity,3);shown=($uniformity -lt 0.9)}
}
# Whether the viewer left a screen: a new trace screen name, or a picture
# that differs from the reference in over a third of its pixels.
function Test-LeftScreen([string]$screen,$reference){
    if((Get-TextTrace).screen -ne $screen){return $true}
    $client=[ViewerInput]::Client($script:viewer);if(-not $client){return $true}
    $bitmap=[ViewerInput]::Capture($client)
    try {return [ViewerInput]::Difference($reference,$bitmap) -gt 0.33} finally {$bitmap.Dispose()}
}
function Wait-Exit([int]$seconds){
    $deadline=[DateTime]::UtcNow.AddSeconds($seconds)
    while(-not $p.HasExited -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100}
    if($p.HasExited){$p.ExitTime.ToUniversalTime()}else{$null}
}
# Quick Start, found a city, end the turn, save and quit, located through
# the bridge's text trace. Escape quits from the title menu at once, so it
# is pressed only on the map, exactly once, for the quit prompt.
function Invoke-Play([datetime]$viewerSeen){
    $state=[ordered]@{titleFound=$false;mapDate=$null;turnAdvanced=$false;saveExists=$false;exitedThroughGame=$false;secondsConfirmToExit=$null}
    $saves=Join-Path $conquests 'Saves'
    # 0. Title menu.
    $target=$null;$deadline=$viewerSeen.AddSeconds(120)
    while(-not $p.HasExited -and [DateTime]::UtcNow -lt $deadline){
        $target=Find-TraceText (Get-TextTrace) '^\s*Quick Start\s*$';if($target){break};Start-Sleep -Seconds 2
    }
    $shown=Wait-ViewerContent 300
    $settle=Wait-Viewer 3 60;$shot=Save-ViewerStep '00-title'
    Add-PlayStep '00-title' $null $settle $shot @{target=$target;shown=$shown}
    $state.titleFound=[bool]$target
    if(-not $target -or -not $shown.shown -or $p.HasExited){return [pscustomobject]$state}
    # 1. Quick Start, clicked again while the title stays; the map is up
    # once the trace shows the date.
    $action=@();$reference=[ViewerInput]::Capture([ViewerInput]::Client($script:viewer))
    try {
        for($attempt=1;$attempt -le 3 -and -not $p.HasExited;$attempt++){
            $action+=Invoke-ViewerClick $target.x $target.y
            $deadline=[DateTime]::UtcNow.AddSeconds(30);$left=$false
            while(-not $p.HasExited -and [DateTime]::UtcNow -lt $deadline -and -not ($left=Test-LeftScreen 'title' $reference)){Start-Sleep -Seconds 1}
            if($left){break}
        }
    } finally {$reference.Dispose()}
    $clicked=[DateTime]::UtcNow
    while(-not $p.HasExited -and ([DateTime]::UtcNow-$clicked).TotalSeconds -lt $StepSeconds){
        $state.mapDate=Get-TraceDate (Get-TextTrace);if($state.mapDate){break};Start-Sleep -Seconds 3
    }
    $settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '01a-quick-start'
    Add-PlayStep '01a-quick-start' $action $settle $shot @{date=$state.mapDate}
    if(-not $state.mapDate -or $p.HasExited){return [pscustomobject]$state}
    # A dialog or advisor popup at the start takes Enter.
    for($round=1;$round -le 3 -and -not $p.HasExited;$round++){
        $trace=Get-TextTrace
        if($trace.screen -notmatch 'popup|dialog|advisor|message'){break}
        $action=Send-Keys 'Enter';$settle=Wait-Viewer 3 60;$shot=Save-ViewerStep "01b-popup-$round"
        Add-PlayStep "01b-popup-$round" $action $settle $shot @{screenBefore=$trace.screen}
    }
    # 2. Found a city: B, Enter at the name dialog, Escape if the city screen opened.
    $action=Send-Keys 'B';$settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '02a-build'
    Add-PlayStep '02a-build' $action $settle $shot
    $action=Send-Keys 'Enter';$settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '02b-name'
    Add-PlayStep '02b-name' $action $settle $shot
    $trace=Get-TextTrace
    if($trace.screen -match 'city'){
        $action=Send-Keys 'Escape';$settle=Wait-Viewer 3 120;$shot=Save-ViewerStep '02c-city-screen'
        Add-PlayStep '02c-city-screen' $action $settle $shot @{screenBefore=$trace.screen}
    }
    # 3. End the turn: Space gives each unit no orders; Enter once the game
    # asks to end the turn, or once Space no longer changes anything.
    $dateBefore=Get-TraceDate (Get-TextTrace);$unchanged=0;$previous=$null
    for($round=1;$round -le 15 -and -not $state.turnAdvanced -and -not $p.HasExited;$round++){
        $trace=Get-TextTrace
        $prompt=@($trace.runs|Where-Object {$_.text -match 'end\W.*turn|turn\W.*end|press\W+enter'}|ForEach-Object {$_.text})
        $keys=if($prompt.Count -or $unchanged -ge 2){'Enter'}else{'Space'}
        $action=Send-Keys $keys;$settle=Wait-Viewer 3 $(if($keys -eq 'Enter'){180}else{30})
        $after=Get-TextTrace;$dateAfter=Get-TraceDate $after
        $state.turnAdvanced=[bool]($dateBefore -and $dateAfter -and $dateAfter -ne $dateBefore)
        $texts=(Get-TraceTexts $after) -join "`n"
        if($keys -eq 'Space' -and $texts -eq $previous -and $settle.difference -lt 0.005){$unchanged++}else{$unchanged=0}
        $previous=$texts
        $shot=Save-ViewerStep "03-end-turn-$round"
        Add-PlayStep "03-end-turn-$round" $action $settle $shot @{prompt=$prompt;dateBefore=$dateBefore;dateAfter=$dateAfter;turnAdvanced=$state.turnAdvanced}
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
    $state.saveExists=Test-Path -LiteralPath $saveFile
    Add-PlayStep '04c-saved' $action $settle $shot @{saveFile=$saveFile;saveExists=$state.saveExists;saved=$saved}
    # 5. Escape once for the quit prompt, then its Yes/Quit button.
    $action=Send-Keys 'Escape';$settle=Wait-Viewer 3 60;$shot=Save-ViewerStep '05a-quit-prompt'
    Add-PlayStep '05a-quit-prompt' $action $settle $shot
    $exitedAt=$null;$confirmedAt=$null
    for($round=1;$round -le 2 -and -not $p.HasExited;$round++){
        $choice=Find-TraceText (Get-TextTrace) '^\s*(Yes|Quit)\b'
        if(-not $choice){break}
        $action=Invoke-ViewerClick $choice.x $choice.y;$confirmedAt=[DateTime]::Parse($action.pressedUTC,$null,[Globalization.DateTimeStyles]::RoundtripKind)
        $exitedAt=Wait-Exit 60
        $settle=$null;$shot=if($p.HasExited){[pscustomobject]@{screenshot=$null;client=$null}}else{$settle=Wait-Viewer 3 30;Save-ViewerStep "05b-confirm-$round"}
        Add-PlayStep "05b-confirm-$round" $action $settle $shot @{choice=$choice;exitedUTC=if($exitedAt){$exitedAt.ToString('o')}else{$null}}
        if($exitedAt){break}
    }
    $state.exitedThroughGame=[bool]$exitedAt
    if($exitedAt -and $confirmedAt){$state.secondsConfirmToExit=[Math]::Round(($exitedAt-$confirmedAt).TotalSeconds,2)}
    [pscustomobject]$state
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
        CIV3R_TEXT_TRACE=$(if($Play){'1'}else{''})
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
    $placement=Get-ViewerPlacement $window
    Note "VIEWER_PLACEMENT $(ConvertTo-Json $placement -Compress)"
    Note "NATIVE_MODULES $($nativeModules -join ';')"
    # Not $play: PowerShell names ignore case, and that is the -Play switch.
    $playResult=$null
    if($Play -and -not $p.HasExited -and $window -ne [IntPtr]::Zero){
        $screenshots+=Save-Screenshot "$ReportName-viewer-started.png"
        $script:viewer=$window
        if(-not $viewerSeen){$viewerSeen=[DateTime]::UtcNow}
        $playResult=Invoke-Play $viewerSeen
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
    $viewerWindowLines=@($log -split "`r?`n"|Where-Object {$_ -match 'viewer: asking for a|fitting decorated window'})
    $row=[pscustomobject]@{
        name=$ReportName;program=$program;arguments=$arguments;workingDirectory=$conquests
        environment=$environment;installed=$installed;settingsFrom=$SettingsFrom;settingsCopied=$settingsCopied
        nativeControl=$nativeControl;negativeControl=$negativeControl
        pid=$processId;sessionId=$sessionId;userInteractive=[Environment]::UserInteractive
        exitCode=$exitCode;exitCodeHex=('0x{0:X8}' -f $exitCode)
        killedAfterTimeout=$timedOut;closeSent=$closeSent;exitedAfterClose=$exitedAfterClose
        secondsToCloseExit=if($closedAt -and $exitedAfterClose){([DateTime]::UtcNow-$closedAt).TotalSeconds}else{$null}
        markers=$found;screenshots=$screenshots;nativeModules=$nativeModules;play=$playResult;playSteps=$playSteps
        placement=$placement;viewerWindowLines=$viewerWindowLines
        wineLoadMarker=($stdout -match 'WINE_LOAD_APPLICATION status=00000000')
        wineAttachMarker=($stdout -match 'WINE_ATTACH status=00000000')
        firstErrLines=$errLines;unixlibLines=$unixlibLines;steps=$steps
        passed=if($Play){[bool]($found['adapter'] -and $found['art'] -and $found['viewer'] -and $playResult -and $playResult.turnAdvanced -and $playResult.saveExists -and $playResult.exitedThroughGame -and $exitCode -eq 0)}
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
