# jdumper 已实现能力

> JVM 运行时字节码导出工具。

| 功能 | 说明 |
|---|---|
| P0 JVMTI agent dump | `-agentpath:jdserver.dll` 启动，导出全部类文件 |
| P0.5 启动参数隐藏 | 字节码补丁 `VMManagementImpl#getVmArguments()`，隐藏自身 agent 参数 |
| P1 远程注入 | `jdumper inject -p <pid>`，CreateRemoteThread + LoadLibraryW |
| P1 dump 前预览 | `jdumper inspect -p <pid>` / `classes`，枚举 loaded classes、module、loader identity/parent 并用 all/matched/skipped 视图预览过滤命中，不写 `.class` |
| P2 只读 inventory | `jdumper inspect -p <pid> --external`，不注入 DLL，通过 HotSpot Klass walk 写出与 P1 inspect 对齐的 `_inspect.json` |
| P1 默认 dump | `jdumper dump -p <pid>`，注入 agent + 等待 session 产物 |
| JVM 进程列表 | `jdumper list`，列出可见 java.exe/javaw.exe 进程 |
| P2 外部读取预检 | `jdumper dump -p <pid> --preflight`，定位 jvm.dll + 探测 VMStructs |
| P2 外部读取 | JDK 8 `--external --cache-bait` 可写出匹配 class；JDK 17/21 输出稳定 negative gate |
| dump 输出打包 | `jdumper pack -i <dump-dir> -o <out.jar>`，将 `.class` 树打包为 JAR |
| dump 输出索引 | `_index.json` v2 记录 dumped/skipped class、filter hits、source path、class magic、路径安全、postValidation 和 SHA-256/hash |
| dump 分组导出 | `--group-by package|module` 输出到 `_by_package/` 或 `_by_module/<module>/`，inspect 会同步预估路径 |
| 下游工具协同 | dump 输出目录保持 class 文件树布局，可被任意 class 文件工具直接读取；`jdumper pack` 结果是标准 JAR |
| 过滤 | include/exclude glob、正则、module/loader 精确值、排除 JDK 类 |
| 过滤诊断 | CLI 在 inject/dump/inspect 前报告 invalid regex 并 fail closed |
| 输出模式 | 默认不写注入 log 且不展开 agent options；`--quiet` 收敛成功输出；`--diagnostic` 显式写 `%TEMP%\jdumper-<pid>.log` 并打印原始 options |
| session 状态 | `_meta.json` 记录过滤条件、写入计数、retransform 统计 |
| session registry / 二次 sweep | `jdumper sessions` 列出本地记录 session；`jdumper sweep -o <dir> [filters]` 复读 metadata 后用同一输出目录执行增量 dump |
| 注入低残留控制 | 初次注入和二次 sweep/inspect 都通过远程内存传递 options，不创建 `%TEMP%\jdumper-<pid>.opts`；旧版 opts 文件只作为 agent 兼容读取入口 |
| late-complete 处理 | agent running 后 CLI timeout 写 `_cli_error.json`，不覆盖 `_meta.json`；`status` 同时展示最终 session 状态和 CLI errorCode |
| ClassFileLoadHook 热路径 | 过滤器（含正则预编译）与 FileClassSink 进程级单例；未配置 loader/module 过滤时跳过 JNI 反射链；`_meta.json` 类写入按 256 类/2s 节流并以 tmp+rename 原子替换（终态立即落盘） |

## 已知限制（文档化声明）

- 首载类 module 恒 `unknown`：`ClassFileLoadHook` 首载回调拿不到`class_being_redefined`，`--group-by module` 下进 `_by_module/unknown/`，`--module` 精确过滤对首载类恒不命中（fail-closed）。见 readme/doc。
- `collect` 命令是死路径：agent 无管道生产者（writeClass 直写文件），仅历史占位。
- `hideAgentArgs` 对 vmArgs 已填充的 JVM 结构性无效（补丁保留缓存字段快路径，live-attach 前已调过 `getInputArguments()` 则永久返回未过滤列表）；已读出数据不可撤回。边界详见 `dserver/arg_hider.hpp` 与 user_guide。

## 能力索引

| 能力域 | 当前权威来源 |
|---|---|
| JVMTI agent dump | `dserver/` |
| 远程注入 | `dump/` |
| inspect / classes 预览 | `dump/cmd_inspect.cpp` + `dserver/inspect.cpp` |
| P2 外部读取 | `dump/` + libjninfra |
| 启动参数隐藏 | `dserver/` |
| session metadata / filtering | `dump/`、`doc/`（design.rst 过滤一节） |
| session registry / sweep | `dump/cmd_sessions.cpp`、`dump/cmd_sweep.cpp`、`dump/session_registry.cpp` |
| pack | `dump/cmd_pack.cpp` |

## Validation maturity

- 构建/单测：`xmake f --with_tests=y` + `xmake build` + `xmake run jdumper-unittest`（见 doc/user_guide.rst 构建）。
- live 行为验证（P0/P1/inspect JVM smoke、真实应用样本、P2 external matrix）需要 JDK 8/17/21 真实环境，按 `doc/design.rst` MVP 场景清单逐项执行；本仓不附带依赖宿主目录布局的 smoke 脚本。
- 尚未产品级：失败恢复体验、daemon/IPC 型长生命周期会话仍需后续固化。
