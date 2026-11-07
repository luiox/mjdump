.. _design:

mjdump 设计文档
================

.. _design-goal:

1. 目标
-------

mjdump 从运行中的 JVM 进程获取运行时 class bytes，并把输出整理成下游字节码
分析工具可以直接消费的离线材料（目录形态或 pack 后的 JAR）。它不是 debugger，
也不做反混淆。

.. _design-modes:

2. 模式
-------

.. list-table::
   :header-rows: 1

   * - 模式
     - 入口
     - 用途
     - 当前状态
   * - P0
     - ``-agentpath:mjserver.dll``
     - 可靠的启动期 JVMTI dump
     - 基线路径
   * - P0.5
     - agent 字节码补丁
     - 在 Java management API 视野里隐藏 mjserver 自身的 VM 参数
     - 已支持，可回退
   * - P1
     - ``mjdump dump -p <pid>``
     - 向运行中的 JVM 注入 ``mjserver.dll`` 并经 JVMTI dump
     - Windows 上的实用路径，选项低残留传递
   * - P1 inspect
     - ``mjdump inspect -p <pid>`` / ``classes``
     - 预览已加载类、过滤命中、loader/module 数据与预计输出路径
     - dump 前先看的路径
   * - P2 preflight
     - ``mjdump dump -p <pid> --preflight``
     - 只读 HotSpot 元数据预检，不写 class
     - 诊断路径
   * - P2 external
     - ``--external``
     - 来源门通过时从进程内存复制 class bytes
     - JDK 8 cache-bait 正向；JDK 17/21 稳定 negative gate

P0/P1 是现代 JDK 上受支持的真实 dump 路径。P2 是刻意的 source-gated 路径：
class bytes 不可用时必须以稳定错误码失败，而不是假装 dump 成功。

.. _design-components:

3. 组件
-------

.. list-table::
   :header-rows: 1

   * - 组件
     - 角色
   * - ``mjdump.exe``
     - CLI：list、inject、dump、inspect/classes、status、pack、sessions、sweep、collect
   * - ``mjserver.dll``
     - 进程内 JVMTI/JNI agent
   * - libjninfra
     - 共享的 JVMTI/HotSpot/dump 原语（独立库，git 直连包）
   * - ``_meta.json``
     - 会话元数据与稳定失败诊断

微观层锚点：选项键语义见 ``mjserver-config`` 头注，会话元数据的字段与状态机
见 ``mjserver-session`` 头注。

.. _design-data-flow:

4. 数据流
---------

P0/P1：

.. code-block:: text

   target JVM
     -> mjserver.dll
     -> JVMTI ClassFileLoadHook / loaded-class sweep
     -> class filter
     -> class tree under output dir
     -> _meta.json

P1 inspect/classes：

.. code-block:: text

   mjdump.exe
     -> validate normalized filters
     -> send inspect options through remote memory
     -> mjserver.dll
     -> JVMTI GetLoadedClasses inventory
     -> _inspect.json and optional JSON stdout

P2：

.. code-block:: text

   mjdump.exe
     -> locate target jvm.dll
     -> copy HotSpot VMStructs snapshot
     -> walk CLDG/SystemDictionary
     -> apply class filter
     -> source gate
     -> write class bytes or stable error metadata

CLI 侧对 ``_meta.json`` 的等待终态与 late-complete 契约见 :ref:`cli-practical-dump`。

.. _design-filtering-output:

5. 过滤与输出
-------------

所有模式共享同一套过滤语义：

- package/class 选择器（``--package`` / ``--class`` 归一化为斜杠形式），
- 子树选择器（``include=com/example/**``），
- regex include/exclude，
- loader/module 精确选择器（fail-closed），
- ``excludeJdk``。

输出规则：

- class 文件按包形目录写出，``--group-by`` 可切换 ``_by_package/`` 与
  ``_by_module/<module>/`` 布局；
- ``_meta.json`` 记录模式、过滤器、计数、错误与 P2 source-gate 数据；
- ``_index.json``（v2）记录已写出与被跳过的类、过滤命中、source path、
  hash 与写后校验计数；dump 完成后自动 post-verify，magic 无效、路径不安全
  或读回失败都以 ``errorCode=post_verify_failed`` 失败，避免误报成功；
- ``_inspect.json`` 记录预览类行、命中状态、跳过原因、loader/module 数据与
  预计输出路径；
- ``mjdump pack`` 把目录转换为 JAR，并默认读回校验条目数；
- 输出目录保持 ``com/example/Foo.class`` 兼容布局，可直接被常规 class 文件
  工具读取。

.. _design-failure-buckets:

6. 稳定失败桶
-------------

P2 与注入路径不允许猜测或静默成功。已知失败桶：

- ``source_unavailable``，
- ``no_cached_class_bytes``，
- 目标进程不存在（``bad_pid``），
- 位数不匹配（``bitness_mismatch``），
- ``jvm.dll`` 未加载（``not_hotspot``），
- 注入失败（``inject_failed`` / ``open_process_failed`` / ``access_denied``），
- agent 会话失败或超时（``agent_session_failed`` / ``agent_timeout`` /
  ``agent_metadata_timeout``），
- 无匹配类（正常完成，``classWrites=0``）。

这些桶是产品面的一部分：status 输出、自动化脚本与后续报告都依赖它们的稳定性。

.. _design-post-mvp:

7. 后续研究项
-------------

以下是研究/加固项，不是默认发布保证：

- 硬件断点 hook，
- 模块隐藏 / 手动映射，
- Linux/macOS 注入，
- JDK 17/21 上的纯外部 dump（需要证明一个稳定的非 ``_cached_class_file``
  字节来源），
- 长生命周期多 JVM 会话 daemon。

产品方向的完整路线图见 ``doc/roadmap.md``。

.. _design-validation:

8. 验证
-------

构建与单测（选项含义见 :ref:`user-guide-build`）：

.. code-block:: powershell

   xmake f -m release --with_tests=y -y
   xmake build mjserver
   xmake build mjdump
   xmake run mjdump-unittest

涉及真实 JVM 的 live 行为（P0/P1 dump、参数隐藏、P2 兼容性矩阵）由 ``tools/`` 的 live smoke 套件在真实 JDK 8/17/21 环境逐项验证（用法见 :ref:`cli-live-smoke`），行为契约以本文与 :ref:`user-guide` 为准。

.. _design-mvp-scope:

9. MVP 冻结范围
---------------

MVP 是一个可用的 JVM 运行时 class dumper，三条路径边界清晰：

- P0 agent 启动：``-agentpath:mjserver.dll`` 捕获新加载类，并在 VMInit 做
  已加载类 sweep。
- P0.5 参数隐藏：Java management API 里不出现 mjserver 自身的
  ``-agentpath`` / ``-agentlib`` 条目，其余 VM 参数保持可见。
- P1 实用 dump：``mjdump dump -p <pid>`` 注入 ``mjserver.dll``，按
  package/class/regex 过滤做真实 JVMTI dump。
- P1 inspect/classes 预览：``mjdump inspect -p <pid>`` 与 ``classes`` 别名
  枚举已加载类、loader/module 身份、过滤决策与预计输出路径，不写 ``.class``。
- P2 外部读取：``--preflight`` 读 HotSpot 元数据并应用过滤；带
  ``--cache-bait`` 的 ``--external`` 在验证过的 HotSpot 8 上可写出 class bytes。
- 会话诊断：每次实用运行都写 ``_meta.json``，``status`` 解释成功与稳定失败桶。
- no-daemon 会话连续性：``sessions`` 列出已记录会话，
  ``sweep -o <dir> [filters]`` 复用 metadata 向同一输出目录做二次 sweep。
- 选项低残留传递：CLI 发起的 inject/dump/inspect 经远程内存传选项，不创建
  ``%TEMP%\mjdump-<pid>.opts``；诊断模式把显式日志做成 opt-in。

.. _design-mvp-coverage:

10. JVM 覆盖矩阵
----------------

MVP 不要求每个模式在每个 JDK 上都可用，只要求每台被验证的 JDK 上至少有一条
真实 dump 路径：

.. list-table::
   :header-rows: 1

   * - JVM
     - MVP dump 路径
     - 状态
   * - Corretto 8u352
     - P0、P1、P2 ``--external --cache-bait``
     - 已支持
   * - Corretto 17.0.5
     - P0/P1 JVMTI dump
     - 已支持
   * - Corretto 21.0.4
     - P0/P1 JVMTI dump
     - 已支持

对 JDK 17/21，纯 P2 外部字节复制在证明稳定来源前刻意留在 MVP 之外；那里的
MVP 行为是显式诊断：CLDG 遍历与过滤命中可用，但 ``--external`` 以
``errorCode=source_unavailable`` 失败而不是假装复制了字节。实测记录随
兼容性验证批次持续更新（AI 方向层文档维护）。

.. _design-mvp-scenarios:

11. MVP 场景覆盖
----------------

MVP 验证集必须同时覆盖成功 dump 与可解释的非 dump 结果：

- P0 启动 dump 带 include/exclude 过滤：匹配类被写出、排除类不被写出、
  会话正常完成。
- P0.5 参数隐藏：自身 agent 参数被隐藏、无关 VM 参数保留、回退开关
  （``hideAgentArgs=false``）生效。
- P1 在 JDK 8/17/21 上的实用 dump：``dump -p --class`` 写出一个匹配类并
  报告 metadata。
- P1 inspect/classes 预览：枚举已加载类，JSON 输出机器可读，非法过滤
  fail closed。
- dump 输出可复读：目录可被 class 文件工具直接读取，pack 出的 JAR 可被
  JAR 工具读取。
- P1 无匹配会话：命令成功退出，``classWrites=0`` 且 ``sweepMatched=0``。
- P1 regex 过滤：regex 命中类被写出，非命中类不存在。
- P1 late-complete 超时：CLI 超时记录 ``cliErrorCode=agent_timeout``，
  不覆盖 agent 已完成的 metadata。
- P1 稳定失败矩阵：坏 pid、非 HotSpot、权限不足、位数不匹配均写出稳定
  ``errorCode`` metadata。
- 会话 registry 与二次 sweep：会话出现在 ``sessions --json``，
  ``sweep -o`` 可补充一个新选中的类。
- P2 HotSpot 8 外部正向：``--external --cache-bait`` 写出的字节与
  ``javac`` 输出一致（SHA-256 相等）。
- P2 negative gate：无 bait 的 Java 8 与 JDK 17/21 不支持来源均报告稳定
  错误码。

.. _design-mvp-non-goals:

12. MVP 非目标
--------------

- JDK 17/21 上精确的 P2 class 字节复制。
- ``JVM_DefineClass*`` / ``JVM_LookupDefineClass`` 的 native hook 拦截。
- 硬件断点 hook 与模块隐藏 / 手动映射。
- daemon/IPC 型长生命周期多 JVM 会话控制。
- 全自动接入长流程字节码分析管线。

这些保留为 MVP 之后的加固或研究任务（见 :ref:`design-post-mvp`）。
