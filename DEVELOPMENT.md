# AutoClicker 开发文档

本文档记录 AutoClicker 的架构设计、核心机制、性能优化与踩坑经验，供后续开发维护参考。

- 语言：C++20（MSVC），仅依赖 Windows SDK
- 窗口框架：原生 Win32 + GDI 自绘（无 MFC / 无外部 UI 库）
- 构建：Visual Studio 2022+ / MSBuild，`Release | x64`

---

## 1. 模块结构

| 文件 | 职责 |
|---|---|
| `main.cpp` | 主窗口、GDI 自绘 UI（新拟态）、布局、命中测试、输入处理 |
| `clicker.cpp` | 连点核心线程、精确计时、热键检测、多倍点击 Hook、实时 CPS 统计 |
| `canattack.cpp` | 仅能攻击时连点：UDP 监听线程（35785 端口，5ms 循环）、Minecraft Java 进程自动注入线程、反重复注入 |
| `report.cpp` | HWID 使用调查上报：启动时后台 GET 上报（WinHTTP，域名+端口写死，5s 超时，fire-and-forget） |
| `httputil.cpp` | 极简 WinHTTP GET 工具（域名+端口，5s 超时，可选读响应体），report/update 共用 |
| `update.cpp` | 启动时版本检查：GET /version/latest 对比本地版本，有新版弹 MessageBox（含更新内容） |
| `servercfg.h` | 服务器域名+端口常量（写死，无配置文件），report/update 共用 |
| `versionutil.h` | 版本工具（纯函数头文件）：点分数字版本比较 + JSON 字符串提取，update 模块与单元测试共用 |
| `config.cpp` | 配置读写（`%APPDATA%\AutoClicker\autoclickerSave.txt`，追加式、向后兼容） |
| `overlay.cpp` | Toast 通知（无边框分层窗口，逐像素 Alpha + 新拟态样式） |
| `sound.cpp` | 系统提示音（`PlaySoundW`，Windows Media 目录 wav） |
| `ui.cpp` | Win11 窗口样式（暗色标题栏、圆角） |
| `types.h` | 窗口尺寸、主题色板、常量、主题访问器 |
| `clicker.h` | 全局状态变量声明、`cpsToMs`/`msToCps10` 换算 |

### 全局状态

所有功能状态（`isstart`、`isMultiActive`、`isScrollClickActive`、`leftenabled` 等）为进程内全局变量，由 `clicker.h` 声明、`clicker.cpp` 定义。跨线程访问的（`g_clickCount`、`g_debounceUntil`）用 `std::atomic`。

---

## 2. 线程模型

| 线程 | 用途 | 关键点 |
|---|---|---|
| UI 线程 | Win32 消息循环 + 自绘 | `PeekMessage` 循环 + `Sleep(5)`；`WM_TIMER` 16ms 按需重绘 |
| 连点线程 | 热键扫描 + 精确点击计时 | 4ms 热键扫描周期；亚毫秒点击定时 |
| Hook 线程 | `WH_MOUSE_LL` 全局鼠标钩子 | 多倍点击、滚轮转点击；`GetMessageW` 阻塞零开销 |
| Toast 线程 | 一次性通知动画 | detach，自清理 |
| 注入线程 | 扫描 javaw/java 进程，向未注入的 MC 客户端注入 DLL | 1s 周期；功能关闭时完全不注入 |
| UDP 监听线程 | 绑定 127.0.0.1:35785，接收 0/1 可攻击状态 | 5ms 循环，SO_RCVTIMEO 25ms |

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

---

## 4. 新拟态（Neumorphism）绘制系统

### 配色

`types.h` 定义深浅两套色板（背景 = 表面色，靠阴影/高光表达立体）：

| 语义 | 深色 | 浅色 |
|---|---|---|
| BG / 表面 | RGB(43,44,52) | RGB(228,232,240) |
| 暗影 SHADOW_DARK | RGB(27,28,34) | RGB(197,202,212) |
| 高光 SHADOW_LIGHT | RGB(60,62,72) | RGB(255,255,255) |
| 强调色 ACCENT | RGB(92,156,255) | RGB(56,132,255) |

所有颜色通过 `BG()` / `CARD()` / `SHADOW_DARK()` 等内联访问器按主题切换，绘制代码无需分支。

### 核心绘制原语（Region 精确几何）

```
NeuRaised      浮雕（凸起）：同心外扩月牙 ∩ 象限
               - 暗影：外扩 i 且半径 +i（同心圆弧），裁剪到右下象限
               - 高光：同上，裁剪到左上象限
NeuInset       内凹（按下/轨道/输入框）：
               条带 = 按钮 − 同心腐蚀(i)（内缩 i 且半径 −i）
               上半条带 = 暗影，下半条带 = 高光，中间保持基色
NeuInsetAccent 强调内凹（选中态）：同上，accent 底 + 黑/白内阴影
NeuDropShadow  投影：同心外扩月牙（选中侧边栏按钮用）
NeuButton      通用按钮：hover 时三层同心圆角光晕（内浓外淡）+ 表面提亮
```

**为什么必须用 Region 而不是圆角矩形平移**：见 §7 踩坑记录。

### 滑块 / 开关 / 分段选择器

- 滑块：轨道 = `NeuInset`；填充 = accent 圆角条（左端贴轨道起点）；thumb = 圆 + 投影 + accent 环，hover/拖动时外加同心光晕环
- 开关：轨道 `NeuInset` / `NeuInsetAccent`，knob 白色凸起 + 投影 + accent 细环
- 分段选择器（滚轮左/右）：整体 `NeuRaised` → clip 半边 → `NeuInsetAccent` → 统一 1px 轮廓线

### 图标

侧边栏 4 个图标全部 GDI 手绘（鼠标、叠块、滚轮、三滑块），统一约束在按钮内 22×22 区域（`DrawMouseIcon` 等）。选中白色 / hover accent / 默认灰。

---

## 5. UI 布局与交互

### 窗口

- 默认 640×480（经典 4:3 横向），可缩放，最小 480×420
- 启动时自动适配工作区（任务栏安全）：`SPI_GETWORKAREA` 限制尺寸并居中
- DPI：启动时 `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`（Win10 1703+）

### 结构

```
┌ 标题栏：AutoClicker v1.9 | 📌置顶 | ☀/☾主题 ┐
├ 侧边栏（4 图标按钮）：连点 | 多倍 | 滚轮 | 高级
│ 内容区：行1 = 两卡并排，行2 = 全宽卡
│   · 连点页：左键卡 | 右键卡 / 快捷键+保持卡
│   · 多倍页：倍率卡 | 延迟卡 / 快捷键卡
│   · 滚轮页：全宽卡
│   · 高级页：CPS上限卡 | 随机CPS卡 / 定时停止卡
└ 状态栏：连点● | 多倍● | 滚轮● | 实时CPS chip ┘
```

### 响应式缩放

`g_lyScale = f` 由可用高度与基准行高（165/135）之比计算，clamp 到 [0.72, 1.15]，超出部分均匀分配到卡间距。所有卡内控件偏移统一经 `S(v) = (int)(v * f)` 缩放（**全局函数**，Layout 与 Paint 共用）。标签区 `S(8)..S(26)`，控件从 `S(32)` 起，保证任何缩放比下不重叠。

### 交互

- 命中测试：`g_hr[E_*]` 按当前页注册，页面切换时重建
- 滑块拖动：`WM_MOUSEMOVE` 只置 `g_dirty`，由 `WM_TIMER` 16ms 统一重绘（节流，避免每事件全量重绘）
- 键盘：方向键切换页面；输入框聚焦时方向键保留给文本
- 改键捕获：`CaptureKey` 循环等待物理按键（GetAsyncKeyState），Esc 或 **15 秒超时**自动取消（防止误触卡死 UI）

### 按需重绘

`WM_TIMER`（16ms）只在实际变化时重绘（`g_dirty`）：
- 状态位快照（isstart / 页面 / 输入框焦点等）变化
- `g_clickCount` 或实时 CPS 变化
- 拖动中强制重绘

空闲时零重绘，CPU ≈ 0%。

---

## 6. 配置系统

- 路径：`%APPDATA%\AutoClicker\autoclickerSave.txt`（`CSIDL_APPDATA`）
- 格式：每行一个值，**顺序敏感**；新字段**追加在末尾** → 旧配置文件天然兼容
- 当前字段顺序（19 行）：cpsLeft10, cpsRight10, cpsMax, randomCpsEnabled, randomCpsRange, vk_key, leftenabled, rightenabled, keepClicke, vk_multi_key, multiMul, multiDelayMs, vk_scroll_key, scrollClickButton, vk_scroll_lr_key, theme, autoStopEnabled, autoStopSeconds, topmost
- 载入时对每个值做范围校验（防手改损坏）

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

点击"快捷键"按钮后 `CaptureKey` 阻塞等键，误触即卡死。**修复**：Esc 取消 + 15 秒超时。

### 7.9 计数统计调试的经典自坑

写内部统计时 `if (s_cnt == 0) s_last = ns; else { ... s_cnt++; }` —— s_cnt 永为 0，else 永不执行。**教训**：初始化标志与计数变量要分离（`s_cnt = -1` 表示未初始化）。

### 7.10 点击页值文本矩形左右反转（v2.4）

点击页左右卡片共用循环绘制 CPS/毫秒文本，`RECT r` 的右边界错误地固定取 `L.track[SL_L].right`：`i=1`（右键卡片）时 `r.left = card[1].left + 20 > r.right = track[SL_L].right`，矩形反转导致 GDI 绘制错乱——右键的 CPS 文本消失/画到左卡片毫秒区域，与左键“25 毫秒”重叠（用户表现为“仅显示 20”）。多倍页因分别用 `SL_MUL`/`SL_DEL` 的 track 而未受影响。**修复**：改为 `L.track[SL_L + i].right`（`SL_L=0, SL_R=1`）。**教训**：两列对称布局中右列的所有坐标都必须由右列自己的布局值推导，不能复用左列。

### 7.11 快捷键空值（0）被配置加载过滤（v2.4）

按 Esc 清除快捷键后 `CaptureKey` 写 `vk=0` 并保存，但 `LoadConfig` 的读取条件是 `v >= 1 && v <= 255`，0 被拒绝加载，变量保持默认值——下次启动快捷键“复活”。后加的 `vk_canattack_key` 用了 `v >= 0` 所以正常。**修复**：四个快捷键统一改为 `v >= 0`；同时将 UI 提示从“Esc 取消”改为“Esc 清除”（行为本就是清除）。**教训**：范围校验要与允许值域一致，支持“空值”的字段必须显式包含 0。

### 7.12 版本提示弹窗“双 v”（v2.5 版本检查）

服务器返回的版本号带 `v` 前缀（`"v2.6"`），弹窗拼接代码是 `L"发现新版本 v" + 版本号` → 显示 `vv2.6`。**修复**：`versionutil.h` 新增 `NormalizeVersionDisplay`（去首尾空白 + 去 `v`/`V` 前缀），比较用原串、显示用规范化串；单测覆盖。**教训**：服务器字段值不可假设格式受控——显示到 UI 前的字符串一律先规范化；比较逻辑与显示逻辑应解耦（比较用原串保证严格性，显示用规范化串保证美观）。

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
msbuild AutoClicker.sln /p:Configuration=Release /p:Platform=x64
msbuild AutoClicker.sln /p:Configuration=Debug   /p:Platform=x64
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
| 随机 CPS | 高级页 | ±N CPS 抖动，模拟真人 |
| CPS 上限 | 高级页 | 20-500，手动输入 |
| 定时停止 | 高级页 | 1-3600 秒 |
| 版本检查 | 启动时自动 | 有新版弹窗提示（版本号+更新内容），已是最新静默 |
| HWID 上报 | 启动时自动 | 写死服务器域名+端口，WinHTTP GET，5s 超时，结果写 report.log |
| 实时 CPS | 状态栏右下 | 1 秒滑动窗口 |
| 窗口置顶 / 主题 | 标题栏 | 图钉 / ☀☾，hover 有提示 |

## 11. 仅能攻击时连点（MCCanAttackJni 联动）

### 协议（已通过反汇编确认）

- `MCCanAttackJni.dll`（MinGW x64 JNI）被注入后通过 `JNI_GetCreatedJavaVMs` 附加 JVM，反射查找 `Minecraft.hitResult`（新旧映射名兼容，`func_71410_x`/`m_91087_` 等）
- 判定结果写入状态结构 offset 0x14，循环 `Sleep(5)` 后 `sendto` 到 `127.0.0.1:35785`，载荷 1 字节：`'0'`(0x30) 不可攻击 / `'1'`(0x31) 可攻击
- 另有共享内存 `Local\MCCanAttackStatus_<游戏PID>` 备用通道（未使用）

### 本程序实现

- **UDP 监听**：`canattack.cpp` 绑定 `127.0.0.1:35785`（仅回环），`SO_RCVTIMEO=25ms`，每轮 `Sleep(5)` 后 `recvfrom`；校验源地址为回环、载荷为 0/1 才更新 `g_canAttack`；无新包 300ms 后回落到 0（fail-safe，宁可不点不可误点）
- **门控**：`clicker.cpp` 中 `canAtkGate = !canAttackOnlyClick || g_canAttack == 1`，左右键 `leftActive/rightActive` 加门控；门控在点击半途关闭时立即补发 UP（防止目标窗口卡按键）
- **注入线程**（反注入要点）：
  - 先 `SeDebugPrivilege`（应对游戏以管理员运行）
  - 只认 `javaw.exe`/`java.exe` + 拥有 `GLFW30`/`LWJGL` 窗口（识别 MC 客户端而非任意 Java 程序）
  - `IsWow64Process` 排除 32 位进程（DLL 是 x64）
  - `TH32CS_SNAPMODULE` 检查 DLL 是否已加载 → 绝不重复注入；已注入的 PID 入集合，进程退出后从集合清除（游戏重启会自动重新注入）
  - `CreateRemoteThread(LoadLibraryA)` 等待 3s，超时后复查模块列表再判定成败
- **UI**：连点页第三行 = 开关按钮 + 实时状态芯片（可攻击/不可攻击/未连接）+ 快捷键按钮；状态栏第 4 个指示灯（绿=可攻击，红=不可，灰=无数据）；配置追加 2 行（末尾追加，向后兼容）

### 已知限制

- 多个游戏实例同时上报时取最后到达的包（正常只玩一个）
- 游戏以管理员运行时注入会被拒，需以管理员运行本程序
- 端口 35785 被其他程序占用时无法接收（状态显示“未连接”）

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

- **本地版本号** `kLocalVersion`（update.cpp 顶部，与 UI 标题栏一致）；发新版时需同步改三处：`update.cpp`（kLocalVersion）、`httputil.cpp`（User-Agent）、`main.cpp`（标题栏版本显示）
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
- 连点场景配置文件（游戏预设一键切换）
