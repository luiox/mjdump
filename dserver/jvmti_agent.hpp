/// @defgroup jdserver-jvmti-agent JVMTI agent 生命周期与回调注册
///
/// jdserver 的 JVMTI 初始化是 `-agentpath`（Agent_OnLoad）与远程注入
/// （控制入口）共用的同一条路径：协商 retransform capabilities、注册
/// ClassFileLoadHook / VMInit、再对已加载类做 retransform sweep。两种入口
/// 的分工与竞态守卫（isAgentStarted）是本模块的核心契约；三模式（P0/P1/P2）
/// 的整体关系见 doc:design-modes。
///
/// 回调运行时序与锁约束的技术背景见 doc:techniques-jvmti-dump-callback。

#pragma once
#include <jvmti.h>
#include <jni.h>

/// @brief Initialize JVMTI capabilities and callbacks for class dumping.
///
/// This is the common setup used by both `Agent_OnLoad` and remote injection.
/// It registers ClassFileLoadHook for future class bytes and VMInit for the
/// loaded-class retransform sweep.
bool initJVMTI(JavaVM* vm);

/// @brief Retransform already-loaded classes so ClassFileLoadHook can dump them.
void dumpExistingClasses(JNIEnv* env);

/// @brief 获取当前 agent 持有的 JVMTI 环境。
/// @return 已初始化时返回借用指针;未初始化时返回 null。
jvmtiEnv* getJvmtiEnv();

/// @brief Return whether this DLL has already initialized its JVMTI agent.
///
/// `DllMain` also runs during normal `-agentpath` loading. The guard prevents
/// the delayed injection worker from racing `Agent_OnLoad` and losing explicit
/// agent options.
bool isAgentStarted() noexcept;

/// @brief Parse options and start the in-process dumping agent once.
jint initAndRun(JavaVM* vm, char* options);
