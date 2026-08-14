# enc_dll.ps1: XOR 0x5A 加密两个载荷 -> AutoClicker\*.enc.dat
#   MCCombatStatusJni.dll -> mcstatus_enc.dat   (手动映射/APC 注入载荷)
#   glfw_proxy.dll        -> glfw_proxy_enc.dat (glfw 代理: 游戏启动自行加载, 零注入)
# 由 AutoClicker.vcxproj 的 PreBuildEvent 调用。
param([string]$In = "", [string]$Out = "")
$root = Split-Path -Parent $PSScriptRoot

function Enc([string]$src, [string]$dst) {
    $s = (Resolve-Path $src).Path
    $b = [System.IO.File]::ReadAllBytes($s)
    for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = $b[$i] -bxor 0x5A }
    [System.IO.File]::WriteAllBytes($dst, $b)
    Write-Host ("enc_dll: {0} -> {1} ({2} bytes)" -f $s, $dst, $b.Length)
}

if ($In -and $Out) {
    Enc $In $Out
} else {
    Enc (Join-Path $root 'MCCombatStatusJni.dll') (Join-Path $root 'AutoClicker\mcstatus_enc.dat')
    Enc (Join-Path $root 'glfw_proxy.dll') (Join-Path $root 'AutoClicker\glfw_proxy_enc.dat')
}
