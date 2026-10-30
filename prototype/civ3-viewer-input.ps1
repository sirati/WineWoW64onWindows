# Input and capture for the Civ3 renderer's native viewer window.
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
    public static uint Move(int x, int y) { return Send(new INPUT[] { Mouse(x, y, 0x0001) }); }
    // Press and release the left button at a screen point.
    public static uint Press(int x, int y) {
        uint sent = Send(new INPUT[] { Mouse(x, y, 0x0002) });
        System.Threading.Thread.Sleep(80);
        sent += Send(new INPUT[] { Mouse(x, y, 0x0004) });
        return sent;
    }
    public static uint Click(int x, int y) {
        uint sent = Move(x, y);
        System.Threading.Thread.Sleep(150);
        return sent + Press(x, y);
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
    // Share of the most common colour in a sample; near 1 while the viewer is blank.
    public static double Uniformity(Bitmap a) {
        Rectangle r = new Rectangle(0, 0, a.Width, a.Height);
        BitmapData d = a.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try {
            int n = a.Width * a.Height; int[] px = new int[n];
            Marshal.Copy(d.Scan0, px, 0, n);
            var counts = new System.Collections.Generic.Dictionary<int, int>();
            int samples = 0, best = 0;
            for (int k = 0; k < n; k += 7) {
                int c; counts.TryGetValue(px[k], out c); counts[px[k]] = ++c;
                if (c > best) best = c; samples++;
            }
            return samples > 0 ? (double)best / samples : 1.0;
        } finally { a.UnlockBits(d); }
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
}
"@
