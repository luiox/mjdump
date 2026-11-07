#include "arg_hider.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint8_t kCpUtf8               = 1;
constexpr uint8_t kCpInteger            = 3;
constexpr uint8_t kCpFloat              = 4;
constexpr uint8_t kCpLong               = 5;
constexpr uint8_t kCpDouble             = 6;
constexpr uint8_t kCpClass              = 7;
constexpr uint8_t kCpString             = 8;
constexpr uint8_t kCpFieldref           = 9;
constexpr uint8_t kCpMethodref          = 10;
constexpr uint8_t kCpInterfaceMethodref = 11;
constexpr uint8_t kCpNameAndType        = 12;
constexpr uint8_t kCpMethodHandle       = 15;
constexpr uint8_t kCpMethodType         = 16;
constexpr uint8_t kCpDynamic            = 17;
constexpr uint8_t kCpInvokeDynamic      = 18;
constexpr uint8_t kCpModule             = 19;
constexpr uint8_t kCpPackage            = 20;

struct CpEntry
{
    uint8_t     tag = 0;
    uint16_t    a   = 0;
    uint16_t    b   = 0;
    std::string text;
};

struct MethodAttr
{
    size_t      start      = 0;
    size_t      end        = 0;
    uint16_t    name_index = 0;
    const char* name       = nullptr;
};

struct MethodInfo
{
    size_t                  start            = 0;
    size_t                  attrs_start      = 0;
    size_t                  end              = 0;
    uint16_t                name_index       = 0;
    uint16_t                descriptor_index = 0;
    std::vector<MethodAttr> attrs;
};

void appendU1(std::vector<unsigned char>& output, uint8_t value)
{
    output.push_back(value);
}

void appendU2(std::vector<unsigned char>& output, uint16_t value)
{
    output.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<unsigned char>(value & 0xFFU));
}

void appendU4(std::vector<unsigned char>& output, uint32_t value)
{
    output.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
    output.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    output.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<unsigned char>(value & 0xFFU));
}

class ConstantPoolPatch
{
public:
    explicit ConstantPoolPatch(uint16_t original_count)
        : next_index_(original_count)
    {}

    uint16_t nextIndex() const { return next_index_; }

    const std::vector<unsigned char>& bytes() const { return bytes_; }

    uint16_t addUtf8(const char* text)
    {
        const auto     length = static_cast<uint16_t>(std::strlen(text));
        const uint16_t index  = next_index_++;
        appendU1(bytes_, kCpUtf8);
        appendU2(bytes_, length);
        bytes_.insert(bytes_.end(), text, text + length);
        return index;
    }

    uint16_t addClass(const char* name)
    {
        const uint16_t name_index = addUtf8(name);
        const uint16_t index      = next_index_++;
        appendU1(bytes_, kCpClass);
        appendU2(bytes_, name_index);
        return index;
    }

    uint16_t addString(const char* text)
    {
        const uint16_t text_index = addUtf8(text);
        const uint16_t index      = next_index_++;
        appendU1(bytes_, kCpString);
        appendU2(bytes_, text_index);
        return index;
    }

    uint16_t addNameAndType(const char* name, const char* descriptor)
    {
        const uint16_t name_index       = addUtf8(name);
        const uint16_t descriptor_index = addUtf8(descriptor);
        const uint16_t index            = next_index_++;
        appendU1(bytes_, kCpNameAndType);
        appendU2(bytes_, name_index);
        appendU2(bytes_, descriptor_index);
        return index;
    }

    uint16_t addMethodref(uint16_t owner_class, const char* name, const char* descriptor)
    {
        const uint16_t name_type = addNameAndType(name, descriptor);
        const uint16_t index     = next_index_++;
        appendU1(bytes_, kCpMethodref);
        appendU2(bytes_, owner_class);
        appendU2(bytes_, name_type);
        return index;
    }

private:
    uint16_t                   next_index_ = 0;
    std::vector<unsigned char> bytes_;
};

class ClassPatchReader
{
public:
    ClassPatchReader(const unsigned char* bytes, size_t size)
        : bytes_(bytes)
        , size_(size)
    {}

    bool parse()
    {
        size_t cursor = 0;
        if (!skipU4(cursor)) return false;
        if (!skipU2(cursor)) return false;
        if (!skipU2(cursor)) return false;

        cp_count_offset_  = cursor;
        uint16_t cp_count = 0;
        if (!readU2(cursor, cp_count)) return false;
        original_cp_count_ = cp_count;
        cp_.assign(cp_count, CpEntry{});

        for (uint16_t index = 1; index < cp_count; ++index) {
            uint8_t tag = 0;
            if (!readU1(cursor, tag)) return false;
            cp_[index].tag = tag;

            switch (tag) {
            case kCpUtf8:
            {
                uint16_t length = 0;
                if (!readU2(cursor, length) || !has(cursor, length)) return false;
                cp_[index].text.assign(reinterpret_cast<const char*>(bytes_ + cursor), length);
                cursor += length;
                break;
            }
            case kCpInteger:
            case kCpFloat:
                if (!skip(cursor, 4)) return false;
                break;
            case kCpLong:
            case kCpDouble:
                if (!skip(cursor, 8)) return false;
                ++index;
                break;
            case kCpClass:
            case kCpString:
            case kCpMethodType:
            case kCpModule:
            case kCpPackage:
                if (!readU2(cursor, cp_[index].a)) return false;
                break;
            case kCpFieldref:
            case kCpMethodref:
            case kCpInterfaceMethodref:
            case kCpNameAndType:
            case kCpDynamic:
            case kCpInvokeDynamic:
                if (!readU2(cursor, cp_[index].a) || !readU2(cursor, cp_[index].b)) {
                    return false;
                }
                break;
            case kCpMethodHandle:
                if (!skip(cursor, 3)) return false;
                break;
            default: return false;
            }
        }
        cp_end_offset_ = cursor;

        if (!skipU2(cursor)) return false;
        uint16_t this_class = 0;
        if (!readU2(cursor, this_class)) return false;
        this_class_index_ = this_class;
        this_class_name_  = className(this_class);
        if (!skipU2(cursor)) return false;

        uint16_t interfaces_count = 0;
        if (!readU2(cursor, interfaces_count) || !skip(cursor, interfaces_count * 2U)) return false;
        if (!skipMembers(cursor)) return false;

        methods_count_offset_  = cursor;
        uint16_t methods_count = 0;
        if (!readU2(cursor, methods_count)) return false;
        for (uint16_t index = 0; index < methods_count; ++index) {
            MethodInfo method;
            if (!parseMethod(cursor, method)) return false;
            methods_.push_back(method);
        }
        methods_end_offset_ = cursor;
        return true;
    }

    bool patchGetVmArguments(std::vector<unsigned char>& output) const
    {
        const uint16_t vm_args_field =
            findRef(kCpFieldref, this_class_name_, "vmArgs", "Ljava/util/List;");
        const uint16_t get_args0 =
            findRef(kCpMethodref, this_class_name_, "getVmArguments0", "()[Ljava/lang/String;");
        const uint16_t empty_list =
            findRef(kCpMethodref, "java/util/Collections", "emptyList", "()Ljava/util/List;");
        const uint16_t unmodifiable_list = findRef(kCpMethodref,
                                                   "java/util/Collections",
                                                   "unmodifiableList",
                                                   "(Ljava/util/List;)Ljava/util/List;");

        if (vm_args_field == 0 || get_args0 == 0 || empty_list == 0 || unmodifiable_list == 0 ||
            cp_count_offset_ == 0 || cp_end_offset_ == 0 || methods_count_offset_ == 0 ||
            methods_end_offset_ == 0) {
            return false;
        }

        ConstantPoolPatch cp_patch(original_cp_count_);
        const uint16_t    array_list_class = cp_patch.addClass("java/util/ArrayList");
        const uint16_t array_list_init = cp_patch.addMethodref(array_list_class, "<init>", "(I)V");
        const uint16_t array_list_add =
            cp_patch.addMethodref(array_list_class, "add", "(Ljava/lang/Object;)Z");
        const uint16_t string_class = cp_patch.addClass("java/lang/String");
        const uint16_t string_index_of =
            cp_patch.addMethodref(string_class, "indexOf", "(Ljava/lang/String;)I");
        const uint16_t string_array_class   = cp_patch.addClass("[Ljava/lang/String;");
        const uint16_t agentpath_string     = cp_patch.addString("-agentpath:");
        const uint16_t agentlib_string      = cp_patch.addString("-agentlib:");
        const uint16_t jdserver_string      = cp_patch.addString("jdserver");
        const uint16_t stack_map_table_utf8 = cp_patch.addUtf8("StackMapTable");

        bool patched = false;
        output.clear();
        appendSlice(output, 0, cp_count_offset_);
        appendU2(output, cp_patch.nextIndex());
        appendSlice(output, cp_count_offset_ + 2U, cp_end_offset_);
        const auto& cp_bytes = cp_patch.bytes();
        output.insert(output.end(), cp_bytes.begin(), cp_bytes.end());
        appendSlice(output, cp_end_offset_, methods_count_offset_);
        appendU2(output, static_cast<uint16_t>(methods_.size()));
        for (const MethodInfo& method : methods_) {
            if (!isTargetGetVmArguments(method)) {
                appendSlice(output, method.start, method.end);
                continue;
            }

            bool replaced_code = false;
            // 只替换 Code 属性：不新增方法/字段，retransformation 不改变
            // class schema（与纯 Java transformer 方案的对照见
            // doc:techniques-instrumentation-schema）。
            appendSlice(output, method.start, method.attrs_start);
            for (const MethodAttr& attr : method.attrs) {
                if (attr.name != nullptr && std::strcmp(attr.name, "Code") == 0) {
                    writeCodeAttribute(output,
                                       attr.name_index,
                                       stack_map_table_utf8,
                                       vm_args_field,
                                       get_args0,
                                       array_list_class,
                                       array_list_init,
                                       array_list_add,
                                       string_index_of,
                                       agentpath_string,
                                       agentlib_string,
                                       jdserver_string,
                                       unmodifiable_list,
                                       empty_list,
                                       string_array_class,
                                       string_class);
                    replaced_code = true;
                    patched       = true;
                }
                else {
                    appendSlice(output, attr.start, attr.end);
                }
            }
            if (!replaced_code) {
                return false;
            }
        }
        appendSlice(output, methods_end_offset_, size_);
        return patched;
    }

private:
    bool parseMethod(size_t& cursor, MethodInfo& method)
    {
        method.start = cursor;
        if (!skipU2(cursor)) return false;
        if (!readU2(cursor, method.name_index)) return false;
        if (!readU2(cursor, method.descriptor_index)) return false;

        uint16_t attributes_count = 0;
        if (!readU2(cursor, attributes_count)) return false;
        method.attrs_start = cursor;

        for (uint16_t index = 0; index < attributes_count; ++index) {
            MethodAttr attr;
            attr.start = cursor;
            if (!readU2(cursor, attr.name_index)) return false;
            uint32_t length = 0;
            if (!readU4(cursor, length) || !skip(cursor, length)) return false;
            attr.end  = cursor;
            attr.name = utf8(attr.name_index);
            method.attrs.push_back(attr);
        }
        method.end = cursor;
        return true;
    }

    bool skipMembers(size_t& cursor)
    {
        uint16_t count = 0;
        if (!readU2(cursor, count)) return false;
        for (uint16_t member = 0; member < count; ++member) {
            if (!skip(cursor, 6)) return false;
            uint16_t attributes_count = 0;
            if (!readU2(cursor, attributes_count)) return false;
            for (uint16_t index = 0; index < attributes_count; ++index) {
                if (!skipU2(cursor)) return false;
                uint32_t length = 0;
                if (!readU4(cursor, length) || !skip(cursor, length)) return false;
            }
        }
        return true;
    }

    bool isTargetGetVmArguments(const MethodInfo& method) const
    {
        const char* name = utf8(method.name_index);
        const char* desc = utf8(method.descriptor_index);
        return name != nullptr && desc != nullptr && std::strcmp(name, "getVmArguments") == 0 &&
               std::strcmp(desc, "()Ljava/util/List;") == 0;
    }

    uint16_t findRef(uint8_t tag, const char* owner, const char* name, const char* descriptor) const
    {
        if (owner == nullptr || name == nullptr || descriptor == nullptr) {
            return 0;
        }
        for (uint16_t index = 1; index < cp_.size(); ++index) {
            const CpEntry& entry = cp_[index];
            if (entry.tag != tag) {
                continue;
            }
            // className() 对越界 / tag != CLASS 返回 nullptr：畸形常量池
            // （class_index 指向非 CLASS 条目或越界）下跳过该条目，不喂 strcmp
            // （容错契约见 doc:design-failure-buckets）。
            const char* cls = className(entry.a);
            if (cls == nullptr || std::strcmp(cls, owner) != 0) {
                continue;
            }
            const CpEntry* name_type = checked(entry.b, kCpNameAndType);
            if (name_type == nullptr) {
                continue;
            }
            const char* ref_name = utf8(name_type->a);
            const char* ref_desc = utf8(name_type->b);
            if (ref_name != nullptr && ref_desc != nullptr && std::strcmp(ref_name, name) == 0 &&
                std::strcmp(ref_desc, descriptor) == 0) {
                return index;
            }
        }
        return 0;
    }

    const CpEntry* checked(uint16_t index, uint8_t tag) const
    {
        if (index == 0 || index >= cp_.size() || cp_[index].tag != tag) {
            return nullptr;
        }
        return &cp_[index];
    }

    const char* utf8(uint16_t index) const
    {
        const CpEntry* entry = checked(index, kCpUtf8);
        return entry == nullptr ? nullptr : entry->text.c_str();
    }

    const char* className(uint16_t index) const
    {
        const CpEntry* entry = checked(index, kCpClass);
        return entry == nullptr ? nullptr : utf8(entry->a);
    }

    void appendSlice(std::vector<unsigned char>& output, size_t begin, size_t end) const
    {
        if (begin < end && end <= size_) {
            output.insert(output.end(), bytes_ + begin, bytes_ + end);
        }
    }

    static void writeCodeAttribute(std::vector<unsigned char>& output, uint16_t name_index,
                                   uint16_t stack_map_table_name, uint16_t vm_args_field,
                                   uint16_t get_args0, uint16_t array_list_class,
                                   uint16_t array_list_init, uint16_t array_list_add,
                                   uint16_t string_index_of, uint16_t agentpath_string,
                                   uint16_t agentlib_string, uint16_t jdserver_string,
                                   uint16_t unmodifiable_list, uint16_t empty_list,
                                   uint16_t string_array_class, uint16_t string_class)
    {
        appendU2(output, name_index);
        appendU4(output, 167U);
        appendU2(output, 3U);
        appendU2(output, 5U);
        appendU4(output, 118U);

        // Java equivalent:
        // raw = getVmArguments0(); filtered = new ArrayList(raw.length);
        // keep each arg unless it is this agent's -agentpath/-agentlib entry.
        appendU1(output, 0x2A);
        appendU1(output, 0xB4);
        appendU2(output, vm_args_field);
        appendU1(output, 0xC7);
        appendU2(output, 109U);
        appendU1(output, 0x2A);
        appendU1(output, 0xB6);
        appendU2(output, get_args0);
        appendU1(output, 0x4C);
        appendU1(output, 0x2B);
        appendU1(output, 0xC6);
        appendU2(output, 93U);
        appendU1(output, 0x2B);
        appendU1(output, 0xBE);
        appendU1(output, 0x99);
        appendU2(output, 88U);
        appendU1(output, 0xBB);
        appendU2(output, array_list_class);
        appendU1(output, 0x59);
        appendU1(output, 0x2B);
        appendU1(output, 0xBE);
        appendU1(output, 0xB7);
        appendU2(output, array_list_init);
        appendU1(output, 0x4D);
        appendU1(output, 0x03);
        appendU1(output, 0x3E);
        appendU1(output, 0x1D);
        appendU1(output, 0x2B);
        appendU1(output, 0xBE);
        appendU1(output, 0xA2);
        appendU2(output, 59U);
        appendU1(output, 0x2B);
        appendU1(output, 0x1D);
        appendU1(output, 0x32);
        appendU1(output, 0x3A);
        appendU1(output, 0x04);
        appendU1(output, 0x19);
        appendU1(output, 0x04);
        appendU1(output, 0xC6);
        appendU2(output, 36U);
        appendU1(output, 0x19);
        appendU1(output, 0x04);
        appendU1(output, 0x13);
        appendU2(output, agentpath_string);
        appendU1(output, 0xB6);
        appendU2(output, string_index_of);
        appendU1(output, 0x9C);
        appendU2(output, 14U);
        appendU1(output, 0x19);
        appendU1(output, 0x04);
        appendU1(output, 0x13);
        appendU2(output, agentlib_string);
        appendU1(output, 0xB6);
        appendU2(output, string_index_of);
        appendU1(output, 0x9B);
        appendU2(output, 14U);
        appendU1(output, 0x19);
        appendU1(output, 0x04);
        appendU1(output, 0x13);
        appendU2(output, jdserver_string);
        appendU1(output, 0xB6);
        appendU2(output, string_index_of);
        appendU1(output, 0x9C);
        appendU2(output, 10U);
        appendU1(output, 0x2C);
        appendU1(output, 0x19);
        appendU1(output, 0x04);
        appendU1(output, 0xB6);
        appendU2(output, array_list_add);
        appendU1(output, 0x57);
        appendU1(output, 0x84);
        appendU1(output, 0x03);
        appendU1(output, 0x01);
        appendU1(output, 0xA7);
        appendU2(output, 0xFFC5U);
        appendU1(output, 0x2A);
        appendU1(output, 0x2C);
        appendU1(output, 0xB8);
        appendU2(output, unmodifiable_list);
        appendU1(output, 0xB5);
        appendU2(output, vm_args_field);
        appendU1(output, 0xA7);
        appendU2(output, 10U);
        appendU1(output, 0x2A);
        appendU1(output, 0xB8);
        appendU2(output, empty_list);
        appendU1(output, 0xB5);
        appendU2(output, vm_args_field);
        appendU1(output, 0x2A);
        appendU1(output, 0xB4);
        appendU2(output, vm_args_field);
        appendU1(output, 0xB0);

        appendU2(output, 0U);
        appendU2(output, 1U);
        writeStackMapTable(
            output, stack_map_table_name, string_array_class, array_list_class, string_class);
    }

    static void writeStackMapTable(std::vector<unsigned char>& output, uint16_t name_index,
                                   uint16_t string_array_class, uint16_t array_list_class,
                                   uint16_t string_class)
    {
        appendU2(output, name_index);
        appendU4(output, 31U);
        appendU2(output, 7U);

        appendU1(output, 254U);
        appendU2(output, 33U);
        appendU1(output, 7U);
        appendU2(output, string_array_class);
        appendU1(output, 7U);
        appendU2(output, array_list_class);
        appendU1(output, 1U);

        appendU1(output, 252U);
        appendU2(output, 37U);
        appendU1(output, 7U);
        appendU2(output, string_class);

        appendU1(output, 10U);

        appendU1(output, 250U);
        appendU2(output, 6U);
        appendU1(output, 250U);
        appendU2(output, 5U);
        appendU1(output, 250U);
        appendU2(output, 10U);
        appendU1(output, 250U);
        appendU2(output, 6U);
    }

    bool has(size_t cursor, size_t length) const
    {
        return cursor <= size_ && length <= size_ - cursor;
    }

    bool skip(size_t& cursor, size_t length) const
    {
        if (!has(cursor, length)) return false;
        cursor += length;
        return true;
    }

    bool readU1(size_t& cursor, uint8_t& value) const
    {
        if (!has(cursor, 1)) return false;
        value = bytes_[cursor++];
        return true;
    }

    bool readU2(size_t& cursor, uint16_t& value) const
    {
        if (!has(cursor, 2)) return false;
        value = static_cast<uint16_t>((static_cast<uint16_t>(bytes_[cursor]) << 8U) |
                                      static_cast<uint16_t>(bytes_[cursor + 1U]));
        cursor += 2;
        return true;
    }

    bool readU4(size_t& cursor, uint32_t& value) const
    {
        if (!has(cursor, 4)) return false;
        value = (static_cast<uint32_t>(bytes_[cursor]) << 24U) |
                (static_cast<uint32_t>(bytes_[cursor + 1U]) << 16U) |
                (static_cast<uint32_t>(bytes_[cursor + 2U]) << 8U) |
                static_cast<uint32_t>(bytes_[cursor + 3U]);
        cursor += 4;
        return true;
    }

    bool skipU2(size_t& cursor) const { return skip(cursor, 2); }

    bool skipU4(size_t& cursor) const { return skip(cursor, 4); }

    const unsigned char*    bytes_ = nullptr;
    size_t                  size_  = 0;
    std::vector<CpEntry>    cp_;
    const char*             this_class_name_      = nullptr;
    uint16_t                original_cp_count_    = 0;
    uint16_t                this_class_index_     = 0;
    size_t                  cp_count_offset_      = 0;
    size_t                  cp_end_offset_        = 0;
    size_t                  methods_count_offset_ = 0;
    size_t                  methods_end_offset_   = 0;
    std::vector<MethodInfo> methods_;
};

bool isVmManagementImpl(const char* class_name)
{
    return class_name != nullptr && std::strcmp(class_name, "sun/management/VMManagementImpl") == 0;
}

}   // namespace

bool hideAgentInputArguments(jvmtiEnv* jvmti, const char* class_name, jint class_data_len,
                             const unsigned char* class_data, jint* new_class_data_len,
                             unsigned char** new_class_data)
{
    if (!isVmManagementImpl(class_name) || jvmti == nullptr || class_data == nullptr ||
        class_data_len <= 0 || new_class_data_len == nullptr || new_class_data == nullptr) {
        return false;
    }

    ClassPatchReader           reader(class_data, static_cast<size_t>(class_data_len));
    std::vector<unsigned char> transformed;
    if (!reader.parse() || !reader.patchGetVmArguments(transformed) || transformed.empty()) {
        return false;
    }

    unsigned char* jvmti_bytes     = nullptr;
    const jint     transformed_len = static_cast<jint>(transformed.size());
    if (jvmti->Allocate(transformed_len, &jvmti_bytes) != JVMTI_ERROR_NONE ||
        jvmti_bytes == nullptr) {
        return false;
    }

    std::memcpy(jvmti_bytes, transformed.data(), transformed.size());
    *new_class_data_len = transformed_len;
    *new_class_data     = jvmti_bytes;
    return true;
}
