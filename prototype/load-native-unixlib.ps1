[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Path)
# Native control for an x86_64-native unixlib: load it in a plain x64
# process with the flags the host's dlopen uses and look up the Wine
# unixlib tables. Exit 0 when both are found, 2 when the load fails and 3
# when an export is missing.
$ErrorActionPreference='Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class NativeUnixlib {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true)] public static extern IntPtr GetProcAddress(IntPtr module, string name);
}
"@
$full=(Resolve-Path -LiteralPath $Path).Path
# LOAD_WITH_ALTERED_SEARCH_PATH, as in native_loader.c.
$module=[NativeUnixlib]::LoadLibraryExW($full,[IntPtr]::Zero,8)
$lastError=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
if($module -eq [IntPtr]::Zero){
    Write-Output "NATIVE_UNIXLIB_CONTROL path=$full loaded=False error=$lastError"
    exit 2
}
$funcs=[NativeUnixlib]::GetProcAddress($module,'__wine_unix_call_funcs')
$wow64=[NativeUnixlib]::GetProcAddress($module,'__wine_unix_call_wow64_funcs')
Write-Output "NATIVE_UNIXLIB_CONTROL path=$full loaded=True funcs=$($funcs -ne [IntPtr]::Zero) wow64funcs=$($wow64 -ne [IntPtr]::Zero)"
if($funcs -eq [IntPtr]::Zero -or $wow64 -eq [IntPtr]::Zero){exit 3}
exit 0
