.. _techniques-external-memory:

外部内存读取
============

不在目标进程中执行任何代码，通过 ``ReadProcessMemory`` 从进程外读取 JVM
内存中的类元数据和 class bytes。

适用场景
--------

- 目标 JVM 无法注入 DLL（权限不足、安全软件拦截）
- 不希望修改目标进程的任何状态
- 只读 inventory 或预览，不需要完整 dump
- JVMTI / Instrumentation 不可用或被禁用

读取链路
--------

.. code-block:: text

   目标 jvm.dll
     → SA 导出表（gHotSpotVMStructs, gHotSpotVMTypes, ...）
     → 结构快照（VmStructSnapshot）
     → CLDG / SystemDictionary 遍历
     → InstanceKlass → _cached_class_file → ClassFileData
     → bytes

定位 VM 结构描述
----------------

HotSpot 的 Serviceability Agent 导出几个全局变量描述 VM 内部结构的布局。

SA 导出定位
~~~~~~~~~~~

将目标 ``jvm.dll`` 从磁盘映射到本地（``LoadLibraryEx`` with
``DONT_RESOLVE_DLL_REFERENCES``）\ ，通过 ``GetProcAddress`` 获取导出表中
SA 符号的 RVA，然后投影到远程模块基址。

.. code-block:: text

   gHotSpotVMStructs   → VMStructEntry[]（字段名、类型、偏移）
   gHotSpotVMTypes     → VMTypeEntry[]（类型名、大小）
   gHotSpotVMIntConstants → IntConstantEntry[]
   gHotSpotVMLongConstants → LongConstantEntry[]

签名扫描
~~~~~~~~

如果 SA 符号被 strip（发布版 JDK 通常保留），可在 ``jvm.dll`` 的 ``.data``
节中特征扫描已知结构名来确定位置。

结构快照
--------

把远程 VM 的结构描述表整份拷贝到本地内存，解析出每个类型/字段的偏移和
大小。远程指针（如 ``InstanceKlass*``\ ）在本地不做解引用，保留为远程地址。

.. code-block:: c

   struct VmStructSnapshot {
       // 每个字段的偏移量（从结构基址开始）
       ptrdiff_t ClassLoaderDataGraph__head;
       ptrdiff_t ClassLoaderData__next;
       ptrdiff_t ClassLoaderData__klasses;
       ptrdiff_t Klass__next_link;
       ptrdiff_t InstanceKlass__cached_class_file;
       ptrdiff_t ClassFileData__length;
       ptrdiff_t ClassFileData__bytes;
   };

.. _techniques-external-memory-cldg:

CLDG 遍历
---------

HotSpot 使用 ``ClassLoaderDataGraph`` 管理所有类加载器的元数据。从静态
指针 ``ClassLoaderDataGraph::_head`` 开始，遍历单向链表。

.. code-block:: c

   // 1. 读取远程地址 ClassLoaderDataGraph::_head → cloader
   // 2. 对每个 ClassLoaderData:
   //    a. 读取 _klasses → Klass*（InstanceKlass 链头）
   //    b. 遍历 Klass::_next_link 链
   //    c. 对每个 InstanceKlass 读取名称和缓存
   // 3. 沿 ClassLoaderData::_next 走到下一个加载器

每个 ``InstanceKlass`` 可通过 ``_name``\ （Symbol*）读取类名。

SystemDictionary 回退
~~~~~~~~~~~~~~~~~~~~~

JDK 8 没有 CLDG（或 ``ClassLoaderDataGraph::_head`` 不可用）\ ，回退到
``SystemDictionary`` 遍历：

.. code-block:: text

   SystemDictionary::_dictionary
     → BasicHashtable buckets
     → DictionaryEntry
     → Klass literal

SystemDictionary 是哈希表结构，需要遍历所有 bucket 和冲突链。

.. _techniques-external-memory-source-gate:

字节来源门
----------

VM 内部保留原始 class bytes 的方式有限，且不可靠：

.. list-table::
   :header-rows: 1

   * - 来源
     - 条件
     - 可靠性
   * - ``InstanceKlass::_cached_class_file``
     - ClassFileData 指针非空
     - JDK 8 通常有，JDK 17+ 不一定
   * - ``ClassFileData``
     - 长度 + 字节指针
     - 需要结构快照中能找到
   * - 变换后的 class
     - 经过 retransform 后
     - 已不是原始版本

外部读取的正确模型是「发现一个具体的字节来源并证明其边界」，而非假设
``InstanceKlass`` 天然携带原始 classfile。

.. code-block:: c

   bool sourceGate(VmStructSnapshot *snap) {
       // 检查所有需要的偏移都已解析
       return snap->ClassLoaderDataGraph__head != 0
           && snap->InstanceKlass__cached_class_file != 0
           && snap->ClassFileData__length != 0
           && snap->ClassFileData__bytes != 0;
   }

来源门不通过时，不能猜测偏移，应返回稳定的错误码。

.. _techniques-external-memory-cache-fill:

缓存填充
--------

对 ``_cached_class_file`` 为 NULL 的类，可以使用 cache-bait 机制：注入一个
极简 agent，注册空的 ``ClassFileLoadHook``\ ，让 JVM 在类加载时缓存
class bytes。

.. code-block:: text

   external read 发现 _cached_class_file == NULL
     → 注入 cache-bait agent
     → 触发类的 retransform / 等待新类加载
     → JVM 填充 _cached_class_file
     → external read 再次读取 → 命中

机制细节见 :ref:`techniques-decoy-cache`。

一致性窗口
----------

外部读取时 JVM 仍在运行，可能同时加载/卸载/重定义类。

- 地址有效性：指针解引用前需检查地址是否在模块地址范围
- 生命周期：遍历 CLDG 时加载器可能被 GC，需要容忍局部失败
- 暂停窗口：短暂暂停进程（``DebugBreakProcess`` + 恢复）可获得一致性快照，
  但会改变目标时序
- 重试：对于读取失败的节点记录为「不可读」，不中断整体遍历

稳定性边界
----------

- VM 结构布局随 JDK 版本、build 选项、指针压缩而变化
- ``InstanceKlass``\ 的字段顺序在 HotSpot 8、11、17、21 之间不同
- 某些 JVM 实现（如 GraalVM）可能没有 SA 导出表
- ``_cached_class_file`` 可能在 GC 时被回收
- 外部读取得到的是字节层、结构层还是身份层，需要在输出中明确标注

.. _techniques-external-memory-compat:

兼容矩阵与状态字段
------------------

P2 的原则是先做显式 gate：能外部读取就写出 class bytes，不能读取就输出稳定
``errorCode`` 与原因，而不是假装成功。当前矩阵：

- **Corretto 8u352**：walk ``systemDictionary``，bytes 来自
  ``hotspot8_hidden_cached_class_file``；``--external --cache-bait`` 可写出
  匹配 class bytes
- **Corretto 17.0.5**：walk ``cldg``，无 bytes 来源；显式负向 gate
  ``errorCode=source_unavailable``
- **Corretto 21.0.4**：同 JDK 17

外部会话恒写 ``<out>/_meta.json``，兼容性关键字段：

- ``p2WalkSource``：``cldg`` 或 ``systemDictionary``
- ``p2Source``：选定的 class bytes 来源，或 ``none``
- ``p2SourceAvailable``：是否允许字节复制
- ``p2SourceReason``：不可用原因（人类可读）
- ``errorCode``：稳定失败桶（``source_unavailable`` / ``no_cached_class_bytes``）
- ``cacheBaitState``：可选 ``_bait`` 准备会话的终态
