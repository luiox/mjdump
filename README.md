# jdumper — JVM 运行时字节码 Dumper

> 三模式 JVM 字节码运行时导出工具，支持 **JVMTI Agent**（P0）、**远程注入**（P1）、**冻结外部读取**（P2）三种方案。

## 架构

```
jdumper.exe (CLI)
├── collect   ← 遗留 pipe 接收器（agent 侧无生产者，见下）
├── inject    ← 远程注入 jdserver.dll（P1）
├── inspect   ← dump 前预览 loaded classes 和过滤命中
├── dump      ← 默认注入 agent 执行真实 dump；--preflight 保留 P2 预检
├── pack      ← 将 dump 目录打包成可被其它工具消费的 JAR
└── status    ← 打印 dump session 元数据

jdserver.dll (Agent DLL)
├── Agent_OnLoad → JVMTI ClassFileLoadHook（直写文件，无 pipe 通道）
├── VMInit       → 已加载类 RetransformClasses 兜底
└── DllMain      → 注入模式入口（P1）
```

## 已知限制

- **首载类 module 恒为 `unknown`**：`ClassFileLoadHook` 对 agent 启动后新加载的类拿不到 module（`class_being_redefined == NULL`）。`--group-by module` 下这些类进`_by_module/unknown/`；`--module` 精确过滤对它们恒不命中（fail-closed 跳过）。只有 VMInit sweep 触达的已加载类带真实 module。
- **`collect` 是死路径**：agent 从不向 named pipe 发送数据（`writeClass` 直写文件），该命令仅作历史占位保留。
- **`hideAgentArgs` 对 vmArgs 已填充的 JVM 无效**：补丁保留 `vmArgs` 缓存字段的命中快路径，attach 前已调用过 `getInputArguments()` 的 JVM 会持续返回未过滤列表；详见 `doc/user_guide.rst` 的 hideAgentArgs 边界。

## 快速使用

```bash
# P0：-agentpath 启动
java -agentpath:build\windows\x64\release\jdserver.dll -jar app.jar

# P1：远程注入
jdumper inject -p <pid>

# dump 前预览
jdumper inspect -p <pid> --package com.example --limit 100

# P1：默认 dump
jdumper dump -p <pid> -o C:\Dump --package com.example

# 离线分析：dump 目录保持 class 文件树布局，可被任意 class 文件工具直接读取；
# 需要只接受 JAR 的工具时先打包
jdumper pack -i C:\Dump -o C:\Dump\dumped.jar
```

## 构建

```bash
set JAVA_HOME=D:\sdk\java\jdk17
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f -m release --with_tests=y -y
xmake build jdserver
xmake build jdumper
```

依赖注册、JAVA_HOME 要求与包说明见 `xmake.lua` 头注释；详细用法见 `doc/user_guide.rst`。

## 文档

- `doc/index.rst`：设计规格入口（Sphinx 构建；`design.rst` / `cli.rst` / `user_guide.rst` / 技术 Wiki）
- `doc/Doxyfile`：微观契约层（头文件 `///` 注释）
- `doc/roadmap.md`：产品方向路线图
- `doc/user_guide.rst`：能力一览、构建与验证
