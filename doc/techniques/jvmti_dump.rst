.. _techniques-jvmti-dump:

JVMTI Dump Agent
================

通过 JVM Tool Interface（JVMTI）在 JVM 进程内注册 ``ClassFileLoadHook``
回调，在类加载时捕获 class bytes。

基础流程
--------

.. code-block:: text

   DLL 加载（Agent_OnLoad / DllMain）
     → 获取 jvmtiEnv
     → 协商 capabilities
     → 注册 ClassFileLoadHook 回调
     → 启动 writer 线程
     → 对已加载类做 retransform sweep
     → 回调收到每一份 class bytes → 过滤 → 写入

.. _techniques-jvmti-dump-entry:

入口点
------

JVMTI agent 支持两种加载方式，需在同一个 DLL 中实现两个入口。

Agent_OnLoad
~~~~~~~~~~~~

JVM 启动时用 ``-agentpath:<dll>=<options>`` 加载。JVMTI 环境在回调参数中
直接传入，适合可靠的 P0 路径。

.. code-block:: c

   JNIEXPORT jint JNICALL Agent_OnLoad(
       JavaVM *vm, char *options, void *reserved
   ) {
       jvmtiEnv *jvmti;
       (*vm)->GetEnv(vm, &jvmti, JVMTI_VERSION);
       // 解析 options → 配置
       // 协商 capabilities
       // 注册回调
       // 启动 writer 线程
       // retransform 已加载类
   }

DllMain → 注入模式
~~~~~~~~~~~~~~~~~~

进程注入场景下 ``Agent_OnLoad`` 不会被调用。需要走 ``DllMain`` →
``CreateThread`` 路径，然后自行发现 JVM：

.. code-block:: c

   DllMain(HMODULE dll, DWORD reason, LPVOID) {
       if (reason == DLL_PROCESS_ATTACH) {
           DisableThreadLibraryCalls(dll);
           CreateThread(NULL, 0, runInjectMode, dll, 0, NULL);
       }
   }

   DWORD runInjectMode(LPVOID) {
       JavaVM *vm;
       jsize count;
       JNI_GetCreatedJavaVMs(&vm, 1, &count);  // 发现已有 JVM
       // 从环境变量或共享内存读取配置
       // 获取 jvmtiEnv → 协商 → 注册回调 → sweep
   }

Capabilities 协商
-----------------

dump agent 至少需要：

.. list-table::
   :header-rows: 1

   * - Capability
     - 用途
   * - ``can_retransform_classes``
     - 触发已加载类的 ClassFileLoadHook
   * - ``can_retransform_any_class``
     - 允许 retransform 任意类（包括系统类）
   * - ``can_generate_all_class_hook_events``
     - 接收所有类加载事件（包括启动类加载器加载的类）

.. code-block:: c

   jvmtiCapabilities caps = {0};
   caps.can_retransform_classes = 1;
   caps.can_retransform_any_class = 1;
   caps.can_generate_all_class_hook_events = 1;
   (*jvmti)->AddCapabilities(jvmti, &caps);

.. _techniques-jvmti-dump-callback:

ClassFileLoadHook 回调
----------------------

回调在类加载、重定义、重转换时触发。收到的是当前时刻 JVM 内存中的字节码
版本。

.. code-block:: c

   void JNICALL ClassFileLoadHook(
       jvmtiEnv *jvmti,
       JNIEnv *env,
       jclass class_being_redefined,
       jobject loader,
       const char *name,        // 类名（斜杠形式）
       jobject protection_domain,
       jint data_len,
       const unsigned char *data,
       jint *new_data_len,
       unsigned char **new_data
   ) {
       // 过滤：跳过不需要的类
       // 将 data/data_len 入队 writer 线程
       // 返回 NULL（不改动字节码）
   }

回调运行在 JVM 类加锁临界区内，不能做阻塞 I/O。数据应通过无锁队列传递给
独立 writer 线程。

.. _techniques-jvmti-dump-retransform:

Retransform Sweep
-----------------

JVM 启动后，大量类在 agent 注册回调之前就已经加载完毕。需要遍历已加载类并
触发 retransform，让它们再经过一次 ``ClassFileLoadHook``\ ：

.. code-block:: c

   void dumpExistingClasses(jvmtiEnv *jvmti, JNIEnv *env) {
       jint count;
       jclass *classes;
       (*jvmti)->GetLoadedClasses(jvmti, &count, &classes);
       // 可选：过滤后只 retransform 目标类
       (*jvmti)->RetransformClasses(jvmti, count, classes);
       (*jvmti)->Deallocate(jvmti, classes);
   }

``RetransformClasses`` 会为每个类触发一次 ``ClassFileLoadHook``\ 。回调中
的 data 是当前内存中的版本（可能是已被其他 agent 变换后的版本）。返回 NULL
表示不修改字节码，但 JVM 仍会将回调视为一次 hook 事件。

性能考虑
~~~~~~~~

- 全量 retransform 在类数量大的时候（5000+）有明显延迟
- 可以先调用 ``GetLoadedClasses``\ ，配合名称过滤只 retransform 目标类
- retransform 是同步的，等待所有回调完成后返回

Hidden Class 处理
-----------------

JDK 9+ 的 Hidden Class（通过 ``Lookup::defineHiddenClass`` 定义）不会触发
``ClassFileLoadHook``\ 。捕获它们需要额外手段：

- **JVMTI 原生支持** — 新版本 JDK 的 ``can_generate_hidden_class_events``
  capability
- **Hook JVM_LookupDefineClass** — 在 native 层拦截 ``jvm.dll`` 中的
  ``JVM_LookupDefineClass`` 导出函数（边界见 :ref:`techniques-native-hook`）

.. code-block:: c

   // MinHook 或类似技术拦截 JVM_LookupDefineClass
   JVM_LookupDefineClass_hook(JNIEnv *env, jclass lookup,
       const char *name, const jbyte *buf, jsize len,
       jobject pd, jint flags)
   {
       // 捕获 buf/len，这就是 hidden class 的字节码
       return original(env, lookup, name, buf, len, pd, flags);
   }

.. _techniques-jvmti-dump-writer:

锁无关 Writer
-------------

回调在 JVM 类锁内部执行，不能调用 ``fwrite``\ （CRT 可能持有锁）、
``malloc``\ （堆锁）、或任何可能触发类加载的 JNI 操作。标准模式是：

.. code-block:: text

   回调（生产者）            writer 线程（消费者）
     ┌──────────┐          ┌──────────┐
     │ MPSC 入队 │ ──────→ │ 文件 I/O │
     └──────────┘          └──────────┘

回调可使用线程本地的预分配块和无锁多生产者队列，将数据投递给 writer 线程。
队列的具体实现、容量和内存策略属于产品设计；关键约束是回调返回前完成必要
复制，且生产者不能等待消费者。

数据流
------

.. code-block:: text

   ClassFileLoadHook
     → 按类名与 loader 过滤
     → 命中 → MPSC queue 入队
     → writer 线程出队
     → 按类身份写入 classfile
     → 更新采集统计

输出布局和会话元数据是产品格式，不影响该技术路径的字节保真度判断。

稳定性边界
----------

- 回调内部不可做阻塞操作（包括 CRT 文件 I/O、堆分配、类加载触发）
- 回调中的 ``data`` 是 JVMTI 分配的缓冲区，回调返回后失效，必须拷贝
- 多个 agent 共存时，hook 顺序不可控，观察到的 data 可能已被前序 transformer 修改
- retransform 会触发的回调中，``class_being_redefined``\ 参数可能为 NULL
  （某些 JVM 实现）
