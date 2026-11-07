.. _user-guide:

mjdump 使用指南
================

JVM 运行时字节码导出工具，支持 JVMTI Agent（P0）、远程注入（P1）、外部读取
（P2）三种方案。命令面契约见 :ref:`cli`，总体模式表见 :ref:`design-modes`。

.. _user-guide-capabilities:

0. 能力一览
-----------

- **P0 JVMTI agent dump**：``-agentpath:mjserver.dll`` 启动导出全部类文件
- **P1 远程注入**：``mjdump inject -p <pid>``，CreateRemoteThread 注入
- **P1 dump 前预览**：``inspect`` / ``classes``，预览 loaded classes 与过滤命中
- **P2 只读 inventory**：``inspect --external``，HotSpot Klass walk，不注入
- **P2 外部读取预检**：``dump --preflight``，定位 jvm.dll + 探测 VMStructs
- **P2 外部读取**：JDK 8 ``--external --cache-bait``；JDK 17/21 负向 gate
- **输出打包与索引**：``pack`` 产出标准 JAR；``_index.json`` 记录 SHA-256
- **分组导出**：``--group-by package|module``
- **过滤**：include/exclude glob、正则、module/loader 精确值
- **低残留注入**：经远程内存传 options，不落 ``.opts`` 文件

已知限制（首载类 module 恒 ``unknown`` 等）见 :ref:`cli-filtering-sessions` 与
README。构建与验证入口见下节。

.. _user-guide-build:

1. 构建
-------

.. code-block:: bash

   set JAVA_HOME=D:\sdk\java\jdk17
   xmake f -m release --with_tests=y -y
   xmake build mjserver
   xmake build mjdump
   xmake run mjdump-unittest

需要 Windows x64、JDK 与同位数 C++ 工具链。``JAVA_HOME`` 用于构建期消费
JNI/JVMTI 头文件。首次配置前需注册家族包仓：

.. code-block:: bash

   xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git

libjninfra 的版本锁定与占位符说明见 ``xmake/packages/libjninfra.lua``。

.. _user-guide-p0:

2. P0：-agentpath 启动
----------------------

.. code-block:: bash

   java -agentpath:build\windows\x64\release\mjserver.dll -jar app.jar

   # 带参数
   java "-agentpath:build\windows\x64\release\mjserver.dll=out=C:\Dump,include=com/example/**" -jar app.jar

agent 选项串的完整键位与默认值见 :ref:`user-guide-config`。

.. _user-guide-p1:

3. P1：远程注入
---------------

.. code-block:: bash

   mjdump inject -p <pid>
   mjdump inject -p <pid> --dll build\windows\x64\release\mjserver.dll -o C:\Dump
   mjdump inject -p <pid> --package com.example
   mjdump inject -p <pid> --class com.example.Foo

注入的两段式加载（先 LoadLibraryW，再经远程内存调控制入口）见
:ref:`cli-mode1`。

.. _user-guide-dump:

4. 默认 dump 与 inspect
-----------------------

.. code-block:: bash

   mjdump list
   mjdump inspect -p <pid> --package com.example --limit 100
   mjdump inspect -p <pid> --class com.example.Foo --module unnamed --json
   mjdump inspect -p <pid> --external --class com.example.Foo --filter-preview
   mjdump inspect -p <pid> --package com.example --view matched
   mjdump dump -p <pid> -o C:\Dump --package com.example
   mjdump dump -p <pid> -o C:\Dump --package com.example --group-by package
   mjdump dump -p <pid> -o C:\Dump --class com.example.Foo --group-by module
   mjdump dump -p <pid> -o C:\Dump --package com.example --quiet
   mjdump inspect -p <pid> --class com.example.Foo --diagnostic
   mjdump status -o C:\Dump
   mjdump status -o C:\Dump --json
   mjdump sessions
   mjdump sweep -o C:\Dump --class com.example.NewTarget
   mjdump pack -i C:\Dump -o C:\Dump\dumped.jar

``inspect -p`` 是 dump 前预览路径：CLI 先校验过滤规则，再注入
``mjserver.dll`` 的轻量 inspect 模式，只枚举 ``GetLoadedClasses``，不触发
``RetransformClasses``，不写 ``.class``。输出位于 ``<out>/_inspect.json``，
包含 class name、是否 JDK、是否匹配当前过滤、预计 dump 路径和跳过原因。
``classes`` 是 ``inspect`` 的别名。JDK 9+ 会记录 Java module 名称；JDK 8
没有 module API，module 记为 ``unknown``。loader 记录 class loader 的 Java
类名，bootstrap loader 记为 ``bootstrap``。为区分同名 loader 实例，每行还
会记录 ``loaderIdentityHash``、``loaderParent`` 和
``loaderParentIdentityHash``。

``inspect --view all|matched|skipped`` 控制 ``_inspect.json`` 中的 class
行：``all`` 输出全部可枚举类，``matched`` 只输出会被当前规则 dump 的类，
``skipped`` 只输出被过滤跳过的类。``--filter-preview`` 是
``--view matched``\ 的快捷写法，用于 dump 前查看「会 dump 什么」。
``matched`` 统计始终表示全量命中数；``written`` 表示当前 view 实际写出的
行数。

``inspect --external`` 是只读 P2 inventory：不注入 DLL、不触发
retransform，通过 HotSpot metadata 遍历 Klass 名称并写出同样的
``_inspect.json``。该路径能预览 class/package/regex/JDK 过滤，但拿不到
JVMTI 的真实 module/loader 实例，所以 module 和 loader 字段会标为
``unknown``；如果指定 ``--module`` 或 ``--loader`` 精确过滤，未命中会以
``module_not_matched`` / ``loader_not_matched`` 展示，避免误报会 dump。

``--group-by none|package|module`` 控制 dump 输出布局。默认 ``none`` 保持
``com/example/Foo.class``\ 兼容路径；``package`` 写到
``_by_package/com/example/Foo.class``；``module`` 写到
``_by_module/<module>/com/example/Foo.class``。P1/JVMTI 已加载类 dump 会
记录真实 Java module，无法可靠识别 module 的路径使用 ``unknown`` 分组。
``inspect`` 的 ``estimatedPath`` 会按同一规则预估。

**module 首载限制**：module 信息只能从 ``class_being_redefined``
（retransform 路径）推导，而 ``ClassFileLoadHook`` 对 agent 启动后**新加载**
的类该参数为 NULL。因此首载类 module 恒为 ``unknown``——``--group-by
module``\ 下它们全部进 ``_by_module/unknown/``；``--module`` 精确过滤对它们
恒不命中（按 ``module_not_matched`` fail-closed 跳过）。只有 VMInit sweep
触达的已加载类（经 ``RetransformClasses``）携带真实 module。如需按 module
过滤新加载的类，先等类加载完成后再用 ``--module`` 做第二次 dump。

``dump -p`` 是主力实用路径：CLI 先验证目标是同位数 HotSpot JVM，再通过 P1
注入 ``mjserver.dll``，等待 ``<out>/_meta.json`` 进入终态。零匹配不是错误；
此时 ``status`` 会显示 ``classWrites: 0`` 和 ``sweepMatched: 0``。每个成功
写出的 class 还会记录到 ``<out>/_index.json``，包括 class name、相对路径、
source path、大小、class magic 校验结果、路径安全校验和 SHA-256，供后续
pack/读回 gate 使用。index v2 同时提供 ``dumpedClasses``、
``skippedClasses``、``filterHits`` 和 ``postValidation``；如果 dump 前曾用
同一输出目录执行 ``inspect``，被过滤跳过的 class 会从 ``_inspect.json``
汇总到 ``skippedClasses``。dump 完成后会自动执行 post-verify；如果 index
写出失败、class 读回失败、magic 无效或路径不安全，CLI 会以
``errorCode=post_verify_failed``\ 返回失败，避免误报成功。

P1 失败也会尽量写出 ``<out>/_meta.json``，并提供稳定 ``errorCode``：
``bad_pid``、``access_denied``、``open_process_failed``、
``bitness_mismatch``、``not_hotspot``、``read_process_failed``、
``inject_failed``、``agent_session_failed``、``agent_timeout``、
``agent_metadata_timeout``。

``sessions`` 读取当前用户的本地 session registry，列出由 CLI 发起过的
dump/inspect session、pid 是否还活着、metadata 状态、写出计数和输出目录。
``--json`` 输出机器可读列表，``--active`` 只显示仍处于 ``running`` 且 pid
存活的 session。registry 位于
``%LOCALAPPDATA%\mjdump\sessions.jsonl``\ ，不会启动 daemon，也不会额外
触碰目标 JVM。

``sweep -o <dump-dir> [filters]`` 会重新读取 ``<dump-dir>/_meta.json`` 中的
pid，并以同一个输出目录再次执行 ``dump -p``。传入新过滤规则时就是一次二次
sweep；不传过滤规则时会复用 metadata 里的过滤条件。已有 ``.class`` 保留，
``_index.json`` 会在新 sweep 后重建，因此可以作为轻量增量 dump 使用。agent
已经加载时，二次 sweep 复用同一个控制入口；初次 dump 和二次 sweep 的配置
都通过远程内存传递，不再创建 ``%TEMP%\mjdump-<pid>.opts``。

如果 CLI 等待超时但 agent 已经创建了 running metadata，CLI 不会覆盖
``_meta.json``。本次命令失败会记录到 ``<out>/_cli_error.json``，
``status`` 会继续展示 agent 最终状态，同时附带 ``cliErrorCode`` /
``cliLastError``，避免 late complete 把失败/成功状态来回覆盖。

.. _user-guide-output-analysis:

5. dump 输出分析
----------------

dump 目录保持 ``com/example/Foo.class`` 兼容布局，任何能读 class 文件树或
目录的工具都可以直接消费；需要交给只接受 JAR 的工具时先打包：

.. code-block:: bash

   mjdump pack -i C:\Dump -o C:\Dump\dumped.jar

``pack`` 默认会在写出 JAR 后立刻读回验证资源数量；如果只想做快速打包，
可以加 ``--no-verify``。

.. _user-guide-p2:

6. P2：外部读取预检
-------------------

.. code-block:: bash

   mjdump dump -p <pid> --preflight
   mjdump dump -p <pid> --preflight --class com.example.Foo
   mjdump dump -p <pid> --external -o C:\Dump --class com.example.Foo
   mjdump dump -p <pid> --external --cache-bait -o C:\Dump --class com.example.Foo

P2 是显式诊断路径，不是默认 dump 路径。当前 MVP 边界：

- JDK 8 / Corretto 8u352：``--external --cache-bait`` 可写出匹配 class
  bytes；验证时 dump 出的 ``.class`` 的 SHA-256 应与 ``javac`` 输出一致。
- JDK 8 无 bait：应稳定失败为 ``errorCode: no_cached_class_bytes``。
- JDK 17 / 21：只承诺 CLDG 身份遍历和 filter 命中诊断；纯 external byte
  copy 应稳定失败为 ``errorCode: source_unavailable``\ ，真实 dump 走 P1。

``--cache-bait`` 的机制：先以 agent 跑一个准备会话（产物在 ``<out>/_bait``），
对过滤命中的类在 ``ClassFileLoadHook`` 里原样回传 class bytes，使 HotSpot
填充隐藏的缓存态；随后外部读取会话从推断的 ``InstanceKlass`` 槽位复制。
class bytes 逐字节复制，无字节码改写；该策略在 Corretto 8u352 上验证，
其他 JDK 家族需单独核源门与缓存行为。跨 JVM 家族的兼容矩阵与
``p2*`` 状态字段见 :ref:`techniques-external-memory-compat`。

.. _user-guide-config:

7. 配置参数
-----------

.. list-table::
   :header-rows: 1

   * - 参数
     - 类型
     - 默认值
     - 说明
   * - ``out``
     - path
     - ``C:\JavaClassDump``
     - 输出目录
   * - ``include``
     - glob
     - （全部）
     - 白名单
   * - ``exclude``
     - glob
     - （空）
     - 黑名单
   * - ``includeRegex``
     - regex
     - （空）
     - 正则白名单
   * - ``excludeRegex``
     - regex
     - （空）
     - 正则黑名单
   * - ``module``
     - exact list
     - （空）
     - 逗号分隔 module 精确过滤，CLI 用 ``--module``
   * - ``loader``
     - exact list
     - （空）
     - 逗号分隔 class loader 精确过滤，CLI 用 ``--loader``
   * - ``groupBy``
     - enum
     - ``none``
     - dump 输出布局：``none``、``package``、``module``
   * - ``excludeJdk``
     - bool
     - ``true``
     - 过滤 JDK 类
   * - ``cacheBait``
     - bool
     - ``false``
     - 触发 HotSpot cached class-file 状态
   * - ``hideAgentArgs``
     - bool
     - ``true``
     - 隐藏自身 agent 参数
   * - ``silent``
     - bool
     - ``false``
     - 静默模式
   * - ``diagnostic``
     - bool
     - ``false``
     - 显式写出注入诊断日志
   * - ``inspectLimit``
     - integer
     - ``0``
     - inspect 输出 class 行数上限，0 表示不限制
   * - ``inspectView``
     - enum
     - ``all``
     - inspect 输出视图：``all``、``matched``、``skipped``

.. _user-guide-hideagentargs:

8. hideAgentArgs 失效边界
-------------------------

该开关通过 retransform ``sun/management/VMManagementImpl#getVmArguments()``
实现，只影响补丁生效后的调用，且存在两个结构性限制：

1. **vmArgs 已填充则永久失效**。补丁字节码保留了 ``vmArgs`` 缓存字段的命中
   快路径（retransform 不能撤销已发生的读取）。目标 JVM 在 agent 生效前已
   调用过 ``getInputArguments()``\ （典型：live-attach 到开了 JMX 监控的
   长运行 JVM）时，``vmArgs`` 字段已填充，补丁后的 getter 仍返回缓存中的
   未过滤列表，对整个 JVM 生命周期无效。P0 ``-agentpath`` 启动（management
   类首次加载即被补丁）不受此限制。
2. **已读出的数据不可撤回**。补丁只改写后续调用；此前已被监控端/JMX 客户端
   /日志拉取并缓存的参数列表不会消失。live-attach 场景还存在 attach 到
   retransform 完成之间的窗口，窗口内的调用返回未过滤结果。

.. _user-guide-cli-modes:

9. CLI 输出三档
---------------

默认模式写必要结果文件并打印成功摘要，但不展开完整 agent options；
``--quiet`` 会让 agent 使用 ``silent=true``，并收敛成功路径的 CLI 输出；
``--diagnostic`` 才写 ``%TEMP%\mjdump-<pid>.log``\ 注入诊断日志，并在 CLI
状态输出中打印原始 options。默认注入路径不写该 log，也不写
``%TEMP%\mjdump-<pid>.opts``；CLI 会在远程内存中传递 options，并调用
``mjserver.dll`` 的控制入口。需要排查注入早期、尚未进入控制入口的问题时，
也可在目标环境中设置 ``JDUMPER_DIAGNOSTIC_LOG=1`` 后重试。
