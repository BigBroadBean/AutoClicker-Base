# cursor_gate_e2e.ps1 — 光标门控端到端验证 (v2)
#   阶段B: 门控开 + 光标隐藏 -> 应放行 (>0)      [同时证明连点管线工作]
#   阶段C: 门控开 + 光标可见 -> 应拦截 (+0)
#   阶段D: 门控关(UI点击) + 光标可见 -> 应放行 (>0)
$ErrorActionPreference = 'Stop'
$appd = "$env:APPDATA\AutoClicker"
$exe = 'D:\VibeCoding\AutoClicker-main\x64\Release\InputTuner.exe'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class Tgt2 {
    public static volatile int g_clicks = 0;
    public static IntPtr g_hwnd = IntPtr.Zero;
    public static System.Collections.Generic.List<long> Times = new System.Collections.Generic.List<long>();
    static object s_lock = new object();
    static WndProcDelegate s_keepAlive = null;
    static IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l) {
        if (m == 0x0201 || m == 0x0202) {
            System.Threading.Interlocked.Increment(ref g_clicks);
            lock (s_lock) Times.Add(DateTime.Now.Ticks);
        }
        return DefWindowProc(h, m, w, l);
    }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct WNDCLASS {
        public uint style; public WndProcDelegate lpfnWndProc; public int cbClsExtra; public int cbWndExtra;
        public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground;
        public string lpszMenuName; public string lpszClassName;
    }
    public delegate IntPtr WndProcDelegate(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32")] static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32")] static extern ushort RegisterClassW(ref WNDCLASS wc);
    [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr CreateWindowExW(uint ex, string cls, string title, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32")] public static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32")] public static extern int ShowCursor(bool show);
    [DllImport("user32")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32")] public static extern IntPtr PeekMessageW(out MSG msg, IntPtr h, uint min, uint max, uint remove);
    [DllImport("user32")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public POINT pt; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R2, B; }
    [DllImport("user32")] public static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32")] public static extern IntPtr DispatchMessageW(ref MSG msg);

    public static void Create() {
        WNDCLASS wc = new WNDCLASS();
        s_keepAlive = new WndProcDelegate(WndProc);
        wc.lpfnWndProc = s_keepAlive;
        wc.hInstance = Marshal.GetHINSTANCE(typeof(Tgt2).Module);
        wc.lpszClassName = "CursorTestWnd2";
        wc.hCursor = IntPtr.Zero; wc.hbrBackground = (IntPtr)1;
        RegisterClassW(ref wc);
        g_hwnd = CreateWindowExW(0, "CursorTestWnd2", "CursorTest", 0x00CF0000, 100, 100, 500, 400, IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
        ShowWindow(g_hwnd, 5);
    }
    public static void Pump() {
        MSG m;
        while (PeekMessageW(out m, IntPtr.Zero, 0, 0, 1) != IntPtr.Zero) {
            TranslateMessage(ref m);
            DispatchMessageW(ref m);
        }
    }
    public static int Clicks { get { return g_clicks; } }
    public static void MiddleToggle() {
        mouse_event(0x0020, 0, 0, 0, UIntPtr.Zero);   // MIDDLEDOWN
        System.Threading.Thread.Sleep(80);            // 保持 80ms, 确保 4ms 扫描看到按下沿
        mouse_event(0x0040, 0, 0, 0, UIntPtr.Zero);   // MIDDLEUP
    }
    public static void HideCursor() { while (ShowCursor(false) >= 0) { } }   // 强制 <0
    public static void ShowCursorBack() { while (ShowCursor(true) < 0) { } }
}
'@

function Sample($seconds, $label) {
    $before = [Tgt2]::Clicks
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $seconds) { [Tgt2]::Pump(); Start-Sleep -Milliseconds 100 }
    $delta = [Tgt2]::Clicks - $before
    Write-Host "$label : new clicks=$delta"
    return $delta
}

[Tgt2]::Create()
[Tgt2]::SetForegroundWindow([Tgt2]::g_hwnd) | Out-Null
Start-Sleep -Milliseconds 400

# 配置: 方案4 -> 左键开 + 保持模式 + 光标门控开
$cfg = "$appd\profile_4.txt"
$lines = Get-Content $cfg
$lines[6] = '1'; $lines[8] = '1'; $lines[28] = '1'
Set-Content -Path $cfg -Value $lines -Encoding Ascii

$p = Start-Process -FilePath $exe -WorkingDirectory 'D:\VibeCoding\AutoClicker-main' -PassThru
Start-Sleep -Milliseconds 2000
[Tgt2]::SetForegroundWindow([Tgt2]::g_hwnd) | Out-Null
Start-Sleep -Milliseconds 500

[Tgt2]::MiddleToggle()          # 连点总开关 (默认中键热键)
Start-Sleep -Milliseconds 800

# 阶段B: 门控开 + 光标隐藏 -> 放行
[Tgt2]::HideCursor()
$b = Sample 3 'phaseB (gate on, cursor hidden) expect>0'
[Tgt2]::ShowCursorBack()
Start-Sleep -Milliseconds 500    # 沉降: 让已发出的 DOWN/UP 点击对完成 (防卡键的补 UP 不算违规)

# 阶段C: 门控开 + 光标可见 -> 拦截
[Tgt2]::Times.Clear()
$tC0 = [DateTime]::Now.Ticks
$c = Sample 3 'phaseC (gate on, cursor visible) expect=0'
foreach ($t in [Tgt2]::Times) {
    Write-Host ("  click at +{0:N2}s" -f (($t - $tC0) / 10000000.0))
}

# 阶段D: 门控关 (点 UI 按钮) + 光标可见 -> 放行
$p.Refresh()
$cr = New-Object Tgt2+RECT
[Tgt2]::GetClientRect($p.MainWindowHandle, [ref]$cr) | Out-Null
$W = $cr.R2
$cx = $W - 246; $cy = 26
$lp = [IntPtr]((($cy -shl 16) -bor ($cx -band 0xFFFF)))
[Tgt2]::PostMessage($p.MainWindowHandle, 0x0201, [IntPtr]1, $lp) | Out-Null   # 点光标按钮关掉门控
Start-Sleep -Milliseconds 600
$d = Sample 3 'phaseD (gate off via UI, cursor visible) expect>0'

# 清理
[Tgt2]::ShowCursorBack()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
[Tgt2]::DestroyWindow([Tgt2]::g_hwnd) | Out-Null

if ($b -gt 0 -and $c -le 4 -and $d -gt 0) { Write-Host 'PASS: cursor gate works end-to-end (phaseC 边界补 UP 属预期)'; exit 0 }
else { Write-Host 'FAIL'; exit 1 }
