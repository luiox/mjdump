/// @defgroup mjserver-inspect 已加载类预览（inspect 模式）
///
/// 只读枚举 JVMTI loaded-class 列表并写出 `_inspect.json`：类名、loader
/// 身份/父链、module、当前过滤命中与预计输出路径，不触发
/// RetransformClasses、不写 `.class`。module/loader 在非 retransform 路径
/// 上不可得的 fail-closed 语义见 doc:cli-filtering-sessions；`--view`
/// /`--limit` 的用法见 doc:user-guide-dump。

#pragma once

#include <jni.h>

/// @brief 枚举当前 JVM 已加载类并写出 inspect 预览文件。
///
/// 该路径不触发 RetransformClasses,只读取 JVMTI loaded-class 列表,用于
/// `mjdump inspect` 在正式 dump 前预览过滤命中范围。
///
/// @param env 已附加到目标 JVM 的 JNI 环境。
void inspectLoadedClasses(JNIEnv* env);
