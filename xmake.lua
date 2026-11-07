-- jdumper 构建根：三个 target
--   jdserver          JVMTI Agent DLL（P0 -agentpath / P1 远程注入共用入口）
--   jdumper           CLI（list/collect/inject/dump/inspect/classes/sessions/sweep/pack/status）
--   jdumper-unittest  arg_hider 畸形常量池容错回归（gtest，--with_tests=y 时启用）
--
-- 依赖注册前置（本文件不声明 add_repositories，包沙箱继承宿主注册，同 libjzip
-- 口径）：独立开发/CI 先执行
--   xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
-- luiox-repo 提供 libca 与 mjbase（git 直连包）；libjninfra 走本仓项目内联包
-- 定义（xmake/packages/libjninfra.lua）。
--
-- JAVA_HOME 必须先设置：jdserver/jdumper/jdumper-unittest 都要消费
-- jni.h / jvmti.h（jninfra 公开头会传递包含这两个 JVM 头）。
--
-- 构建与验证命令见 doc:user-guide-build 与 doc:design-validation（Sphinx 站）。

set_languages("c++17")
add_rules("mode.debug", "mode.release")

-- 项目内联包须先于 add_requires 注册：先 includes 再 add_requires。
includes("xmake/packages/libjninfra.lua")
add_requires("libjninfra 0.0.1")
add_requires("mjbase 0.0.12")
add_requires("libca 0.0.16", {configs = {modules = "core,fs,log,test"}})

option("with_tests")
    set_default(false)
    set_showmenu(true)
    set_description("build jdumper-unittest (requires gtest)")
option_end()

if has_config("with_tests") then
    add_requires("gtest")
end

if not os.getenv("JAVA_HOME") then
    raise("JAVA_HOME is not set. Required for jni.h/jvmti.h (jdserver / jdumper / jdumper-unittest).")
end
local java_include        = path.join(os.getenv("JAVA_HOME"), "include")
local java_include_win32  = path.join(java_include, "win32")
local java_lib_dir        = path.join(os.getenv("JAVA_HOME"), "lib")

target("jdserver")
    set_kind("shared")
    set_group("jdumper")
    set_languages("c++17")
    set_warnings("all")

    add_includedirs(java_include, java_include_win32)
    add_packages("libjninfra", "libca")
    add_files("dserver/*.cpp")

    if is_plat("windows") then
        add_cxflags("/EHa", "/utf-8", {tools = "cl"})
        add_links("kernel32", "delayimp")
        -- jvm.lib 只用于延迟加载符号解析；运行期 jvm.dll 已随 JVM 进程存在。
        add_linkdirs(java_lib_dir)
        add_shflags("/DELAYLOAD:jvm.dll", {tools = "link"})
    end
target_end()

target("jdumper")
    set_kind("binary")
    set_group("jdumper")
    set_languages("c++17")
    set_warnings("all")

    -- JAVA_HOME include 也要加：消费 jninfra 公开头会传递包含 jni.h/jvmti.h。
    add_includedirs(java_include, java_include_win32)
    add_includedirs("dump")
    add_packages("libjninfra", "mjbase", "libca")
    add_files("dump/*.cpp")

    if is_plat("windows") then
        add_cxflags("/EHa", "/utf-8", {tools = "cl"})
        add_links("kernel32", "advapi32", "bcrypt")
    end
target_end()

target("jdumper-unittest")
    set_kind("binary")
    set_group("jdumper/test")
    set_default(false)
    set_languages("c++17")
    set_warnings("all")

    add_packages("gtest")
    -- jvmti.h 与 dserver 同源（JAVA_HOME），无 JVM 链接依赖（仅类型）。
    add_includedirs(java_include, java_include_win32)
    add_includedirs("dserver")
    add_files("dserver/arg_hider.cpp")
    add_files("unittest/arg_hider_test.cpp")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
target_end()
