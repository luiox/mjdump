/// @defgroup jdserver-arg-hider Agent 参数隐藏（P0.5）
///
/// P0.5 通过 JVMTI class retransformation 隐藏 Java 可见的 agent 启动参数。
///
/// ## 策略
///
/// 早期的 native hook 方案针对 HotSpot `JVM_GetVmArguments`，但新版 JVM 在该
/// 入口点周围使用 RIP-relative 指令，trampoline hook 不可靠（native hook 的
/// 适用边界与风险见 doc:techniques-native-hook），故改用 JVMTI class
/// retransformation：
///
/// 1. `jdserver` 启用 `can_retransform_classes` 和
///    `can_retransform_any_class`
/// 2. `VMInit` 仅在 management 实现已加载时 retransform；不强制早期 bootstrap 加载
/// 3. `ClassFileLoadHook` 检测到目标 class 时，patch 其 Java `getVmArguments()` wrapper
/// 4. Patched wrapper 去掉类似 jdserver 自己 `-agentpath`/`-agentlib` 的启动参数
///
/// 这保持在正常 JVMTI 转换规则内，避免在 `jvm.dll` 中 patch 可执行页。
/// 纯 Java 层（Instrumentation transformer）的等价方案对照见
/// doc:techniques-instrumentation-arghiding。
///
/// ## MVP 边界
///
/// 只移除包含 agent 启动前缀且含 `jdserver` 的参数。
/// VM options（`-Xmx`、`--add-opens`、`-D` 属性）保持可见。
/// 行为由 `hideAgentArgs` 控制，默认为 `true`。
///
/// ## 失效边界（结构性，retransform 无法覆盖）
///
/// 1. 替换字节码保留 `vmArgs` 缓存字段的命中快路径：目标 JVM 在补丁生效前
///    已调用过 `getInputArguments()`（live-attach 到开了 JMX 监控的长运行
///    JVM）时字段已填充，补丁后的 getter 仍返回缓存中的未过滤列表，
///    对该 JVM 实例永久无效。P0 `-agentpath` 启动在类首次加载时补丁，
///    不受此限制。
/// 2. 只改写后续调用：此前已被监控端/日志读出并缓存的参数列表不可撤回；
///    live-attach 存在 attach 到 retransform 完成之间的窗口，窗口内
///    返回未过滤结果。
///
/// 失效边界的完整用户视角描述见 doc:user-guide-hideagentargs。
///
/// ## 实现说明
///
/// transformer 只替换 `getVmArguments()` Code 属性并追加新字节码所需的常量池条目。
/// 不添加方法或字段，retransformation 不改变 class schema。放弃 native hook
/// 路线的完整边界论证（可移植性、Hidden Class 盲区、覆盖面）见
/// doc:techniques-native-hook-boundary。

#pragma once

#include <jvmti.h>

/// @brief Patch HotSpot management bytecode so Java-side VM arguments are hidden.
///
/// The P0.5 implementation targets `sun/management/VMManagementImpl` and rewrites
/// its `getVmArguments()` Java wrapper to remove only jdserver's own
/// `-agentpath` / `-agentlib` entry. This is intentionally class-file based
/// instead of an inline native hook because modern HotSpot builds use
/// RIP-relative instructions around `JVM_GetVmArguments`.
///
/// The function is safe to call from `ClassFileLoadHook`: it only inspects the
/// supplied class bytes and, when a patch is possible, allocates replacement bytes
/// with `jvmtiEnv::Allocate` as required by JVMTI.
///
/// @param jvmti JVMTI environment used for replacement byte allocation.
/// @param class_name Slash-form class name from `ClassFileLoadHook`.
/// @param class_data_len Original class-file length.
/// @param class_data Original class-file bytes.
/// @param new_class_data_len Output length for transformed class bytes.
/// @param new_class_data Output pointer for transformed class bytes.
/// @return true when replacement bytes were produced; false when the class is not
///         the target class or the class shape is unsupported.
bool hideAgentInputArguments(jvmtiEnv* jvmti, const char* class_name, jint class_data_len,
                             const unsigned char* class_data, jint* new_class_data_len,
                             unsigned char** new_class_data);
