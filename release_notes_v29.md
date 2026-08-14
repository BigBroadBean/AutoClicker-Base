# AutoClicker v2.9

## 网易中国版支持（重大）

上游 DLL 升级至 **MCCombatStatusJni V65**，彻底重构为**帧驱动架构**：

- **不再创建采集线程、不再调用 `AttachCurrentThread`** —— 旧架构"外来原生线程附加 JVM"会触发 JVM 的 ThreadStart 事件，正是网易版反作弊强杀游戏的导火索
- 改为**钩住 `gdi32!SwapBuffers`**（LWJGL2/GLFW WGL 渲染路径的汇合点），在游戏自己的渲染线程内用 `GetEnv()` 复用其已有 JNIEnv，解析/采样/上报全部帧驱动完成（每帧预算 8ms、采样 5ms 节流）
- **网易中国版 1.20.1 Forge 真机验证通过**：注入后游戏存活（60+ 秒观察无强杀）、准星目标识别正确（SnowGolem → canAttack=1）；网易窗口类同为 GLFW30，可被本程序自动注入
- 共享内存（`Local\MCCombatStatus_<PID>`）与 UDP 35785 协议零变化；状态刷新频率变为渲染帧率驱动（60fps≈16.6ms）

## 说明

- 本包为**网络版**：启动时 HWID 使用上报 + 版本检查（详见 README）
- 「仅能攻击时连点」/「仅手持放置物时右键连点」支持 1.8.8 ~ 1.21.11 几乎全版本（原版 / Forge / Fabric / NeoForge）
- 长期使用网易版仍建议自行观察（反作弊可能存在更长周期的扫描窗口）

完整更新历史见仓库内 CHANGELOG.md。
