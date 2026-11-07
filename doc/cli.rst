.. _cli:

jdumper CLI 模式
================

``jdumper.exe`` 是三套运行时 dump 方案的操作入口。本篇的目标是让
``jdserver.dll`` agent 通过实用工作流触达，而不是让 P1/P2 停留在空占位。
模式划分的总体表见 :ref:`design-modes`。

.. _cli-mode0:

1. 模式 0：JVMTI Agent 启动（P0）
---------------------------------

最可靠的模式仍然是用以下方式启动目标 JVM：

.. code-block:: text

   java -agentpath:<path-to-jdserver.dll>=out=<dir>,include=<glob> ...

``jdserver.dll`` 注册 JVMTI ``ClassFileLoadHook``，并在 VMInit 经
``retransform_loaded_classes`` 对已加载类做 sweep；class bytes 经
``FileClassSink`` 写出。入口与回调注册的微观契约见 ``jdserver-jvmti-agent``
头注。

P0.5 还会在类加载或 retransform 时补丁
``sun/management/VMManagementImpl#getVmArguments()``。当前实现只过滤
jdserver 自身的 ``-agentpath`` / ``-agentlib`` 条目，因此 Java 侧调用
``ManagementFactory.getRuntimeMXBean().getInputArguments()`` 仍然能看到
``-Xmx``、普通 ``-D`` 属性等无关 VM 选项。调试时可把 agent 选项串里的
``hideAgentArgs`` 设为 ``false`` 关闭该行为。

补丁有两个结构性限制：补丁字节码保留 ``vmArgs`` 缓存字段的快路径，若 JVM
在 agent 生效前已经调用过 ``getInputArguments()``（典型是 live-attach 到
开了 JMX 监控的进程），被补丁的 getter 会一直返回缓存里的未过滤列表；P0
``-agentpath`` 启动在类首次加载时打补丁，不受影响。此外补丁只改写后续调用，
监控端或日志已经读取并缓存的参数无法撤回，live attach 在 attach 到
retransform 完成之间还有一段仍返回未过滤结果的窗口。完整边界见
:ref:`user-guide-hideagentargs`。

.. _cli-filtering-sessions:

2. 共享过滤与会话
-----------------

所有 agent 支撑的实用模式都会写 ``<out>/_meta.json``。该文件记录模式标签、
生命周期状态、输出目录、过滤器、class 写出计数与 loaded-class sweep 计数。
``jdumper status -o <dir>`` 打印人读摘要；``jdumper status -o <dir> --json``
输出原始文件。CLI 发起的 dump/inspect 运行还会追加到用户级 registry
``%LOCALAPPDATA%\jdumper\sessions.jsonl``；``jdumper sessions`` 回放该
registry 并用 ``_meta.json`` / ``_inspect.json`` 刷新每行。

CLI 接受 package、class、子树与 regex 过滤。``--package`` 与 ``--class``
被归一化为斜杠形式的 JVM 类名；``--regex`` 与 ``--exclude-regex`` 转发给
``ClassNameFilter``。

module 身份只能在 retransform 路径上推导（``ClassFileLoadHook`` 收到非
NULL 的 ``class_being_redefined``）。agent 启动后首次加载的类 module 恒为
``unknown``：``--group-by module`` 下它们进入 ``_by_module/unknown/``，
精确 ``--module`` 过滤对它们恒不命中（按 fail-closed 跳过）。只有 VMInit
sweep 触达的已加载类携带真实 module。

.. _cli-mode1:

3. 模式 1：远程注入（P1）
-------------------------

``jdumper inject -p <pid> [--dll <path>] [-o <dir>] [filters]`` 使用
Windows ``CreateRemoteThread + LoadLibraryW`` 路线把 ``jdserver.dll`` 装入
运行中的 JVM。命令在注入前检查目标进程同位数且已加载 ``jvm.dll``。

``LoadLibraryW`` 无法传选项串，因此第一个远程线程只加载 DLL。CLI 随后定位
已加载的 ``jdserver.dll`` 模块，把选项写入远程内存并调用导出的控制入口。
进程内 DLL 用 ``JNI_GetCreatedJavaVMs`` 发现已有 JVM，然后走与
``Agent_OnLoad`` 相同的 JVMTI 初始化路径。默认路线不创建
``%TEMP%\jdumper-<pid>.opts``；读取该文件仅作为 DLL 内的旧版兼容回退保留。

默认注入路径不写 ``%TEMP%\jdumper-<pid>.log``。默认成功输出不展开 agent
选项串全文。``--quiet`` 向 agent 转发 ``silent=true`` 并收敛 CLI 成功输出。
``--diagnostic`` 为该次运行显式启用临时注入日志并在 CLI 状态输出里打印原始
选项串；对发生在控制入口消费远程选项之前的失败，仍可用
``JDUMPER_DIAGNOSTIC_LOG=1`` 兜底。

这使 P1 对同用户 HotSpot 目标实用，同时把后续长生命周期控制通道的工作
隔离开。

``jdumper sweep -o <dir> [filters]`` 是 no-daemon 长会话的边界。它读取
``<dir>/_meta.json``，复用记录的 pid 与输出目录，再次调用同一实用 dump
路径。带新过滤时执行一次进入既有目录的二次 sweep；不带时恢复 metadata 里
的过滤条件。既有 class 文件原地保留，``_index.json`` 在新 sweep 后重建。
CLI 经远程内存向 agent 控制导出传选项，因此初次 dump 与二次 sweep 都不会
产生 ``%TEMP%\jdumper-<pid>.opts``。

.. _cli-mode2:

4. 模式 2：外部读取（P2）
-------------------------

``jdumper dump -p <pid> --preflight`` 是只读的 P2 命令面，完成完整外部
class 字节复制前的安全预检：

- 在目标进程中定位 ``jvm.dll``；
- 校验进程内存可读；
- 解析并清点远程 HotSpot Serviceability Agent 导出表；
- 把这些表拷贝为本地 ``VmStructSnapshot``；
- 遍历 ``ClassLoaderDataGraph`` 或 HotSpot 8 的 ``SystemDictionary`` 回退，
  恢复 Klass 名称并应用常规过滤；
- 报告 cached-class-file 字节来源是否可用；
- 可选 ``--freeze-ms <ms>`` 附加一个短暂的调试器冻结窗口。

``jdumper inspect -p <pid> --external [filters]`` 使用同一条只读 Klass walk，
但写出 ``_inspect.json`` 而不是复制字节。JSON schema 与 agent 侧 inspect
对齐：类行含 ``name``、``matched``、``estimatedPath``、``skipReason``。
由于外部 walk 不进入 JVMTI，module 与 loader 报告为 ``unknown``；精确的
module/loader 过滤因此 fail-closed，而不是被视为命中。

``jdumper dump -p <pid> --external [-o <dir>] [filters]`` 尝试 gated P2
复制路径。它使用同一 graph walk 与过滤，但只有在 libjninfra 的 class 字节
来源门报告 ``InstanceKlass::_cached_class_file`` 与 ``ClassFileData`` 布局
元数据可用时才写字节。目标 JVM 不暴露该布局时，命令以诊断信息失败而不是
猜测偏移。无论成败，命令都会写 ``<out>/_meta.json``，source-gate 失败与
复制统计都可经 ``status`` 检查。

.. _cli-practical-dump:

5. 实用 dump 命令
-----------------

``jdumper dump -p <pid> [-o <dir>] [filters]`` 是可用的 dump 工作流。命令
形状保持简单，默认走经过验证的 P1 注入路线：校验目标 JVM，注入
``jdserver.dll``，等待会话 metadata 或 class 输出。等待循环把 ``completed``
与 ``failed`` 会话状态视为终态，并对无匹配的完成会话给出明确报告。

CLI 等待超时但 agent 已创建 running metadata 时，CLI 不会覆盖
``_meta.json``：本次命令失败记录到 ``<out>/_cli_error.json``，``status``
继续展示 agent 最终状态并附带 ``cliErrorCode`` / ``cliLastError``，避免
late-complete 把失败/成功状态来回覆盖。

在 MVP 范围内，P1 是 JDK 17 与 JDK 21 上受支持的真实 dump 路径；这两个
JDK 上纯 P2 外部字节复制保持 source-gated negative（见
:ref:`design-mvp-coverage`）。

.. _cli-support-commands:

6. 辅助命令
-----------

``jdumper list`` 枚举可见的 ``java.exe`` / ``javaw.exe`` 进程，简化 attach
工作流；同时报告 ``jvm.dll`` 是否已加载、目标位数是否与当前 CLI 构建匹配，
以及进程映像路径，便于避开 launcher/shim PID。

``jdumper collect`` 是保留兼容的遗留 pipe 接收器。agent 侧从不向 named
pipe 发送 class 数据（``writeClass`` 直写文件），所以该命令在当前设计里没有
生产者，在可工作的 dump 流程中不承担角色。它校验记录长度并使用共享的
libjninfra 文件 sink。

``jdumper status -o <dir>`` 汇总已完成或进行中 dump 输出目录的
``_meta.json``。对外部 P2 会话，它还输出 walk 来源、source gate、稳定
``errorCode``、可选的 cache-bait 子会话以及 metadata ``p2`` 对象下的字节
复制计数。

``jdumper sessions [--json] [--active]`` 列出当前用户记录的 dump/inspect
会话。``--active`` 刻意保守：只有 metadata 显示 ``running`` 且记录的 pid
仍存活时才标记为 active。

``jdumper pack -i <dir> -o <out.jar>`` 把 dump 目录打包为 JAR，默认写出后
立即读回校验条目数（``--no-verify`` 跳过）。

.. _cli-live-smoke:

7. live 验证边界
----------------

本仓不附带依赖宿主目录布局的 live smoke 脚本：涉及真实 JVM 的行为验证需要
JDK 8/17/21 环境，按 :ref:`design-mvp-scenarios` 的场景清单逐项人工执行或
由消费方脚本驱动。失败桶的稳定性（:ref:`design-failure-buckets`）是这些
脚本可以依赖的产品契约。
