.. _techniques-capture-paths:

JVM Class Bytes 获取路径
========================

.. _techniques-capture-paths-problem:

问题与术语
----------

JVM 运行时可见的字节码信息至少分为四类：

- **classfile**：以 ``CAFEBABE`` 开头的完整类文件，包含常量池、字段、方法、属性和 code。
- **方法 code array**：某个方法的指令序列，不包含完整类结构。
- **类身份**：类名、加载器、模块或运行时类对象等定位信息。
- **运行时语义**：一次调用的参数、返回值、已解析目标或对象状态。

获取技术应先明确需要哪一类数据。很多错误设计来自将其中两类数据视为可以无损互换。

.. _techniques-capture-paths-lifecycle:

JVM 生命周期中的观察点
----------------------

.. code-block:: text

   classfile 来源
     -> 转换/重定义
     -> 类定义
     -> 验证、链接、初始化
     -> 执行与 JIT
     -> 元数据和缓存保留

越靠前的观察点通常越接近完整 classfile；越靠后的观察点越接近运行时语义，
但越可能丢失原始文件的边界、属性或字节级表示。

.. _techniques-capture-paths-java-instrumentation:

Java Instrumentation
--------------------

Instrumentation 在 Java 层提供 class transformation 回调。它适合在类定义、
重定义或重转换阶段观察完整类文件载荷，并可将变换后的载荷继续交给 JVM。

它的关键问题不是「能否收到 byte 数组」，而是该数组位于变换链的哪个位置：
前序 transformer、后序 transformer、重定义语义和加载器隔离都会改变观察到的
版本。多个 transformer 共存时，应把「原始」「本次输入」「本次输出」和
「最终定义版本」视为不同对象。

JVMTI ClassFileLoadHook 与重转换
--------------------------------

JVMTI 的 ClassFileLoadHook 位于 native agent 层，能够在类文件加载或重定义
相关流程中观察 classfile 数据。其技术价值与 Java transformer 类似，但其
capability 协商、内存所有权、回调时序和错误处理处于 native/JVM 边界。

已加载类的重转换可重新触发可变换的类文件路径，但它不是时间旅行：无法保证
恢复到磁盘中的最初版本，也不能假定所有类或所有 transformation 都具有相同
语义。

JVMTI GetBytecodes
------------------

GetBytecodes 面向单个 ``jmethodID``\ ，返回的是方法的 bytecode 指令数组。
它适合分析方法体、动态生成的方法结构，或与断点/栈帧观察交叉验证。

它不是 class dump 的替代品：方法 code array 无法独立恢复常量池、字段、类
属性、异常表以外的上下文、原始属性排列或完整 classfile 二进制形式。将多个
方法 code array 拼接为「class 文件」通常会制造一个看似可读但并不保真的产物。

类定义边界
----------

类定义边界指 VM 接受某份 class 数据并准备创建运行时类的时刻。观察此边界
理论上能覆盖运行时解密、网络下载、字节数组生成和自定义类加载器产生的类。

该边界通常属于具体 VM 的内部实现。方法可能被内联、分流、替换或被不同的
定义入口绕过；同名符号也不保证代表唯一的定义路径。因此定义边界技术需要先
建立「覆盖哪些定义形态」的模型，不能把一次命中等同于全覆盖。

VM 内存与服务性数据
-------------------

进程外或服务性读取可从 VM 元数据中发现类、加载器关系、方法结构和某些缓存。
它特别适合 inventory、对象关系和无法进入目标进程时的只读观察。

内存中存在 ``Klass``\ 、常量池或已解析的方法，并不推出内存中仍有原始
classfile。VM 可以只保留解析后的结构，丢弃输入缓冲区，或将缓存替换为变换
后的版本。外部读取因此必须区分类身份恢复、结构恢复和原始 bytes 恢复。

.. _techniques-capture-paths-comparison:

路径比较
--------

.. list-table::
   :header-rows: 1

   * - 技术
     - 完整 classfile 潜力
     - 典型强项
     - 根本限制
   * - Instrumentation
     - 高
     - Java 层变换链、重定义
     - 只能观察可到达的变换时机
   * - ClassFileLoadHook
     - 高
     - native 层类文件事件
     - capability、时序和 agent 可见性
   * - GetBytecodes
     - 低
     - 方法体观察
     - 不是完整类结构
   * - 定义边界观察
     - 高
     - 运行时生成/解密类
     - VM 私有实现与覆盖盲区
   * - VM 内存读取
     - 不确定
     - inventory、元数据、只读分析
     - 布局与原始缓存均不稳定

.. _techniques-capture-paths-observability:

可观测面与检测面
----------------

类文件事件路径可能暴露 transformer、agent、重转换或类加载时序；定义边界
观察可能引入代码完整性、调用路径和性能变化；外部读取则依赖进程访问、内存
读取和调试/服务性接口。技术选择不应只比较「是否注入」，还要比较目标可见的
行为、持久痕迹和对执行时序的影响。

常见误区
--------

- 将 ``GetBytecodes``\ 的方法结果当作完整 classfile。
- 将类元数据可遍历误判为原始 classfile 可恢复。
- 将第一次看到的 bytes 当作唯一原始版本，忽略 transformer 顺序。
- 将一个 VM 的定义入口或内存布局当作所有 JVM 的通用机制。
