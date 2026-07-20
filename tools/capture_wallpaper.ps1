# Captures the LeniaWallpaperView window (composited via DWM) to a PNG.
param([string]$Out = "wallpaper_capture.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class Cap {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr hwnd, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static IntPtr found = IntPtr.Zero;
    static bool Check(IntPtr hwnd, IntPtr lp) {
        var sb = new StringBuilder(64);
        GetClassName(hwnd, sb, 64);
        if (sb.ToString() == "LeniaWallpaperView") { found = hwnd; return false; }
        EnumChildWindows(hwnd, Check, IntPtr.Zero);
        return found == IntPtr.Zero;
    }
    public static IntPtr Find() {
        EnumWindows(Check, IntPtr.Zero);
        return found;
    }
}
"@
$hwnd = [Cap]::Find()
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "wallpaper window not found"; exit 1 }
$r = New-Object Cap+RECT
[Cap]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Cap]::PrintWindow($hwnd, $hdc, 2)  # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
Write-Output "saved $Out ($w x $h) printOk=$ok"
