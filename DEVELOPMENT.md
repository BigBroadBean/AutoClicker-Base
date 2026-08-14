# AutoClicker 开发文档

本文档记录 AutoClicker 的架构设计、核心机制、性能优化与踩坑经验，供后续开发维护参考。

- 语言：C++20（MSVC），仅依赖 Windows SDK
- 窗口框架：原生 Win32 + GDI 自绘（无 MFC / 无外部 UI 库）
- 构建：Visual Studio 2022+ / MSBuild，`Release | x64`

---

## 1. 模块结构

| 文件 | 职责 |
|---|---|
| `main.cpp` | 主窗口、GDI 自绘 UI、布局、命中测试、输入处理、页面切换动画、面板页；玻璃态三层渲染调度（RenderChrome/RenderChromeDyn/RenderContent/Present，代码位于 splice 进 main.cpp 的 render 区） |
| `glass.cpp` | 玻璃态渲染内核：32bpp 预乘 Alpha 分层（GLCreate/GLFree/GLClear）、软件圆角/渐变/发丝描边/软阴影光栅化（行级跨度优化 + 1px 抗锯齿）、GDI 绘制 alpha 提升、AlphaBlend 合成、UpdateLayeredWindow 呈现（客户区偏移拷入外尺寸呈现层） |
| `clicker.cpp` | 连点核心线程、精确计时、热键检测、多倍点击 Hook、实时 CPS 统计、拟人化节奏、方案切换热键 |
| `canattack.cpp` | 仅能攻击时连点 + 仅手持放置物时右键连点：共享内存轮询线程（主通道，`Local\MCCombatStatus_<PID>`，常驻运行供 HUD 用）、UDP 兜底监听线程（35785，生命周期门控）、Minecraft Java 进程自动注入线程（退避+节流+反重复注入）、门控唤醒事件 |
| `config.cpp` | 配置方案系统（`%APPDATA%\AutoClicker\profile_N.txt` ×4 + active.txt，旧单文件自动迁移；追加式、向后兼容）+ UI 状态（方案名 ui.txt、窗口位置 window.txt） |
| `overlay.cpp` | Toast 通知（无边框分层窗口，玻璃态样式 + 滑入动画；线程参数按值拷贝防悬垂） |
| `sound.cpp` | 系统提示音（`PlaySoundW`，Windows Media 目录 wav） |
| `ui.cpp` | Win11 窗口样式（暗色标题栏、圆角）+ `g_uiHwnd` + `WM_APP_PROFILE` 自定义消息 |
| `types.h` | 窗口尺寸、玻璃态色板（深空玻璃/雾白玻璃）、4 色强调色板、常量、主题访问器 |
| `clicker.h` | 全局状态变量声明、`cpsToMs`/`msToCps10` 换算 |
| `report.cpp` | HWID 使用调查上报：启动时后台 GET 上报（WinHTTP，域名+端口写死，5s 超时，fire-and-forget） |
| `httputil.cpp` | 极简 WinHTTP GET 工具（域名+端口，5s 超时，可选读响应体），report/update 共用 |
| `update.cpp` | 启动时版本检查：GET /version/latest 对比本地版本，有新版弹 MessageBox（含更新内容） |
| `servercfg.h` | 服务器域名+端口常量（写死，无配置文件），report/update 共用 |
| `versionutil.h` | 版本工具（纯函数头文件）：点分数字版本比较 + JSON 字符串提取，update 模块与单元测试共用 |

### 双版本构建（Base / Net）

同一套代码通过编译宏 `AUTOCLICKER_NET` 产出两种形态：

| 配置 | 宏 | 网络模块 | 用途 |
|---|---|---|---|
| `Release \| x64` | 定义 `AUTOCLICKER_NET` | report.cpp / update.cpp / httputil.cpp 参与编译 | 网络版：HWID 上报 + 版本检查 |
| `Release-Base \| x64` | 不定义 | 上述文件被 vcxproj Condition 排除 | 基础版：无任何网络行为 |

- `main.cpp` 中 `#include` 与 `StartHwidReporter()`/`StartVersionCheck()` 调用均以 `#ifdef AUTOCLICKER_NET` 包裹
- 头文件 report.h / update.h / httputil.h / servercfg.h 同样按配置排除（不参与编译）
- 基础版仍保留 MCCombatStatusJni 注入（仅能攻击时连点 / 仅手持放置物时右键连点）——它是本机 UDP 通信，不属于外部网络交互

### 全局状态

所有功能状态（`isstart`、`isMultiActive`、`isScrollClickActive`、`leftenabled` 等）为进程内全局变量，由 `clicker.h` 声明、`clicker.cpp` 定义。跨线程访问的（`g_clickCount`、`g_debounceUntil`）用 `std::atomic`。

---

## 2. 线程模型

| 线程 | 用途 | 关键点 |
|---|---|---|
| UI 线程 | Win32 消息循环 + 分层自绘 | `PeekMessage` 循环 + `Sleep(5)`；`WM_TIMER` 16ms 按需重绘（三层缓存的脏标志分别驱动）+ 100ms CPS 采样 + 页面切换动画推进；Present = 清空 surface + 2~3 次 AlphaBlend + BitBlt + ULW ≈ 1.5ms/帧 |
| 连点线程 | 热键扫描 + 精确点击计时 | 4ms 热键扫描周期；亚毫秒点击定时；拟人化节奏因子计算 |
| Hook 线程 | `WH_MOUSE_LL` 全局鼠标钩子 | 多倍点击、滚轮转点击；`GetMessageW` 阻塞零开销 |
| Toast 线程 | 一次性通知动画 | detach，自清理；标题/正文 wstring 按值传参（构造时深拷贝）；玻璃态 GLayer 渲染 + ULW 呈现 |
| 共享内存轮询线程 | 主通道：5ms 轮询 `Local\MCCombatStatus_<PID>`（DLL 写出的 packed 状态结构，inGame@16/canAttack@20/canPlace@24/hitType@32/targetName@52/tick@632） | **全生命周期常驻**（5ms 等待，CPU 可忽略）：除门控状态外持续刷新 HUD 快照（游戏内/命中类型/目标实体名），面板页与状态栏依赖；UDP/注入线程仍随门控启停 |
| UDP 监听线程 | 兜底通道（旧版 DLL）：绑定 127.0.0.1:35785，25ms 超时收 2 字节报文 | 生命周期门控：仅门控开启时绑定，关闭即释放端口并 park；端口被占不影响共享内存主通道 |
| 注入线程 | 扫描 javaw/java 进程，向未注入的 MC 客户端注入 DLL | 门控开启时 1s 周期；注入失败 1s→30s 指数退避 + 日志节流；两门控关闭时 park 在唤醒事件上（零 CPU） |

> 三个门控驱动线程共用同一个自动重置唤醒事件：`NotifyGateToggled()`（UI/热键任意线程调用）按消费者数量脉冲 `SetEvent`，门控一开即全部即时苏醒（"随用随上"）。

### 连点线程循环结构

```
for (;;) {
    now = steady_clock::now()
    若 now >= nextScan（4ms 周期）:
        热键边沿检测（异步切换线程不阻塞）
        auto-stop 检查
        改键防抖检查（g_debounceUntil）
    点击状态机（左右键独立）:
        leftHeld && now >= nextLeftTime → 发送 DOWN/UP（交替）
        nextLeftTime = now + 间隔
    计算下一个事件时刻 next = min(nextScan, nextLeftTime, nextRightTime)
    PreciseSleepUntil(next, 是否有点击激活)
}
```

- 左右键点击互不干扰，各自维护 `nextLeftTime` / `nextRightTime`
- `isMultiActive` 时点击暂停（多倍 Hook 接管），热键扫描不受影响
- 点击目标 = 前台窗口句柄 `mhwnd`（UI 线程每轮 `GetForegroundWindow` 刷新）

### 消息发送与输入法钩子对抗

点击消息**必须**通过动态解析的 `PostMessageA` 发送（**不可改回静态链接**）：

```cpp
typedef int(WINAPI* pPostMessageA)(HWND, UINT, WPARAM, LPARAM);
pPostMessageA MyPostMessageA =
    (pPostMessageA)GetProcAddress(LoadLibraryA("User32.dll"), "PostMessageA");
```

原因见 §7 踩坑记录（腾讯微信输入法会 inline-hook 静态导入的 user32 函数）。

---

## 3. 高性能连点引擎（半毫秒级精确计时）

### 系统定时器分辨率

程序生命周期内常驻 `timeBeginPeriod(1)`（`WinMain` 入口调用，`WM_DESTROY` 时 `timeEndPeriod`），使 `Sleep` 精度达到 1ms。**不要**在连点开关时反复调用（曾这样实现，后改为常驻）。

### PreciseSleepUntil 三级休眠

```cpp
static void PreciseSleepUntil(steady_clock::time_point target, bool precise)
{
    for (;;) {
        remain = target - now();
        us = duration_cast<microseconds>(remain).count();
        if (us > 8000)       Sleep((us - 2000) / 1000);   // 粗休眠，提前 2ms 唤醒
        else if (us > 1500)  Sleep(1);                     // 1ms 粒度
        else if (precise)    忙等（YieldProcessor 自旋）;   // 亚毫秒精调
        else                 Sleep(1);                     // 空闲绝不自旋
    }
}
```

- **precise=true**（有点击激活）：最后 ≤1.5ms 用 `YieldProcessor` 低功耗自旋，点击时刻误差 < 0.1ms
- **precise=false**（空闲）：一律真休眠，CPU ≈ 0%

### 实测精度

clicker 线程内 `steady_clock` 直接测量 300 个点击间隔（目标 25.000ms，20.00 CPS 完整周期 50ms）：

```
mean = 25.015ms   （误差 0.06%）
std  = 0.073ms    （73 微秒，亚毫秒达成）
```

外部消息泵测量（`GetMessageTime`）的 std ≈ 6ms 是**测量端噪声**（1ms 量化 + 调度延迟），非引擎误差。

### 随机 CPS

`randomCpsEnabled` 时每个点击间隔在 `±randomCpsRange` CPS 内抖动，并夹紧到 `[CPS_MIN10, cpsMax*10]`。随机数用 **xorshift64***（比 `std::rand()` 快且分布均匀）。

### 拟人化节奏（humanized rhythm）

在随机抖动**之前**先按会话（一次按住鼠标 = 一个会话）计算变速因子 `f`（`cps' = cps / f`，f<1 加速、f>1 减速，clamp [0.5, 1.5]），强度 `humanizeLevel` 1..5 线性缩放：

| 模式 | 公式 | 效果 |
|---|---|---|
| 0 均匀 | f = 1 | 关闭拟人化（与旧版一致） |
| 1 双击连招 | 会话内击次奇偶：f = 1−0.28a / 1+0.38a | 短促双击 + 组间停顿 |
| 2 呼吸波动 | f = 1 + 0.24a·sin(2πt/3.5s) | CPS 正弦起伏 (~3.5s 周期) |
| 3 疲劳递减 | f = 1 + 0.30a·(1−e^(−t/8s)) | 按住越久越慢，渐近 −30% |

左右键各自独立会话（`heldStartL/R` + `clickIdxL/R`），松开即重置。

---

## 4. 玻璃态（Glassmorphism）渲染系统

### 分层透明架构

整窗为 `WS_EX_LAYERED` 分层窗口，全部视觉由 4 个 32bpp **预乘 Alpha**（premultiplied）top-down DIB 层组成：

| 层 | 内容 | 重渲染时机 |
|---|---|---|
| `g_chrome` | 基底玻璃渐变 + 标题/方案芯片/图钉/主题 + 侧栏 + 状态栏底板 | 主题/方案/页面/置顶变化、悬停变化 |
| `g_chromeDyn` | 状态栏动态内容（状态点/文字/迷你曲线） | 10Hz 状态采样变化 |
| `g_content` | 当前页全部内容（含悬停提示） | 控件状态变化、悬停变化、页面切换、面板曲线采样 |
| `g_surface` | 合成层 | 每次 Present |

**Present**（`glass.cpp GLPresent`）：清空 surface → `AlphaBlend` chrome、chromeDyn、content（动画偏移 + 淡入不透明度）→ 拷入「窗口外尺寸」呈现层（客户区按客户区偏移）→ `UpdateLayeredWindow`。

> **关键坑（§7.16）**：`UpdateLayeredWindow` 的 `psize` 会把整个窗口（含边框）**重设为目标尺寸**！因此呈现层必须是窗口外尺寸、`psize` 恒等于窗口当前外尺寸，否则每次呈现都会把窗口缩小一个边框、无限收缩。另一个坑（§7.17）：像素内存布局是 **BGRA**，软件填充写像素时 R/B 通道顺序别写反（饱和色如蓝色强调色会变成橙色）。

### 渲染原语（glass.cpp）

- `GLFillRound` / `GLFillV` / `GLRing` / `GLShadow` / `GLHLine`：软件光栅化，**行级跨度优化**（中间像素快路径预乘 src-over，仅左右边界 2 像素做 1px 抗锯齿覆盖率），坐标越界直接钳制/丢弃（阴影外扩安全）
- `GLRing`：覆盖率 = 外圈 − 内圈，**只描边带、绝不擦除带内已有内容**（早期实现用「填充+内圈擦除」会把面板内部整个抹掉）
- `GLLiftAlphaRect`：GDI 绘制（文字/图标/折线）写 RGB 不写 alpha，画完按区域把 `alpha==0 && RGB!=0` 的像素提升为 255（文字必须用**灰度抗锯齿**字体 + TRANSPARENT 背景模式；ClearType 彩边在透明背景上会花）
- `GLBlend`：AlphaBlend AC_SRC_ALPHA（源需预乘）

### 玻璃设计语言（main.cpp）

- **面板** `GPanel`：软阴影（同心外扩、二次衰减）+ 玻璃填充 + 顶部白色高光渐变（SHEEN）+ 1px 发丝描边
- **按钮** `GButton`：默认弱玻璃 + 发丝；悬停 = 填充提亮 + 强调色描边 + 强调色光晕；选中 = 强调色填充 + 发光；按下 = 强调色半透明
- **开关/滑块/输入框/芯片/圆点** `GToggle/GSlider/GInset/GChip/GDot`：同一套玻璃语言
- **拖动优化**：滑块拖动时卡片走 `GPanelLite`（无阴影无高光），帧耗减半
- 字体：全部 `ANTIALIASED_QUALITY`（灰度 AA，适配 alpha 提升）
- 色板：`types.h` 深空玻璃（蓝调深色）/雾白玻璃双主题 + 4 色强调色；各层透明度硬编码在渲染代码的 alpha 常量里

### 实测帧耗

- Present（动画帧）：清空 1.2MB + 3×AlphaBlend + BitBlt + ULW ≈ **1.5ms** → 动画稳定 60fps
- 内容重渲染（状态变化/悬停）：~6-10ms，一次性开销，不在动画热路径
- 空闲：零重绘（按需渲染），CPU ≈ 0%

---

## 5. UI 布局与交互

### 窗口

- **无边框玻璃窗**：`WS_POPUP | WS_THICKFRAME` + `WS_EX_LAYERED`（分层窗口会禁用 DWM 非客户区渲染，带系统边框会退化出经典标题栏+加粗边框——必须无边框）。窗口尺寸 = 客户区尺寸，无 frame 换算
- 自绘标题栏窗控：最小化 / 最大化(还原) / 关闭（关闭悬停红色），另有提示音 / 置顶 / 主题 / 方案芯片
- `WM_NCHITTEST`：边缘 6px 缩放热区（最大化时关闭）→ 控件命中 → 标题条空白 = HTCAPTION（系统接管拖动与贴靠）；`WM_NCLBUTTONDBLCLK` HTCAPTION = 最大化/还原；`WM_SETCURSOR` 按边缘给方向光标
- 基底玻璃四角圆角（rad 12，最大化时 0）+ 发丝描边勾勒轮廓（无 DWM 阴影，靠描边保证边缘清晰）
- 默认 640×480，可缩放，最小 560×460；位置/尺寸记入 `window.txt`——**保存用 `WINDOWPLACEMENT.rcNormalPosition`**（最大化/最小化关闭时不能存放大后的矩形）
- DPI：启动时 `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`（Win10 1703+）
- `WM_PAINT` 仅消费（表面由 ULW 常驻，无需重画）；`WM_ERASEBKGND` 返回 1

### 结构

```
┌ 标题栏：AutoClicker v2.9 | 方案1 方案2 方案3 方案4 | 🖱光标 | 📢 | 📌置顶 | ☀/☾主题 ┐
├ 侧边栏（5 图标按钮）：连点 | 多倍 | 滚轮 | 面板 | 高级
│ 内容区：行1 = 两卡并排，行2 = 全宽卡
│   · 连点页：左键卡 | 右键卡 / 快捷键+保持+仅能攻击时连点+仅手持放置物时右键连点卡
│   · 多倍页：倍率卡 | 延迟卡 / 快捷键卡
│   · 滚轮页：全宽卡
│   · 面板页：全宽 CPS 曲线卡 / 全宽 游戏状态+方案重命名+强调色卡
│   · 高级页：CPS上限卡 | 随机CPS卡 / 定时停止+拟人化卡
└ 状态栏：连点● | 辅助● | 攻击● | 放置● | 目标名 | CPS 迷你曲线+数值 ┘
```

- 标题栏方案芯片：点击即切换配置方案（保存旧槽→加载新槽→主题/置顶/Toast 联动），悬停提示方案名
- 页面切换动画：内容区 160ms ease-out 水平滑入（标题/侧栏/状态栏静止；`SaveDC` + `IntersectClipRect` + `SetViewportOrgEx` 实现视口平移，动画结束 `RestoreDC`）
- 面板页 CPS 曲线：`WM_TIMER` 每 100ms 采样 `GetRealtimeCps()` 入 120 点环形缓冲（~12s 窗口），Polygon 面积填充 + Polyline 折线 + 最新点光环；状态栏迷你曲线取后 ~76px 窗口

### 响应式缩放

`g_lyScale = f` 由可用高度与基准行高（165/135）之比计算，clamp 到 [0.72, 1.15]，超出部分均匀分配到卡间距。所有卡内控件偏移统一经 `S(v) = (int)(v * f)` 缩放（**全局函数**，Layout 与 Paint 共用）。标签区 `S(8)..S(26)`，控件从 `S(32)` 起，保证任何缩放比下不重叠。

### 交互

- 命中测试：`g_hr[E_*]` 按当前页注册，页面切换时重建
- 滑块拖动：`WM_MOUSEMOVE` 只置 `g_dirty`，由 `WM_TIMER` 16ms 统一重绘（节流，避免每事件全量重绘）
- 键盘：方向键切换页面；输入框聚焦时方向键保留给文本
- 改键捕获：非阻塞状态机（`g_rebinding`），消息循环照常运行，Esc 或 15 秒超时取消（防止误触卡死 UI）
- 光标反馈：`WM_SETCURSOR` 命中任意可交互元素显示手型光标
- 方案名输入：`IN_PROFILE` 目标接受任意可打印字符（含 IME 中文），回车提交 / Esc 取消

### 按需重绘

`WM_TIMER`（16ms）只在实际变化时重绘（`g_dirty`）：
- 状态位快照（isstart / 页面 / 输入框焦点等）变化
- `g_clickCount` 或实时 CPS 变化
- HUD 快照变化（目标名哈希 / 游戏内 / 命中类型 / 方案 / 强调色 / 拟人化）
- 拖动中 / 页面动画中 / 面板页 CPS 采样强制重绘

空闲时零重绘，CPU ≈ 0%。

---

## 6. 配置系统

- 目录：`%APPDATA%\AutoClicker\`（`CSIDL_APPDATA`）
- **方案（profile）**：`profile_1.txt` … `profile_4.txt`，每槽一份完整设置；`active.txt` 记录当前激活槽（1..4）；**旧版单文件 `autoclickerSave.txt` 首次启动自动迁移为 `profile_1.txt`**
- 切换方案：`SwitchProfile(n)` 单锁内完成「旧槽落盘 → 改 active → 写 active.txt → 读新槽」；空白槽先 `ResetDefaultsUnlocked()` 恢复默认再载入。主题(DWM)/置顶/Toast 等 UI 副作用由调用方执行（UI 芯片路径直接在 `Click()` 内，热键路径经 `WM_APP_PROFILE` 消息转交 UI 线程）
- **UI 全局状态**（不属于方案）：`ui.txt` = 4 个方案显示名（UTF-8，面板页重命名）；`window.txt` = 窗口位置/尺寸（4 行整数）
- 方案文件格式：每行一个值，**顺序敏感**；新字段**追加在末尾** → 旧配置文件天然兼容
- 当前字段顺序（29 行）：cpsLeft10, cpsRight10, cpsMax, randomCpsEnabled, randomCpsRange, vk_key, leftenabled, rightenabled, keepClicke, vk_multi_key, multiMul, multiDelayMs, vk_scroll_key, scrollClickButton, vk_scroll_lr_key, theme, autoStopEnabled, autoStopSeconds, topmost, canAttackOnlyClick, vk_canattack_key, placeOnlyRightClick, vk_place_key, humanizeMode, humanizeLevel, accentIdx, vk_profile_key, soundEnabled（提示音总开关，默认开；所有 `Play*Sound` 在 `sound.cpp` 统一门控，仅 `PlayToggleSound` 不受限）, cursorOnlyClick（光标门控：仅系统光标不可见时连点）
- 载入时对每个值做范围校验（防手改损坏）；跨线程写入由全局 `std::mutex` 串行化（§7.14）

---

## 7. 踩坑记录（重要经验）

### 7.1 微信输入法（WeType）inline-hook 破坏宽字符窗口 API

**现象**：窗口标题只显示第一个字符（"AutoClicker" → "A"），`GetWindowTextW` 返回长度 1。

**排查链**：
1. 代码、字符串数据（`strings`/反汇编确认 `L"AutoClicker"` 完整、R8 指针正确）、导入表（`CreateWindowExW` ✓）全部正常
2. 最小复现程序同样中招 → 排除项目代码问题
3. C# P/Invoke（直查导出表）创建窗口标题正常 → 定位到 C++ 静态导入路径
4. `GetProcAddress("CreateWindowExW")` 动态调用**仍中招** → 排除 IAT 钩子，确认为**函数体 inline patch**
5. 进程模块列表发现 `wetype_tip_core.dll`（微信输入法注入）；PowerShell 进程（未注入）正常
6. 交叉实验：`CreateWindowExA` / `SetWindowTextA`（ANSI 路径）完全正常

**根因**：输入法把宽字符 API 的字符串参数按 ANSI 读取，遇第一个 `\0` 截断（"AutoClicker" 宽字符首字节 'A' + `\0`）。

**对策**：
- 窗口创建与标题设置改用 **ANSI 版本**（`RegisterClassA` / `CreateWindowExA` / `SetWindowTextA` / `LoadIconA`）
- 点击消息用 `GetProcAddress` 动态解析 `PostMessageA`（绕过 IAT/inline 钩子）

**教训**：GUI 程序在装有国产输入法的环境里，宽字符窗口 API 都可能被劫持；ANSI API + 动态解析是最稳妥的对抗手段。

### 7.2 GDI `RoundRect` 的圆角参数是"椭圆宽高"而非半径

`RoundRect(hdc, l, t, r, b, w, h)` 的 `w/h` 是圆角椭圆总宽高 → **圆角半径 = w/2**。原代码直接传 `radius`，导致按钮实际圆角只有设计值一半，与阴影 Region（按半径 `*2` 计算）不一致 → 高亮与按钮轮廓"不重合"。

**修复**：`FillRoundRect`/`DrawRoundRect` 内部统一传 `radius * 2`，全项目 `radius` 语义 = 圆角半径。

### 7.3 阴影层必须"同心"而非"平移"

圆角矩形整体平移（半径不变、圆心偏移）画阴影，圆角处弧线不平行 → 阴影厚度不均匀。正确做法：
- 外阴影：外扩 i **且半径 +i**（同心），裁剪象限表达方向
- 内阴影：内缩 i **且半径 −i**（同心腐蚀），条带 = 按钮 − 腐蚀区，按水平中线分上半暗/下半亮

用 `CreateRoundRectRgn` + `CombineRgn` + `FillRgn` 实现精确几何，圆角处处等距。

### 7.4 `Sleep(0)` 在高精度定时器下的忙循环

`timeBeginPeriod(1)` 后 `Sleep(0)` 几乎立即返回（就绪队列为空时），若在等待循环里调用会形成忙循环 → 空闲 CPU 14.8%。**修复**：空闲路径用 `Sleep(1)` 真休眠，只有点击激活路径才允许忙等（≤1.5ms 窗口）。

### 7.5 中文源码在 GBK 代码页下编译错误

UTF-8 无 BOM 的 `.cpp` 含中文注释时，MSVC 按 GBK(936) 解析，多字节序列可能吞掉换行符 → 后续代码被注释掉（C4819 / 未声明标识符）。**修复**：vcxproj 全部配置加 `/utf-8` 编译选项。

### 7.6 窗口底部被任务栏遮挡

`CW_USEDEFAULT` 定位可能让窗口底部探出工作区（截图验证状态栏被任务栏盖住）。**修复**：启动时 `SPI_GETWORKAREA` 限制窗口高度并居中。

### 7.7 侧边栏改造时预设按钮坐标复制错误

Layout 中右侧卡片（右键/延迟）的预设按钮 x 坐标错误复用了左侧卡的值，两组按钮重叠、选中态被覆盖（连点页因左右 CPS 恰好相同未暴露）。**教训**：两列布局中右列的 x 必须用 `card[1].left` 重新计算。

### 7.8 改键捕获无限阻塞 UI

点击"快捷键"按钮后 `CaptureKey` 阻塞等键，误触即卡死。**第一版修复**：Esc 取消 + 15 秒超时——但窗口仍阻塞（消息循环不跑，不能拖动/重绘，用户以为死机）。**最终修复**：彻底非阻塞——点击后进入 `g_rebinding` 状态（按钮显示"请按下新键…"），`WM_KEYDOWN`/`WM_SYSKEYDOWN`/`WM_MBUTTONDOWN`/`WM_XBUTTONDOWN` 接键提交、Esc 清除、`WM_TIMER` 15 秒超时取消（不改绑定）、点击其他位置取消。**教训**：模态改键必须改造为消息驱动状态机，超时兜底≠用户体验。

### 7.9 计数统计调试的经典自坑

写内部统计时 `if (s_cnt == 0) s_last = ns; else { ... s_cnt++; }` —— s_cnt 永为 0，else 永不执行。**教训**：初始化标志与计数变量要分离（`s_cnt = -1` 表示未初始化）。

### 7.10 点击页值文本矩形左右反转（v2.4）

点击页左右卡片共用循环绘制 CPS/毫秒文本，`RECT r` 的右边界错误地固定取 `L.track[SL_L].right`：`i=1`（右键卡片）时 `r.left = card[1].left + 20 > r.right = track[SL_L].right`，矩形反转导致 GDI 绘制错乱——右键的 CPS 文本消失/画到左卡片毫秒区域，与左键“25 毫秒”重叠（用户表现为“仅显示 20”）。多倍页因分别用 `SL_MUL`/`SL_DEL` 的 track 而未受影响。**修复**：改为 `L.track[SL_L + i].right`（`SL_L=0, SL_R=1`）。**教训**：两列对称布局中右列的所有坐标都必须由右列自己的布局值推导，不能复用左列。

### 7.11 快捷键空值（0）被配置加载过滤（v2.4）

按 Esc 清除快捷键后 `CaptureKey` 写 `vk=0` 并保存，但 `LoadConfig` 的读取条件是 `v >= 1 && v <= 255`，0 被拒绝加载，变量保持默认值——下次启动快捷键“复活”。后加的 `vk_canattack_key` 用了 `v >= 0` 所以正常。**修复**：四个快捷键统一改为 `v >= 0`；同时将 UI 提示从“Esc 取消”改为“Esc 清除”（行为本就是清除）。**教训**：范围校验要与允许值域一致，支持“空值”的字段必须显式包含 0。

### 7.12 版本提示弹窗“双 v”（版本检查功能）

服务器返回的版本号带 `v` 前缀（`"v2.6"`），弹窗拼接代码是 `L"发现新版本 v" + 版本号` → 显示 `vv2.6`。**修复**：`versionutil.h` 新增 `NormalizeVersionDisplay`（去首尾空白 + 去 `v`/`V` 前缀），比较用原串、显示用规范化串；单测覆盖。**教训**：服务器字段值不可假设格式受控——显示到 UI 前的字符串一律先规范化；比较逻辑与显示逻辑应解耦（比较用原串保证严格性，显示用规范化串保证美观）。

### 7.13 点击半途停连/切模式导致目标窗口“卡按键”

连点状态机 DOWN/UP 交替发送；若最后一帧是 DOWN（`CS_WAIT_UP`）时停连（`isstart=false`、关左右键、定时停止）或切多倍模式，代码直接 `leftSt=CS_IDLE` **不发 UP**——目标窗口永远收不到抬起，保持模式下没有物理 UP 兜底，游戏里鼠标键永久卡住。此前只有 can-attack 门控单独处理了补发 UP。**修复**：抽出 `releaseLeft/releaseRight` 辅助函数（CS_WAIT_UP 时补发 UP 再复位），**所有**离开 WAIT_UP 的路径统一调用（门控关闭、isMultiActive、停连、关按钮）。**教训**：状态机里任何"丢弃状态"的出口都要检查是否有未完成的外部副作用（已发送的 DOWN 需要配对的 UP）。

### 7.14 配置文件跨线程竞写

`SaveConfig()` 被 UI 线程（控件点击）与连点线程（热键切换、定时停止）同时调用：`ofstream` trunc+写 23 行，两个线程同时写会把 `autoclickerSave.txt` 写花（交错/截断）。**修复**：`LoadConfig/SaveConfig` 加全局 `std::mutex`。**教训**：多线程共享的持久化写入必须串行化；这种竞态平时不触发、一触发配置全丢，比崩溃更难排查。

### 7.15 零时长合成点击 + 连点器点自己

多倍/滚轮点击原来是 `PostMsgA(DOWN); PostMsgA(UP);` 背靠背——点击持续时间为 0ms，部分游戏/反作弊会丢弃；且主连点线程不检查目标窗口类，用户点 UI 时连点会把合成点击打进自己的窗口。**修复**：hook 路径 DOWN 后 `Sleep(kClickHoldMs=10)` 再 UP（模拟主连点引擎约半个周期的按住时长），`PostMessageA` 动态解析收敛为 hook 启动时解析一次的共享指针；主连点线程加 `IsOwnWindow`（窗口类 `ACgdi`）检查，自身为前台时暂停连点（与 hook 已有的排除逻辑一致）。**教训**：合成输入的三要素——时长（非零按住）、目标（排除自身/错误窗口）、发送路径（动态解析防 IME 钩子）——缺一不可。

### 7.16 UpdateLayeredWindow 会把窗口「缩小一个边框」（玻璃态改造）

带边框的 `WS_EX_LAYERED` 窗口，若给 `UpdateLayeredWindow` 传 `psize = 客户区尺寸`，Windows 会把**整个窗口（含边框）**重设成该尺寸——第一次呈现窗口就小一圈，随后 WM_SIZE 换更小的层，再呈现再缩小，无限收缩；若传错 pptDst（窗口左上角而非客户区屏幕坐标）内容还会错位。**修复**：呈现层做成「窗口外尺寸」，客户区内容按 `ClientToScreen(0,0) − GetWindowRect().左上` 的偏移拷入，`pptDst = 窗口左上角`、`psize = 窗口当前外尺寸`（恒等于窗口尺寸 → 永不触发缩放）。**教训**：带边框窗口的 ULW 语义是「整窗分层位图」，不是「客户区分层位图」。

### 7.17 BGRA 字节序写反：蓝色强调色变成橙色

软件光栅化直写 DIB 像素时，把 R 值写进了 BGRA 的 B 槽（`p[0]=sr` 应为 `p[0]=sb`）。灰度色完全无感（R≈G≈B），浅色主题看起来「还行」；一旦出现饱和色就露馅——蓝色 accent 变橙色、深空蓝底变暖棕。**修复**：`BlendPx` 参数语义为 (R,G,B)，内部按 BGRA 落位（`p[0]=B, p[1]=G, p[2]=R`）。**教训**：直写像素缓冲必须单测验证通道序；用纯灰测试图是测不出来的，要拿饱和色（红/蓝）验证。

### 7.18 分层窗口带系统边框 = 经典标题栏 + 加粗边框

`WS_EX_LAYERED` 会禁用 DWM 的非客户区渲染：带 `WS_OVERLAPPEDWINDOW` 的窗口回退为经典（basic）标题栏样式，边框也变成加粗的经典缩放边框（用户反馈「标题栏出问题了、外框大了一圈」）。**修复**：整窗改为无边框（`WS_POPUP | WS_THICKFRAME`），标题栏/窗控自绘（最小化/最大化/关闭），拖动/缩放/双击最大化由 `WM_NCHITTEST`/`WM_NCLBUTTONDBLCLK` 提供。**教训**：做分层玻璃窗要么全无边框自绘，要么不用分层——两者不可兼得。

### 7.19 最大化关闭后窗口「越来越大」

窗口在最大化状态关闭时若直接存 `GetWindowRect`，下次启动会把最大化矩形（含 -7px 过扫描）当作普通尺寸恢复——窗口一次比一次大。**修复**：`WM_DESTROY` 用 `GetWindowPlacement` 存 `rcNormalPosition`（普通状态矩形，与当前是否最大化/最小化无关）。**教训**：持久化窗口几何必须存「普通态」，用 WINDOWPLACEMENT 而不是 GetWindowRect。

---

## 8. 性能数据

| 场景 | 实测 |
|---|---|
| 空闲 CPU（UI + 连点线程） | 0.00%（单核） |
| 点击间隔精度（内部测量） | std = 0.073ms |
| 平均 CPS 误差 | < 0.1%（19.985 vs 20.00） |
| 点击消息率 | 最高 500+ 消息/秒（PostMessageA 异步，无阻塞） |
| 热键响应 | ≤ 4ms（扫描周期） |

---

## 9. 构建与测试

```powershell
# 命令行构建
msbuild AutoClicker.sln /p:Configuration=Release /p:Platform=x64          # Net 网络版
msbuild AutoClicker.sln /p:Configuration=Debug   /p:Platform=x64
msbuild AutoClicker.sln /p:Configuration=Release-Base /p:Platform=x64     # Base 基础版（无网络模块）
```

- 工具链：VS2022+（项目当前用 v145 工具集，MSVC 14.51）
- 无第三方依赖，`#pragma comment(lib, ...)` 显式链接 winmm / dwmapi

### 验证方法（自动化截图 + 像素采样）

1. **UI 验证**：`PrintWindow`/`CopyFromScreen` 截图 + 关键坐标像素采样（背景色、accent 内凹、圆角圆弧方程逐点校验）
2. **连点精度**：C# 目标窗口 + `GetMessageTime()` 统计消息入队时刻；clicker 内部 `steady_clock` 直接测量间隔均值/标准差
3. **CPU**：`TotalProcessorTime` 差值采样
4. **输入法钩子对抗**：`tasklist /v` 验证窗口标题完整；进程模块列表检查注入

---

## 10. 功能速查

| 功能 | 入口 | 说明 |
|---|---|---|
| 左/右键连点 | 连点页 | 滑块 + 6/10/15/20 预设，保持模式免按 |
| 多倍点击 | 多倍页 | 倍数 1-5、延迟 1-200ms、+/− 微调 |
| 滚轮转点击 | 滚轮页 | 左/右选择 + 两个快捷键 |
| 仅能攻击时连点 | 连点页 | 仅左键在可攻击时连点，含状态芯片 + 快捷键 |
| 仅手持放置物时右键连点 | 连点页 | 仅右键在手握放置物时连点，含状态芯片 + 快捷键 |
| 光标检测门控 | 标题栏 | 光标图标开关：仅光标不可见（游戏视角）时连点，背包/聊天/菜单自动暂停 |
| 随机 CPS | 高级页 | ±N CPS 抖动，模拟真人 |
| 拟人化节奏 | 高级页 | 均匀/双击连招/呼吸波动/疲劳递减 + 强度 1-5 |
| CPS 上限 | 高级页 | 20-500，手动输入 |
| 定时停止 | 高级页 | 1-3600 秒 |
| 实时 CPS 面板 | 面板页 | 12s CPS 曲线 + 实时值 + 累计点击 + 会话时长 |
| 游戏状态 | 面板页 | 连接/游戏内/目标实体/命中类型（共享内存 HUD 快照） |
| 配置方案 | 标题栏芯片 | 4 套完整预设一键切换 + 面板页重命名 + 可绑切换热键 |
| 强调色 | 面板页 | 蓝/紫/绿/橙 4 色，全局即时生效 |
| 窗口位置记忆 | 自动 | 关闭时保存位置/尺寸，启动恢复 |
| 版本检查 | 启动时自动 | 有新版弹窗提示（版本号+更新内容），已是最新静默 |
| HWID 上报 | 启动时自动 | 写死服务器域名+端口，WinHTTP GET，5s 超时，结果写 report.log |
| 实时 CPS | 状态栏右下 | 1 秒滑动窗口 + 迷你曲线 |
| 窗口置顶 / 主题 | 标题栏 | 图钉 / ☀☾，hover 有提示 |
| 提示音开关 | 标题栏 | 喇叭按钮一键静音全部提示音（`sound.cpp` 统一门控）；开关自身确认音不受限，关闭时也响最后一次 |

## 11. 仅能攻击时连点 / 仅手持放置物时右键连点（MCCombatStatusJni 联动）

> DLL 由上游项目 [MCCombatStatus-JNI](https://github.com/BigBroadBean/MCCombatStatus-JNI)（V63+，原名 MCCanAttack-JNI）提供。V64 起映射表由 `tools/gen_maps.py` 从 `mappings-extracted` 自动生成 **171 张表**（54 版本 × 4 命名空间：vanilla 混淆名 / forge MCP+SRG→Mojang+stable / mojang 全 Mojang / intermediary Fabric），并按 classpath 版本号自动定位版本。真机验证：原版 1.14、Forge 1.16.5、Fabric 1.16.5/1.21.11、NeoForge 1.20.4/1.21/1.21.11。
>
> **V65 起架构变更（规避网易版反检测）**：DLL 不再创建采集线程、不再调用 `AttachCurrentThread`（外来原生线程附加 JVM 会触发 ThreadStart 事件被游戏侧保护检测）。改为钩住 `gdi32!SwapBuffers`（LWJGL2/GLFW WGL 渲染路径的汇合点），在游戏自己的 Client thread 内用 `GetEnv()` 复用其已有 JNIEnv，解析/采样/上报全部帧驱动完成（每帧预算 8ms、采样 5ms 节流、跨帧引用全局化）。**共享内存布局与 UDP 协议完全不变，本程序零改动**；状态刷新频率从固定 5ms 变为渲染帧率（60fps≈16.6ms），300ms 陈旧阈值兼容。**网易中国版 1.20.1 Forge 真机已验证**（注入后游戏存活、target=SnowGolem/canAttack=1 正确、60s 观察无强杀；网易窗口类同为 GLFW30，本程序的注入识别可自动命中）。

### 协议（已通过反汇编确认）

- `MCCombatStatusJni.dll`（MinGW x64 JNI）被注入后通过 `JNI_GetCreatedJavaVMs` 附加 JVM，反射查找 `Minecraft.hitResult`（新旧映射名兼容，`func_71410_x`/`m_91087_` 等），同时解析手持物品链（旧版 1.8.8~1.12.2 的 `ItemBlock`、新版 1.13+ 的 `BlockItem`，可选解析——失败仅 canPlace 恒 0，不拖垮 canAttack）
- 判定结果写入共享内存 `Local\MCCombatStatus_<游戏PID>`，循环 `Sleep(5)` 后 `sendto` 到 `127.0.0.1:35785`，载荷 **2 字节**：
  - `byte0 = '0'`(0x30) 不可攻击 / `'1'`(0x31) 可攻击（与旧版 1 字节协议完全一致）
  - `byte1 = '0'`(0x30) 手持非放置物 / `'1'`(0x31) 手持放置物（V57+ 新增，兼容旧接收端）

### 本程序实现

- **状态通道（双通道）**：
  - **共享内存轮询（主通道，常驻）**：轮询线程每 5ms 读 `Local\MCCombatStatus_<游戏PID>` 映射视图，校验 `magic=='MCST' && version==7` 后按偏移读 `inGame@16`/`canAttack@20`/`canPlace@24`/`hitType@32`/`targetName@52`，`tick@632` 每轮必须递增（worker 未就绪/卡死时不喂状态，由 300ms 陈旧回落兜底）；1s 重扫进程表发现新发布者/清理死进程。**全生命周期常驻**（无论门控开关）：canAttack/canPlace 供门控用，inGame/hitType/targetName 供面板页与状态栏 HUD 快照（发布者消失时清空快照）。**无端口冲突、无 socket 开销**
  - **UDP 监听（旧版 DLL 兜底）**：绑定 `127.0.0.1:35785`（仅回环），`SO_RCVTIMEO=25ms`，每轮 `Sleep(5)` 后 `recvfrom`；校验源地址为回环才处理——`n>=1` 且 byte0 为 0/1 更新 `g_canAttack`，`n>=2` 时再按 byte1 更新 `g_canPlace`（兼容旧版 1 字节包：byte0 仍生效，canPlace 保持原值）
  - **生命周期门控（随用随上）**：两门控都关闭时轮询线程 unmap 全部映射、UDP 释放端口、注入线程停止扫描，三者 park 在共享唤醒事件上（零唤醒零 CPU）；`NotifyGateToggled()` 按消费者数量脉冲自动重置事件，任一开关一切换三个线程即刻苏醒
  - **fail-safe**：无新数据 300ms 后两个状态一起回落到 0（宁可不点不可误点）
- **门控**：`clicker.cpp` 中三个独立门 `canAtkGate = !canAttackOnlyClick || g_canAttack == 1`、`canPlaceGate = !placeOnlyRightClick || g_canPlace == 1`、`cursorGate = !cursorOnlyClick || !IsCursorShowing()`（`GetCursorInfo` 的 `CURSOR_SHOWING`：MC 游戏视角光标隐藏、背包/聊天/菜单可见）；`leftActive/rightActive` 叠加三门控（可同时开启）；**所有**离开 CS_WAIT_UP 的路径（门控关闭、停连、关左右键、开多倍模式、定时停止）统一走 `releaseLeft/releaseRight` 补发 UP（防止目标窗口卡按键，见 §7.13）
- **注入线程**（反注入要点，两个功能共用同一个注入线程；**任一开关开启即触发注入**，两个都关闭时不注入）：
  - 先 `SeDebugPrivilege`（应对游戏以管理员运行）
  - 只认 `javaw.exe`/`java.exe` + 拥有 `GLFW30`/`LWJGL` 窗口（识别 MC 客户端而非任意 Java 程序）
  - `IsWow64Process` 排除 32 位进程（DLL 是 x64）
  - `TH32CS_SNAPMODULE` 检查 DLL 是否已加载 → 绝不重复注入；已注入的 PID 入集合，进程退出后从集合清除（游戏重启会自动重新注入）
  - `CreateRemoteThread(LoadLibraryA)` 等待 3s，超时后复查模块列表再判定成败
  - 注入失败 1s→30s 指数退避（等待可被门控切换中断），跳过/失败日志按 PID 节流（首次 + 每 16 次尝试），日志不再无限膨胀
- **UI**：连点页第三行 = 仅能攻击时连点开关 + 状态芯片（可攻击/不可攻击/未连接）+ 快捷键；第四行 = 仅手持放置物时右键连点开关 + 状态芯片（手持放置物/非放置物/未连接）+ 快捷键；状态栏第 4/5 个指示灯（绿=1，红=0，灰=无数据）；配置追加 4 行（末尾追加，向后兼容）

### 已知限制

- 多个游戏实例同时上报时取最后到达的包（正常只玩一个）
- 游戏以管理员运行时注入会被拒，需以管理员运行本程序
- 端口 35785 被其他程序占用时**不影响**共享内存主通道（仅旧版 DLL 的 UDP 兜底失效）
- 共享内存 layout 版本号（当前 7）变化时轮询器拒读该映射，自动回落 UDP（旧 DLL 场景）
- 放置物判定链在某环境下解析失败时 `canPlace` 恒为 0（不影响 canAttack）
- 旧版 1 字节 UDP 发送端（旧 DLL）下 canPlace 无数据：仅「仅能攻击时连点」可用，「仅手持放置物时右键连点」按 fail-safe 暂停右键

## 12. HWID 使用调查上报

### 目的

统计本程序的使用机器数（唯一设备数 / 使用次数），服务器端按 HWID 去重计数。

### 实现

- **HWID 生成**（`report.cpp` `GetHwid()`）：优先读 `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid`（每台机器唯一、重启不变），去掉横线/花括号、转大写 hex，格式 `HW-` + 32 位 hex；注册表不可读时兜底用系统盘卷序列号（`HW-%08X`），再不行 `HW-UNKNOWN`
- **服务器地址**：`servercfg.h` 顶部常量 `kServerHost` / `kServerPort` / `kServerPath` 写死（域名+端口模式，**无配置文件**）；当前线上为 `http://counter.bigbroadbean.top:3000/report`（report 与 update 共用，改一处两边生效）
- **发送**：WinHTTP（`#pragma comment(lib, "winhttp.lib")`，系统自带、无第三方依赖）；`WINHTTP_ACCESS_TYPE_NO_PROXY` 直连（实测公网服务器连接 70ms / 总耗时 130ms，5s 超时充足；若部署环境需走系统代理，改为 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`）；`WinHttpSetTimeouts` 给解析/连接/发送/接收全阶段 5s 硬上限——服务器不可用时线程最多挂 5s
- **线程**：`StartHwidReporter()` 在 `WinMain` 中启动 detached 线程，fire-and-forget，与 UI / 连点 / 注入线程完全隔离；失败只写日志 `%APPDATA%\AutoClicker\report.log`（追加式，与 inject.log 同目录），绝不弹窗、不阻塞
- **URL 编码**：HWID 只含 hex 与 `-`（均为 URL 安全字符），仍做了防御性 percent-encoding（`UrlEncode`）

### 已知限制

- 仅启动时上报一次，会话内不重复；无重试（失败下次启动再报）
- 单向采集：客户端不读取响应体，只确认 HTTP 状态码（服务器返回非 200 也只记日志）
- 换系统重装后 MachineGuid 变化，会记为新用户（可接受）

## 13. 启动时版本检查

### 目的

启动时与服务器最新版本对比，有新版本时弹窗提示（最新版本号 + 更新内容），已是最新则静默。

### 流程

```
启动 → 后台线程 → GET /version/latest → 解析 "version"
  ├─ 服务器版本 > 本地版本 → GET /content/latest → MessageBox 弹窗（当前/最新版本 + 更新内容）
  └─ 否则 / 失败 / 未设置 → 静默（写 update.log）
```

### 实现要点

- **版本号单一来源**：`types.h` 的 `APP_VERSION` / `APP_VERSION_W`（Net / Base 双产品线**共用同一版本号 v2.9**，无宏区分）；`main.cpp` 标题栏显示、`update.cpp` 的 `kLocalVersion`（版本比较）、`httputil.cpp` 的 User-Agent 全部引用它——**发新版只改 types.h 一处**
- **接口**：`GET /version/latest` → `{"code":0,"data":{"version":"2.6.0",...}}`；`GET /content/latest` → `{"code":0,"data":{"update_content":"...",...}}`；服务器未设置时 `data:null`
- **JSON 解析**：`GetJsonString` 极简提取（无第三方库），处理 `\n \r \t \" \\` 转义；`\uXXXX` 不处理（Node JSON.stringify 直接输出原始 UTF-8，服务器内容受控）
- **版本比较** `CompareVersions`（versionutil.h）：动态解析，只比较数字段——忽略 `v` 前缀、`.`/`-` 分隔符，各段按数值比较（非字典序），段数不同时缺段按 0 处理（"2.5" < "2.5.1" < "2.10"）；已解析过数字段后遇到字母视为后缀（beta/rc 等）开始，忽略其后全部内容（"v2.5" == "2.5"，"2.5.0-rc1" == "2.5"，即预发布不高于正式版）。踩坑：段相等时不能以"当前字符非数字"判结束（分隔符 `.` 也是非数字，导致 "2.5" 与 "2.6.0" 被误判相等），必须两边都到字符串末尾才算相等
- **弹窗显示规范化** `NormalizeVersionDisplay`（versionutil.h）：比较用原串，显示前去掉首尾空白与 `v`/`V` 前缀——服务器返回 `"v2.6"` 时若直接拼 `"发现新版本 v" + 版本号` 会显示 `vv2.6`（踩坑，见 §7.12）
- **单元测试**：仓库根目录 `test_version.cpp`（不进 vcxproj），30 个用例覆盖 v 前缀/位数不同/数值比较/后缀/边界/显示规范化；编译运行：`cl /std:c++20 /EHsc /utf-8 /I AutoClicker test_version.cpp`
- **弹窗**：后台线程直接 `MessageBoxW`（MB_OK | MB_ICONINFORMATION | MB_TOPMOST），标题「AutoClicker 发现新版本」，内容含当前版本/最新版本/更新内容；不阻塞 UI 线程
- **共用**：域名+端口常量在 `servercfg.h`（与 HWID 上报共用，改一处两边生效）；GET 工具在 `httputil.cpp`

### 已知限制

- 仅提示，无强制更新
- 服务器未设置版本号视为"无更新"
- 服务器不可用时静默跳过（下次启动再查），日志在 `%APPDATA%\AutoClicker\update.log`

## 14. 未来改进方向

- 多显示器坐标支持（当前点击位置取光标，天然支持多屏）
- 点击位置锁定（固定坐标点击）
- 配置迁移到 JSON（当前纯文本行格式简单但脆弱）
- 用 DirectWrite 替换 GDI 文本（ClearType 在部分缩放比下更优）
- 攻击冷却感知连点（DLL 读 `getAttackStrengthScale`/`attackStrengthTicker`，满蓄力才出手）
- 目标实体白名单/黑名单（DLL 的 `targetName` 已上报，只差消费端筛选）

> ✅ 已完成（V64）：映射自动生成——`gen_maps.py` 从 mappings-extracted（54 版本）
> 生成 171 张 JniMap，DLL 覆盖 1.8.8 → 1.21.11 几乎全版本，见上游项目文档。
