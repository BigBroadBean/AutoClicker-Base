# enc_dll.ps1: XOR 0x5A 加密 MCCombatStatusJni.dll -> AutoClicker\mcstatus_enc.dat
# 由 AutoClicker.vcxproj 的 PreBuildEvent 调用; 加密后的资源嵌入 exe,
# 运行时内存解密 + 手动映射注入, 磁盘上不再出现明文 DLL。
param([string]$In = "", [string]$Out = "")
$root = Split-Path -Parent $PSScriptRoot
if (-not $In)  { $In  = Join-Path $root 'MCCombatStatusJni.dll' }
if (-not $Out) { $Out = Join-Path $root 'AutoClicker\mcstatus_enc.dat' }
$src = (Resolve-Path $In).Path
$b = [System.IO.File]::ReadAllBytes($src)
for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = $b[$i] -bxor 0x5A }
[System.IO.File]::WriteAllBytes($Out, $b)
Write-Host ("enc_dll: {0} -> {1} ({2} bytes)" -f $src, $Out, $b.Length)
