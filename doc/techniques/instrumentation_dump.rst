.. _techniques-instrumentation:

Instrumentation Dump
====================

通过 Java 层的 ``java.lang.instrument.Instrumentation`` API 注册
``ClassFileTransformer``\ ，在类加载时捕获 class bytes。

基础流程
--------

.. code-block:: text

   premain(String, Instrumentation)
     → 注册 ClassFileTransformer
     → 对已加载类调用 retransformClasses
     → transformer.transform() 收到每一份 class bytes → 过滤 → 写入

premain Agent
-------------

``META-INF/MANIFEST.MF`` 中声明 ``Premain-Class``\ ，通过 ``-javaagent:<jar>``
加载。

.. code-block:: java

   public class DumpAgent {
       public static void premain(String args, Instrumentation inst) {
           // 注册 transformer（第二个参数 true = 支持 retransform）
           inst.addTransformer(new DumpTransformer(), true);
           // 可选：触发已加载类的 retransform
           retransformLoadedClasses(inst);
       }
   }

Manifest 必须指定 ``Can-Retransform-Classes: true``\ ，否则
``retransformClasses`` 会抛出 ``UnmodifiableClassException``。

ClassFileTransformer
--------------------

.. code-block:: java

   public class DumpTransformer implements ClassFileTransformer {
       public byte[] transform(
           ClassLoader loader,
           String className,       // 斜杠形式（com/example/Foo）
           Class<?> classBeingRedefined,
           ProtectionDomain protectionDomain,
           byte[] classfileBuffer // 当前 class bytes
       ) {
           // 过滤：跳过不需要的类
           if (!shouldDump(className, loader)) return null;
           // 将 classfileBuffer 写入文件或队列
           enqueue(className, classfileBuffer);
           // 返回 null 表示不改字节码
           return null;
       }
   }

``addTransformer`` 的第二个参数 ``canRetransform`` 很关键：

- ``true``\ ：retransform 时也会触发此 transformer
- ``false``\ ：仅捕获初始加载，retransform 时跳过

Retransform 已加载类
--------------------

``premain`` 执行时大量核心类已加载。需要主动触发 retransform：

.. code-block:: java

   void retransformLoadedClasses(Instrumentation inst) {
       Class<?>[] loaded = inst.getAllLoadedClasses();
       List<Class<?>> target = new ArrayList<>();
       for (Class<?> c : loaded) {
           if (c.isArray() || c.isPrimitive() || c.isSynthetic()) continue;
           if (c.getClassLoader() == null && !dumpBootstrap) continue;
           if (classNameMatches(c.getName(), filter)) {
               target.add(c);
           }
       }
       inst.retransformClasses(target.toArray(new Class<?>[0]));
   }

限制：

- ``retransformClasses`` 不接受数组类、原始类型、primitive 类
- 某些 JVM 实现不允许 retransform 正在初始化中的类
- 大量 retransform 会触发频繁的 ``transform``\ 调用，有性能开销

.. _techniques-instrumentation-arghiding:

Agent 参数隐藏
--------------

``-javaagent`` 参数会在
``ManagementFactory.getRuntimeMXBean().getInputArguments()`` 中暴露。隐藏
方法：

在注册 ClassFileTransformer 后，retransform ``sun.management.VMManagementImpl``
\，将其 ``getVmArguments`` 方法的 Code 属性替换为过滤后的版本：

.. code-block:: java

   // 通过 ASM 或字节码工具修改 getVmArguments 的字节码
   // 删除返回列表中匹配 -javaagent:*dumper* 的条目
   // 只替换 Code 属性，不修改类的其他结构
   public byte[] transform(
       ClassLoader loader, String className, ... , byte[] classfileBuffer
   ) {
       if (!"sun/management/VMManagementImpl".equals(className))
           return null;
       // 用 ASM 读取 classfileBuffer
       // 找到 getVmArguments 方法
       // 修改其字节码，过滤掉 agent 参数
       // 返回修改后的字节码
   }

这个方案不依赖 native hook，纯 Java 层实现。

.. _techniques-instrumentation-schema:

Code 属性替换与 class schema
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

只替换 Code 属性，不新增字段/方法，类的 schema 不变。这是该方案区别于
重写整个类文件的要点：JVMTI retransform 对「结构不变、仅方法体替换」的
变换容忍度最高，新增成员会引入更严格的约束与失败面。

与 JVMTI 的差异对比
-------------------

.. list-table::
   :header-rows: 1

   * -
     - Instrumentation
     - JVMTI
   * - 语言
     - Java
     - C/C++
   * - 捕获时机
     - ClassFileTransformer.transform
     - ClassFileLoadHook 回调
   * - Hidden Class
     - 不可见
     - 部分版本支持或需 native hook
   * - 文件 I/O
     - 直接写入（JVM 类锁外）
     - 需无锁队列 + writer 线程
   * - 参数隐藏
     - 字节码变换（纯 Java）
     - 字节码变换或 native hook
   * - 进程注入
     - 不支持（agent 必须预置）
     - 支持远程注入 DLL
   * - 可观测
     - ``getInputArguments()`` 暴露 agent
     - DLL 可被 PEB 枚举

Instrumentation 适合轻量级、不需要对抗复杂运行时的 dump 场景。对需要隐藏
agent 存在、捕获 hidden class 的场景，需要用 JVMTI 或 native hook 补充。
