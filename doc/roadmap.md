# mjdump 路线图

> mjdump 负责从运行中的 JVM 获取 class bytes，并把输出整理成下游字节码
> 分析工具能继续处理的离线材料。

---

## 1. 定位

获取路径、bytes 来源边界和 P2 兼容性知识见 `doc/` 下的技术 Wiki
（`doc/index.rst` 的 toctree）；本文只保留产品方向。

mjdump 解决"字节码从哪里来"的问题。它不负责反混淆，也不负责 debugger
交互；它的核心价值是稳定拿到运行时 class bytes，并记录足够的 session 元数据。

三条技术路径：

- P0：`-agentpath` 启动式 JVMTI dump，可靠但需要改 JVM 启动参数。
- P1：远程注入 agent 后 dump，不要求目标 JVM 预先带参数。
- P2：外部只读进程内存，尽量 zero-injection，但受 HotSpot 版本和 cached
  bytes 限制。

---

## 2. 能力路线图

| 能力 | 目标 | 当前方向 |
|---|---|---|
| P0 agent dump | ClassFileLoadHook + 已加载类 sweep/retransform | 作为可靠基线和验证入口 |
| 参数隐藏 | 隐藏自身 agent 参数，不破坏其他 JVM 参数 | 仅隐藏精准匹配项，保留回退开关 |
| P1 注入 dump | CreateRemoteThread + LoadLibraryW 注入 agent | Windows/JDK 17/21 实用路径优先 |
| P2 preflight | jvm.dll、VMStructs、ReadProcessMemory、版本能力诊断 | 明确 source gate，不假装成功 |
| P2 external dump | JDK 8 cached class-file + cache-bait；JDK 17/21 negative gate | 继续积累 HotSpot 版本矩阵 |
| 过滤系统 | package/class/module/loader/regex/exclude/excludeJdk | 所有模式复用同一 filter 语义 |
| session 元数据 | `_meta.json` 记录模式、过滤、计数、错误码、sweep 统计 | 作为 status、report、debug 输入 |
| 输出整理 | dump 目录、`_index.json`、pack 成 JAR、供任意 class/JAR 工具读取 | 与 mbase JAR 能力对齐 |
| 长生命周期会话 | 多 JVM、多 session 状态管理 | 已提供 no-daemon registry、active list 和二次 sweep 边界 |
| 反检测升级 | 配置无痕、硬件断点 hook、模块隐藏 | 研究项，不进入默认 release gate |
| 跨平台 | Linux/macOS 注入与读取 | 后置，Windows 路径先产品化 |

---

## 3. 优先级

1. 把 JDK 8/17/21 的 P0/P1/P2 行为写成稳定兼容矩阵。
2. 真实应用 dump 验证：不仅是 demo JVM，也要覆盖实际混淆/保护场景。
3. 失败原因稳定化：`source_unavailable`、`no_cached_class_bytes` 等成为
   脚本可判断错误码。
4. 输出与下游工具串联：目录和 pack JAR 都能被后续工具复读。
5. 长生命周期会话先设计，不急着做复杂 daemon。

---

## 4. 不在 mjdump 内做的事

- 不做 bytecode deobf；只获取和整理 class bytes。
- 不做传统 debugger；断点、step、locals 属于独立工具域。
- 不承诺 JDK 17/21 pure external class bytes dump，除非找到稳定 source。

---

## 5. 验证

涉及真实 JVM 的行为按 `doc/design.rst` 的 MVP 场景清单（JVM 覆盖矩阵 +
场景覆盖）在 JDK 8/17/21 上逐项验证；构建与单测入口见 `doc/user_guide.rst`
的构建一节。

---

## 6. 后续兼容性工作

JDK 17+ 在拿到非 `_cached_class_file` 的稳定 class bytes 来源之前，P2 只保
identity 遍历，字节 dump 走 P1。候选方向：

- 定位版本相关的 class-file 镜像或 archived parser payload；
- 增加受控的进程内准备步骤，暴露可读来源；
- 维持 P2 identity-only，直到新来源被证实稳定。

失败恢复体验与 daemon/IPC 型长生命周期会话尚未产品级，后续固化。
