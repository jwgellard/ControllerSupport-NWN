# Test helper for driving NWN: screenshot, focus, click, key.
# Usage: gamectl.ps1 shot <out.png> [scale]
#        gamectl.ps1 focus
#        gamectl.ps1 click <x> <y> [right]
#        gamectl.ps1 move <x> <y>
#        gamectl.ps1 key <vk hex> [hold ms]
param([string]$cmd, [string]$a1, [string]$a2, [string]$a3)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct CI { public int cbSize; public int flags; public IntPtr h; public int x; public int y; }
  [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CI ci);
  public static string Cursor() { CI c = new CI(); c.cbSize = Marshal.SizeOf(typeof(CI)); GetCursorInfo(ref c); return "cursor " + ((c.flags & 1) != 0 ? "SHOWN" : "hidden") + " at " + c.x + "," + c.y; }
}
"@
[W]::SetProcessDPIAware() | Out-Null

function GameWindow {
    $p = Get-Process nwmain -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) { return $p.MainWindowHandle } else { return [IntPtr]::Zero }
}

switch ($cmd) {
    'shot' {
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        $scale = if ($a2) { [double]$a2 } else { 1.0 }
        if ($scale -ne 1.0) {
            $w = [int]($b.Width * $scale); $h = [int]($b.Height * $scale)
            $small = New-Object System.Drawing.Bitmap $w, $h
            $g2 = [System.Drawing.Graphics]::FromImage($small)
            $g2.InterpolationMode = 'HighQualityBicubic'
            $g2.DrawImage($bmp, 0, 0, $w, $h)
            $small.Save($a1, [System.Drawing.Imaging.ImageFormat]::Png)
        } else {
            $bmp.Save($a1, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        "saved $a1 ($($b.Width)x$($b.Height)) fg=$([W]::GetForegroundWindow()) game=$(GameWindow)"
    }
    'focus' {
        $h = GameWindow
        if ($h -eq [IntPtr]::Zero) { 'no game window'; break }
        # An Alt tap lets this process take the foreground.
        [W]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [W]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
        [W]::ShowWindow($h, 9) | Out-Null
        "focus: $([W]::SetForegroundWindow($h)) fg=$([W]::GetForegroundWindow()) game=$h"
    }
    'move' { [W]::SetCursorPos([int]$a1, [int]$a2) | Out-Null; "moved" }
    'cursor' { [W]::Cursor() }
    'click' {
        [W]::SetCursorPos([int]$a1, [int]$a2) | Out-Null
        Start-Sleep -Milliseconds 120
        if ($a3 -eq 'right') { $d = 0x8; $u = 0x10 } else { $d = 0x2; $u = 0x4 }
        [W]::mouse_event($d, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
        [W]::mouse_event($u, 0, 0, 0, [UIntPtr]::Zero)
        "clicked $a1,$a2"
    }
    'key' {
        $vk = [byte][Convert]::ToInt32($a1, 16)
        $scan = [byte][W]::MapVirtualKey($vk, 0)
        $hold = if ($a2) { [int]$a2 } else { 80 }
        [W]::keybd_event($vk, $scan, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $hold
        [W]::keybd_event($vk, $scan, 2, [UIntPtr]::Zero)
        "key $a1"
    }
}
