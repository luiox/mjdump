.. _jdumper-docs:

jdumper 设计规格
================

.. _jdumper-doc-map:

文档地图与分层规则
------------------

jdumper 是 JVM 运行时字节码 dump 工具（JVMTI 三模式）。本目录是跨模块设计、
数据流、命令面契约与技术背景的权威文本；微观契约（参数、返回值、失败模式、
不变量）在代码 ``///`` 注释里（Doxygen 层），本目录只管「为什么这样设计」与
「契约是什么」。

分层规则：

- ``doc/*.rst``（本目录）：设计规格层，RST 编写，Sphinx ``-W`` 门禁。
- 代码 ``///`` 注释：微观契约层，Doxygen ``WARN_AS_ERROR`` 门禁（``doc/Doxyfile``）。
- markdown（``README.md`` / ``doc/roadmap.md`` / ``doc_ai/``）：人读与 AI
  方向层，不进 doctree。

代码注释以 ``doc:<label>`` 形式指向本目录的具体小节；``check_refs.py`` 校验
这些指向不会腐烂。各篇文档的标题即为该文档的根 label。

.. toctree::
   :maxdepth: 2
   :caption: 设计与契约

   design
   cli
   user_guide

.. toctree::
   :maxdepth: 2
   :caption: 运行时 class bytes 技术 Wiki

   techniques/capture_paths
   techniques/hotspot_external_read
   techniques/jvmti_dump
   techniques/instrumentation_dump
   techniques/native_hook_dump
   techniques/external_memory_dump
   techniques/decoy_cache_dump

.. _techniques-wiki-charter:

技术 Wiki 约定
--------------

技术 Wiki 讨论「运行中的 JVM 中可以在何处取得何种字节码信息」。它关注数据
来源、时机、保真度和可观测面，不描述任何具体工具的投递方式、命令或支持
状态。

基本分类：

.. list-table::
   :header-rows: 1

   * - 观察点
     - 通常得到的数据
     - 主要问题
   * - 类文件变换事件
     - 完整 classfile 或其变换版本
     - 取得时刻与变换顺序
   * - 方法级查询
     - 某个方法的 code array
     - 缺少类级结构和属性
   * - 类定义边界
     - 定义请求附近的 class 数据
     - VM 私有实现和动态生成形态
   * - VM 内存与服务性数据
     - 类身份、元数据或缓存内容
     - 布局变化与原始 bytes 是否仍存在

「找到一个类」不等于「拿到完整 classfile」；「拿到方法 bytecode」也不等于
「能重建原始类」。
