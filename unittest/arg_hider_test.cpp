// arg_hider ClassPatchReader 畸形常量池容错回归。
//
// 背景：ClassPatchReader 解析 sun/management/VMManagementImpl 常量池时，
// Fieldref 条目的 class_index 可能指向非 CLASS 条目（如 Utf8）或越界。
// 此前的 findRef 直接把 className() 结果喂给 strcmp，畸形输入下触发
// 空指针解引用；修复后 className() 对非法索引返回 nullptr 并跳过该条目。
// 本文件固化这两种畸形形态的拒绝行为。
//
// ClassPatchReader 在 arg_hider.cpp 匿名命名空间内，经公开入口
// hideAgentInputArguments 触发：jvmti 指针只在补丁成功路径被解引用
// （Allocate），畸形用例走不到，传哑指针即可。

#include "arg_hider.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message)
{
    if (condition) {
        return true;
    }
    std::cerr << "expectation failed: " << message << std::endl;
    return false;
}

void appendU1(std::vector<unsigned char>& out, uint8_t value)
{
    out.push_back(value);
}

void appendU2(std::vector<unsigned char>& out, uint16_t value)
{
    out.push_back(static_cast<unsigned char>(value >> 8U));
    out.push_back(static_cast<unsigned char>(value));
}

void appendU4(std::vector<unsigned char>& out, uint32_t value)
{
    out.push_back(static_cast<unsigned char>(value >> 24U));
    out.push_back(static_cast<unsigned char>(value >> 16U));
    out.push_back(static_cast<unsigned char>(value >> 8U));
    out.push_back(static_cast<unsigned char>(value));
}

void appendUtf8(std::vector<unsigned char>& out, const std::string& text)
{
    appendU2(out, static_cast<uint16_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

// 最小 VMManagementImpl：结构合法（parse 通过），但 Fieldref 条目的 class_index
// 由调用方给定——指向 Utf8 条目或越界索引即目标容错回归的两种畸形形态。
std::vector<unsigned char> buildVmManagementImplClass(uint16_t fieldref_class_index)
{
    std::vector<unsigned char> out;
    appendU4(out, 0xCAFEBABEU);
    appendU2(out, 0U);    // minor_version
    appendU2(out, 52U);   // major_version
    appendU2(out, 10U);   // constant_pool_count（条目 1..9）
    appendU1(out, 1U);
    appendUtf8(out, "sun/management/VMManagementImpl");          // 1
    appendU1(out, 7U);
    appendU2(out, 1U);                                           // 2 Class
    appendU1(out, 1U);
    appendUtf8(out, "java/lang/Object");                         // 3
    appendU1(out, 7U);
    appendU2(out, 3U);                                           // 4 Class
    appendU1(out, 1U);
    appendUtf8(out, "definitely/not/a/class");                   // 5 Utf8（非 CLASS）
    appendU1(out, 9U);
    appendU2(out, fieldref_class_index);
    appendU2(out, 7U);                                           // 6 Fieldref
    appendU1(out, 12U);
    appendU2(out, 8U);
    appendU2(out, 9U);                                           // 7 NameAndType
    appendU1(out, 1U);
    appendUtf8(out, "vmArgs");                                   // 8
    appendU1(out, 1U);
    appendUtf8(out, "Ljava/util/List;");                         // 9
    appendU2(out, 0x0021U);   // access_flags
    appendU2(out, 2U);        // this_class
    appendU2(out, 4U);        // super_class
    appendU2(out, 0U);        // interfaces_count
    appendU2(out, 0U);        // fields_count
    appendU2(out, 1U);        // methods_count
    appendU2(out, 0x0001U);   // method access_flags
    appendU2(out, 8U);        // method name_index
    appendU2(out, 9U);        // method descriptor_index
    appendU2(out, 0U);        // method attributes_count
    appendU2(out, 0U);        // class attributes_count
    return out;
}

bool refuseMalformed(uint16_t fieldref_class_index, const char* scenario)
{
    // 畸形用例永远到不了 Allocate，jvmti 只需非空。
    static int dummy_env_storage = 0;
    auto*      fake_jvmti        = reinterpret_cast<jvmtiEnv*>(&dummy_env_storage);

    std::vector<unsigned char> bytes = buildVmManagementImplClass(fieldref_class_index);
    jint                       new_len  = 0;
    unsigned char*             new_data = nullptr;
    const bool                 patched =
        hideAgentInputArguments(fake_jvmti,
                                "sun/management/VMManagementImpl",
                                static_cast<jint>(bytes.size()),
                                bytes.data(),
                                &new_len,
                                &new_data);
    return expect(!patched, scenario);
}

}   // namespace

int main()
{
    int failures = 0;

    // 畸形用例 1：Fieldref 的 class_index 指向 Utf8 条目（非 CLASS）。
    if (!refuseMalformed(5U, "fieldref class_index -> utf8 entry should refuse to patch")) {
        ++failures;
    }

    // 畸形用例 2：class_index 越界（>= constant_pool_count）。
    if (!refuseMalformed(250U, "fieldref class_index out of range should refuse to patch")) {
        ++failures;
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "arg_hider_unittest: passed" << std::endl;
    return 0;
}
