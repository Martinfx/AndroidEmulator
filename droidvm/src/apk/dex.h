// Parser for Dalvik executable (classes.dex) files.
// Format reference: https://source.android.com/docs/core/runtime/dex-format
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace droid {

std::string utf16ToUtf8(const std::u16string& s);
std::u16string utf8ToUtf16(const std::string& s);

struct DexProto {
    std::string shorty;
    uint32_t returnType;
    std::vector<uint16_t> params;  // type indices
};

struct DexFieldId { uint16_t classIdx, typeIdx; uint32_t nameIdx; };
struct DexMethodId { uint16_t classIdx, protoIdx; uint32_t nameIdx; };

struct DexTry {
    uint32_t start;
    uint16_t count;
    std::vector<std::pair<uint32_t, uint32_t>> handlers;  // (type idx, address)
    int64_t catchAll = -1;
};

struct DexCode {
    uint16_t registers = 0, ins = 0, outs = 0;
    const uint16_t* insns = nullptr;
    uint32_t insnsSize = 0;  // in 16-bit units
    std::vector<DexTry> tries;
};

struct DexEncodedField { uint32_t fieldIdx, access; };
struct DexEncodedMethod { uint32_t methodIdx, access; uint32_t codeOff; };

struct DexClassDef {
    uint32_t classIdx, access, superIdx, sourceFileIdx;
    std::vector<uint16_t> interfaces;
    std::vector<DexEncodedField> staticFields, instanceFields;
    std::vector<DexEncodedMethod> directMethods, virtualMethods;
    uint32_t staticValuesOff = 0;
};

// One entry of an encoded_array (static field initial values).
struct DexValue {
    enum Type : uint8_t {
        Byte = 0x00, Short = 0x02, Char = 0x03, Int = 0x04, Long = 0x06, Float = 0x10,
        Double = 0x11, MethodType = 0x15, MethodHandle = 0x16, String = 0x17, TypeRef = 0x18,
        Field = 0x19, Method = 0x1a, Enum = 0x1b, Array = 0x1c, Annotation = 0x1d,
        Null = 0x1e, Boolean = 0x1f
    };
    Type type;
    uint64_t bits;  // raw value; index for String/TypeRef/...
};

constexpr uint32_t kNoIndex = 0xFFFFFFFF;

class DexFile {
public:
    explicit DexFile(std::vector<uint8_t> bytes);  // throws std::runtime_error

    const std::u16string& string(uint32_t idx) const { return strings_.at(idx); }
    const std::string& stringUtf8(uint32_t idx) const { return stringsUtf8_.at(idx); }
    const std::string& typeDescriptor(uint32_t typeIdx) const { return stringsUtf8_.at(typeIds_.at(typeIdx)); }
    const DexProto& proto(uint32_t idx) const { return protos_.at(idx); }
    const DexFieldId& field(uint32_t idx) const { return fields_.at(idx); }
    const DexMethodId& method(uint32_t idx) const { return methods_.at(idx); }
    const std::vector<DexClassDef>& classDefs() const { return classDefs_; }

    // "(II)V"-style signature of a proto, built from descriptors.
    std::string protoSignature(uint32_t protoIdx) const;
    DexCode code(uint32_t codeOff) const;
    std::vector<DexValue> encodedArray(uint32_t off) const;

    size_t stringCount() const { return strings_.size(); }
    size_t typeCount() const { return typeIds_.size(); }
    size_t fieldCount() const { return fields_.size(); }
    size_t methodCount() const { return methods_.size(); }

    const uint8_t* data() const { return bytes_.data(); }
    size_t size() const { return bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;
    std::vector<std::u16string> strings_;
    std::vector<std::string> stringsUtf8_;
    std::vector<uint32_t> typeIds_;
    std::vector<DexProto> protos_;
    std::vector<DexFieldId> fields_;
    std::vector<DexMethodId> methods_;
    std::vector<DexClassDef> classDefs_;
};

}  // namespace droid
