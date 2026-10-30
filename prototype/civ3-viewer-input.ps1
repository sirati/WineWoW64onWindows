# Input, capture and OCR for the Civ3 renderer's native viewer window.
# Dot-sourced by run-civ3-renderer.ps1 -Play. Input is ordinary Windows input
# (SendInput) to the viewer, which forwards it to the game. Coordinates are
# client-area pixels of the viewer window.
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class ViewerInput {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public INPUTUNION u; }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(IntPtr className, string title);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint count, INPUT[] inputs, int size);

    // Client area in screen coordinates: x, y, width, height.
    public static int[] Client(IntPtr hwnd) {
        RECT r; POINT p = new POINT();
        if (!GetClientRect(hwnd, out r) || !ClientToScreen(hwnd, ref p)) return null;
        return new int[] { p.X, p.Y, r.Right - r.Left, r.Bottom - r.Top };
    }
    public static bool Activate(IntPtr hwnd) { SetForegroundWindow(hwnd); return GetForegroundWindow() == hwnd; }
    static uint Send(INPUT[] inputs) { return SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT))); }
    static INPUT Mouse(int x, int y, uint flags) {
        INPUT i = new INPUT(); i.type = 0;
        int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        i.u.mi.dx = (int)(((long)(x - vx) * 65535) / Math.Max(1, vw - 1));
        i.u.mi.dy = (int)(((long)(y - vy) * 65535) / Math.Max(1, vh - 1));
        // MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK
        i.u.mi.dwFlags = flags | 0x8000 | 0x4000;
        return i;
    }
    // Move, press and release the left button at a screen point.
    public static uint Click(int x, int y) {
        uint sent = Send(new INPUT[] { Mouse(x, y, 0x0001) });
        System.Threading.Thread.Sleep(150);
        sent += Send(new INPUT[] { Mouse(x, y, 0x0002) });
        System.Threading.Thread.Sleep(80);
        sent += Send(new INPUT[] { Mouse(x, y, 0x0004) });
        return sent;
    }
    static INPUT Key(ushort vk, bool up) {
        INPUT i = new INPUT(); i.type = 1;
        i.u.ki.wVk = vk; i.u.ki.wScan = (ushort)MapVirtualKey(vk, 0);
        i.u.ki.dwFlags = up ? 2u : 0u;
        return i;
    }
    // Press a key with optional modifiers held (VK_SHIFT 0x10, VK_CONTROL 0x11).
    public static uint Tap(ushort vk, ushort[] modifiers) {
        uint sent = 0;
        foreach (ushort m in modifiers) { sent += Send(new INPUT[] { Key(m, false) }); System.Threading.Thread.Sleep(40); }
        sent += Send(new INPUT[] { Key(vk, false) }); System.Threading.Thread.Sleep(80);
        sent += Send(new INPUT[] { Key(vk, true) }); System.Threading.Thread.Sleep(40);
        for (int k = modifiers.Length - 1; k >= 0; k--) { sent += Send(new INPUT[] { Key(modifiers[k], true) }); System.Threading.Thread.Sleep(40); }
        return sent;
    }
    public static Bitmap Capture(int[] client) {
        Bitmap b = new Bitmap(client[2], client[3], PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(b)) g.CopyFromScreen(client[0], client[1], 0, 0, new Size(client[2], client[3]));
        return b;
    }
    // Fraction of pixels that differ between two captures of the same size.
    public static double Difference(Bitmap a, Bitmap b) {
        if (a.Width != b.Width || a.Height != b.Height) return 1.0;
        Rectangle r = new Rectangle(0, 0, a.Width, a.Height);
        BitmapData da = a.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        BitmapData db = b.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try {
            int n = a.Width * a.Height; int[] pa = new int[n], pb = new int[n];
            Marshal.Copy(da.Scan0, pa, 0, n); Marshal.Copy(db.Scan0, pb, 0, n);
            int changed = 0; for (int k = 0; k < n; k++) if (pa[k] != pb[k]) changed++;
            return (double)changed / n;
        } finally { a.UnlockBits(da); b.UnlockBits(db); }
    }
    public static void SaveScaled(Bitmap b, string path, int scale) {
        using (Bitmap s = new Bitmap(b.Width * scale, b.Height * scale))
        using (Graphics g = Graphics.FromImage(s)) {
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            g.DrawImage(b, 0, 0, s.Width, s.Height);
            s.Save(path, ImageFormat.Png);
        }
    }
}
"@

$script:ocrEngine=$null;$script:ocrError=''
try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $null=[Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]
    $null=[Windows.Media.Ocr.OcrResult,Windows.Foundation,ContentType=WindowsRuntime]
    $null=[Windows.Foundation.IAsyncOperation`1,Windows.Foundation,ContentType=WindowsRuntime]
    $null=[Windows.Graphics.Imaging.BitmapDecoder,Windows.Graphics,ContentType=WindowsRuntime]
    $null=[Windows.Graphics.Imaging.SoftwareBitmap,Windows.Foundation,ContentType=WindowsRuntime]
    $null=[Windows.Storage.StorageFile,Windows.Storage,ContentType=WindowsRuntime]
    $null=[Windows.Storage.Streams.RandomAccessStream,Windows.Storage.Streams,ContentType=WindowsRuntime]
    $script:asTask=([System.WindowsRuntimeSystemExtensions].GetMethods()|Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'})[0]
    $script:ocrEngine=[Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    if(-not $script:ocrEngine){$script:ocrError='no OCR language'}
} catch {$script:ocrError=$_.Exception.Message}
function Wait-WinRT($operation,[Type]$type){
    $task=$script:asTask.MakeGenericMethod($type).Invoke($null,@($operation))
    if(-not $task.Wait(30000)){throw 'WinRT operation timed out'}
    $task.Result
}
# Words with client-area boxes. OCR reads a 2x copy; small game text reads better.
function Read-ViewerText([System.Drawing.Bitmap]$bitmap,[string]$scaledPath){
    if(-not $script:ocrEngine){return @()}
    try {
        [ViewerInput]::SaveScaled($bitmap,$scaledPath,2)
        $file=Wait-WinRT ([Windows.Storage.StorageFile]::GetFileFromPathAsync($scaledPath)) ([Windows.Storage.StorageFile])
        $stream=Wait-WinRT ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
        try {
            $decoder=Wait-WinRT ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
            $soft=Wait-WinRT ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
            $result=Wait-WinRT ($script:ocrEngine.RecognizeAsync($soft)) ([Windows.Media.Ocr.OcrResult])
        } finally {$stream.Dispose()}
        $lines=@()
        foreach($line in $result.Lines){
            $words=@($line.Words|ForEach-Object {
                [pscustomobject]@{text=$_.Text;x=[int]($_.BoundingRect.X/2);y=[int]($_.BoundingRect.Y/2);w=[int]($_.BoundingRect.Width/2);h=[int]($_.BoundingRect.Height/2)}})
            $lines+=[pscustomobject]@{text=$line.Text;words=$words}
        }
        return $lines
    } catch {$script:ocrError=$_.Exception.Message;return @()}
}
# Client-area centre of the first line containing the phrase's words in order.
function Find-ViewerPhrase($lines,[string]$phrase){
    $tokens=@($phrase.ToLowerInvariant() -split '\s+')
    foreach($line in $lines){
        $words=@($line.words)
        for($i=0;$i -le $words.Count-$tokens.Count;$i++){
            $match=$true
            for($k=0;$k -lt $tokens.Count;$k++){
                if(($words[$i+$k].text.ToLowerInvariant() -replace '[^a-z0-9]','') -ne ($tokens[$k] -replace '[^a-z0-9]','')){$match=$false;break}
            }
            if(-not $match){continue}
            $first=$words[$i];$last=$words[$i+$tokens.Count-1]
            $left=$first.x;$top=[Math]::Min($first.y,$last.y);$right=$last.x+$last.w;$bottom=[Math]::Max($first.y+$first.h,$last.y+$last.h)
            return [pscustomobject]@{phrase=$phrase;line=$line.text;x=[int](($left+$right)/2);y=[int](($top+$bottom)/2)}
        }
    }
    return $null
}
