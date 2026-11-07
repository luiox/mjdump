-- jdumper 构建根：三个 target
--   jdserver          JVMTI Agent DLL（P0 -agentpath / P1 远程注入共用入口）
--   jdumper           CLI（list/collect/inject/dump/inspect/classes/sessions/sweep/pack/status）
--   jdumper-unittest  arg_hider 畸形常量池容错回归（gtest，--with_tests=y 时启用）
--
-- 依赖注册（家族同款口径）：工程级 add_repositories，数据克隆进本工程
-- .xmake/<plat>/<arch>/repositories，不依赖机器全局注册状态。
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")

-- JAVA_HOME 必须先设置：jdserver/jdumper/jdumper-unittest 都要消费
-- jni.h / jvmti.h（jninfra 公开头会传递包含这两个 JVM 头）。
--
-- 构建与验证命令见 doc:user-guide-build 与 doc:design-validation（Sphinx 站）。

set_languages("c++17")
add_rules("mode.debug", "mode.release")
-- 全线静态 CRT：mjbase 源仓 set_runtimes("MT") 硬性口径，包图与 target
-- 必须同一 runtime，混链 LNK2038。
set_runtimes("MT")

-- libca 版本/模块单源：本仓直接 require 与嵌套归并共用同一字面量，避免
-- 同一依赖图出现两个 libca 实例（conflict configs dependencies / 链接行
-- 多条 -libpath 均是它的症状）。
LUIOX_LIBCA_VERSION = "0.0.16"
LUIOX_LIBCA_MODULES =
    "core,crypto,str,fs,env,io,json,net,http,process,opt,thread,log,zip,resources,i18n,test"
add_requires("libjninfra 0.0.2")
add_requires("mjbase 0.0.19")
add_requires("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})
-- 嵌套边镜像钉定：mjbase/libjhasm 的 libjbc 边是裸下界，不镜像会与消费
-- 图的 libjbc 实例分裂。
add_requireconfs("mjbase.libjbc", {version = "0.4.26"})
add_requireconfs("libjhasm.libjbc", {version = "0.4.26"})
-- sqlite3 边：包面 on_load 声明的 shared=false 在 requireconf 传导时会
-- 丢失，显式钉回静态口径。
add_requireconfs("mjbase.sqlite3", {version = "3.53.0", configs = {shared = false}})
-- libjstd/libjhasm 边：包面是裸下界，钉成精确版让已装实例直接命中。
add_requireconfs("mjbase.libjstd", {version = "0.0.1"})
add_requireconfs("mjbase.libjhasm", {version = "0.3.3"})
-- 家族各包自声明的 libca 模块集归并到本仓实例。
add_requireconfs("**.libca", {override = true, configs = {modules = LUIOX_LIBCA_MODULES}})

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
        add_links("kernel32", "delayimp", "jvm")
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
