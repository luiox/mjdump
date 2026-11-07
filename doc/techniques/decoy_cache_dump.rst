.. _techniques-decoy-cache:

Decoy / Cache-bait
==================

利用 HotSpot 的 class bytes 缓存机制，以最小侵入代价触发
``InstanceKlass::_cached_class_file``\ 填充，然后通过外部内存读取获取
字节码。

原理
----

HotSpot 的 ``InstanceKlass`` 中有一个可选字段 ``_cached_class_file``
（``ClassFileData*``\ ），当类文件经过 JVMTI ``ClassFileLoadHook`` 回调时，
JVM 会将传入的字节码缓存到该字段中。

.. note::

   这不是 HotSpot 规范承诺的行为，而是实测发现的实现细节。不同 JDK 版本的
   行为可能不同。

Decoy Agent
-----------

注册一个空的 ``ClassFileLoadHook``\ （不修改任何字节码），然后立即卸载。
副作用是 JVM 会为后续加载的每个类填充 ``_cached_class_file``\ 。

.. code-block:: c

   // 一个极简的 decoy JVMTI agent
   Agent_OnLoad(JavaVM *vm, char *, void *) {
       jvmtiEnv *jvmti;
       (*vm)->GetEnv(vm, &jvmti, JVMTI_VERSION);

       jvmtiCapabilities caps = {0};
       caps.can_retransform_classes = 1;
       caps.can_retransform_any_class = 1;
       (*jvmti)->AddCapabilities(jvmti, &caps);

       jvmtiEventCallbacks cb = {0};
       cb.ClassFileLoadHook = [](jvmtiEnv*, JNIEnv*, jclass,
           jobject, const char*, jobject, jint,
           const unsigned char*, jint*, unsigned char**) {
           // 空回调，不改数据
       };
       (*jvmti)->SetEventCallbacks(jvmti, &cb, sizeof(cb));
       (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
           JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);

       // 如果需要在此时填充已有的类，做一遍 retransform
       // 然后可以卸载（FreeLibraryAndExitThread）
       return JVMTI_ERROR_NONE;
   }

这个 agent 不需要 writer 线程、不需要 MPSC 队列、不需要文件 I/O。只是注册
一个空 hook 然后让路。

与外部读取配合
--------------

decoy agent 和外部内存读取配合构成两阶段 dump：

.. code-block:: text

   阶段 1：decoy agent 注入
     → 注册空 ClassFileLoadHook
     → retransform 目标类（或等待新类加载）
     → JVM 填充 _cached_class_file
     → agent 卸载

   阶段 2：外部内存读取
     → 遍历 CLDG
     → 对每个 InstanceKlass 检查 _cached_class_file
     → 命中 → ReadProcessMemory 拷贝 ClassFileData
     → 未命中 → 记录为 no_cached_class_bytes

decoy agent 的生命周期可以是瞬时的（注入 → hook → retransform → 卸载），
对目标 JVM 的影响小于长期驻留的 dump agent。

.. _techniques-decoy-cache-retransform:

Retransform 填充
----------------

对已加载的类，decoy agent 需要调用 ``RetransformClasses`` 来触发
``ClassFileLoadHook`` 回调，回调执行后 JVM 会填充 ``_cached_class_file``\ ：

.. code-block:: text

   GetLoadedClasses
     → 过滤目标类
     → RetransformClasses（触发 ClassFileLoadHook）
     → 空回调返回 NULL
     → JVM 缓存当前 class bytes

注意事项：

- ``RetransformClasses`` 是同步操作，对 5000+ 类执行时有明显延迟
- 回调中的 ``data`` 是当前内存版本（可能已被其他 agent 变换）
- 如果目标类已经被其他 agent 变换过，缓存的是变换后的版本而非原始文件版本

.. _techniques-decoy-cache-bait:

Cache-bait
----------

当目标是特定类且 ``_cached_class_file`` 为空时，cache-bait 机制注入 decoy
agent 并只 retransform 目标类：

.. code-block:: text

   目标：com.example.Foo
   步骤：
     1. 外部读取 → Foo._cached_class_file == NULL
     2. 注入 decoy agent（cacheBait=true）
     3. agent retransform Foo
     4. Foo._cached_class_file → ClassFileData{bytes, length}
     5. 外部读取 → 拷贝 bytes
     6. agent 卸载

cache-bait 是有副作用的：retransform 会触发 JVM 重新验证和链接类，可能
触发死循环（如果 ``ClassFileLoadHook`` 的实现依赖于自身被加载）。缓存填充
成功后应立即卸载 agent。

限制
----

- ``_cached_class_file`` 不是 JVM 规范的一部分，版本间行为可能变化
- JDK 21 的某些 build 中，``_cached_class_file``\ 可能始终为 NULL
- 缓存的是变换后的版本，不保证与原始 classfile 一致
- retransform 触发类加载器锁，多线程环境下可能死锁
- 需要 ``can_retransform_classes`` + ``can_retransform_any_class``
  capability（某些 JVM 配置禁用）
