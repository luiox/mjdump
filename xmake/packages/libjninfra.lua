-- libjninfra 包定义（项目内联定义）
--
-- 为什么项目内联（而不是等 luiox-repo 收编）：libjninfra 尚处于 0.0.x 快照
-- 期，还未在 luiox-repo 注册同名定义；本仓是它的首个独立消费方，先把 0.0.1
-- 基线以项目内联形态钉住。项目内联的 package() 定义优先于一切包仓，后续
-- luiox-repo 收编同名定义后，本文件整体退役，届时这里只保留 add_versions
-- 之上的注释存档。
--
-- 机制同 libjbc：add_urls 指向 git 仓库 + add_versions 第二参数锁 commit。
-- xmake 用系统 git 克隆并 checkout 到锁定 commit，随后在本包沙箱内独立构
-- 建安装（不进消费工程的 target 图）。0.0.1 的 commit 占位符由首次发版时
-- 回填真实值。
--
-- 发版/升级：在 libjninfra 仓库定版后，于此文件 add_versions 追加新版本条
-- 目（既有条目只追加不修改，见家族可复现纪律）；消费项目自行择机升级。
--
-- 链接说明：整包单库（links = libjninfra）。公开头为 C++17（jninfra 的
-- result/容器原语被 jdserver/jdumper 直接消费），on_load 统一传导语言标准，
-- windows 下同时传导 /utf-8（公开头含 UTF-8 中文注释，GBK ACP 机器上
-- on_test 探针只吃包元数据的 cxxflags，不同步 /utf-8 会注释吞行致语法错）。
--
-- 许可：MIT。

package("libjninfra")
    set_homepage("https://github.com/luiox/libjninfra")
    set_description("JVMTI/HotSpot runtime primitives: class filter, retransform sweep, dump sinks, VMStructs snapshot (git direct package)")
    set_license("MIT")

    add_urls("https://github.com/luiox/libjninfra.git")

    -- 语义版本名 + 锁定 commit。版本条目只追加、不修改。
    -- 0.0.1 = 首个独立仓基线（dump/hotspot/jni/jvmti 四组原语）。
    add_versions("0.0.1", "LIBJNINFRA_COMMIT_PLACEHOLDER") -- 0.0.1 基线（commit 待回填）

    on_load(function (package)
        package:add("links", "libjninfra")
        -- 语言标准传导：公开头使用 C++17。
        if package:is_plat("windows") then
            package:add("cxxflags", "/std:c++17")
            -- 公开头含 UTF-8 中文注释，消费方编译期（含本包 on_test 探针）
            -- 必须按 UTF-8 读源，GBK ACP 下否则注释吞行致语法错。
            package:add("cxxflags", "/utf-8")
        else
            package:add("cxxflags", "-std=c++17")
        end
    end)

    on_install("windows", "linux", "macosx", function (package)
        -- 包安装只装库本体：unittest 均为开发态目标。
        import("package.tools.xmake").install(package, {with_tests = false})
    end)

    on_test(function (package)
        assert(package:has_cxxincludes("jninfra/error.hpp", {configs = {languages = "cxx17"}}))
    end)
package_end()
