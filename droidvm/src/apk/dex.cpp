#include "apk/dex.h"

#include <cstring>
#include <stdexcept>

namespace droid {

namespace {

struct Reader {
    const uint8_t* base;
    size_t size;
    size_t pos;

    void need(size_t n) const {
        if (pos + n > size) throw std::runtime_error("dex: read past end of file");
    }
    uint8_t u1() { need(1); return base[pos++]; }
    uint16_t u2() { need(2); uint16_t v; std::memcpy(&v, base + pos, 2); pos += 2; return v; }
    uint32_t u4() { need(4); uint32_t v; std::memcpy(&v, base + pos, 4); pos += 4; return v; }
    uint32_t uleb() {
        uint32_t result = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            uint8_t b = u1();
            result |= uint32_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return result;
        }
        throw std::runtime_error("dex: bad uleb128");
    }
    int32_t sleb() {
        int32_t result = 0;
        int shift = 0;
        uint8_t b;
        do {
            b = u1();
            result |= int32_t(b & 0x7f) << shift;
            shift += 7;
        } while ((b & 0x80) && shift < 35);
        if (shift < 32 && (b & 0x40)) result |= -(int32_t(1) << shift);
        return result;
    }
};

// Dex strings are "modified UTF-8": like CESU-8, with U+0000 encoded in two bytes.
std::u16string decodeMutf8(Reader& r) {
    uint32_t units = r.uleb();
    std::u16string out;
    out.reserve(units);
    for (;;) {
        uint8_t a = r.u1();
        if (a == 0) break;
        if (a < 0x80) {
            out.push_back(a);
        } else if ((a & 0xe0) == 0xc0) {
            uint8_t b = r.u1();
            out.push_back(char16_t(((a & 0x1f) << 6) | (b & 0x3f)));
        } else {
            uint8_t b = r.u1(), c = r.u1();
            out.push_back(char16_t(((a & 0x0f) << 12) | ((b & 0x3f) << 6) | (c & 0x3f)));
        }
    }
    return out;
}

}  // namespace

std::string utf16ToUtf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        }
        if (c < 0x80) {
            out.push_back(char(c));
        } else if (c < 0x800) {
            out.push_back(char(0xc0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3f)));
        } else if (c < 0x10000) {
            out.push_back(char(0xe0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(char(0x80 | (c & 0x3f)));
        } else {
            out.push_back(char(0xf0 | (c >> 18)));
            out.push_back(char(0x80 | ((c >> 12) & 0x3f)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(char(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

std::u16string utf8ToUtf16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint8_t a = uint8_t(s[i]);
        uint32_t c;
        int len;
        if (a < 0x80) { c = a; len = 1; }
        else if ((a & 0xe0) == 0xc0) { c = a & 0x1f; len = 2; }
        else if ((a & 0xf0) == 0xe0) { c = a & 0x0f; len = 3; }
        else { c = a & 0x07; len = 4; }
        if (i + len > s.size()) break;
        for (int k = 1; k < len; ++k) c = (c << 6) | (uint8_t(s[i + k]) & 0x3f);
        i += len;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(char16_t(0xd800 + (c >> 10)));
            out.push_back(char16_t(0xdc00 + (c & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}

DexFile::DexFile(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {
    if (bytes_.size() < 0x70 || std::memcmp(bytes_.data(), "dex\n", 4) != 0)
        throw std::runtime_error("not a dex file");
    Reader h{bytes_.data(), bytes_.size(), 0x38};
    uint32_t stringIdsSize = h.u4(), stringIdsOff = h.u4();
    uint32_t typeIdsSize = h.u4(), typeIdsOff = h.u4();
    uint32_t protoIdsSize = h.u4(), protoIdsOff = h.u4();
    uint32_t fieldIdsSize = h.u4(), fieldIdsOff = h.u4();
    uint32_t methodIdsSize = h.u4(), methodIdsOff = h.u4();
    uint32_t classDefsSize = h.u4(), classDefsOff = h.u4();

    Reader r{bytes_.data(), bytes_.size(), 0};
    auto at = [&](size_t off) -> Reader& { r.pos = off; return r; };

    strings_.reserve(stringIdsSize);
    for (uint32_t i = 0; i < stringIdsSize; ++i) {
        uint32_t dataOff = at(stringIdsOff + 4 * i).u4();
        strings_.push_back(decodeMutf8(at(dataOff)));
        stringsUtf8_.push_back(utf16ToUtf8(strings_.back()));
    }

    for (uint32_t i = 0; i < typeIdsSize; ++i) typeIds_.push_back(at(typeIdsOff + 4 * i).u4());

    for (uint32_t i = 0; i < protoIdsSize; ++i) {
        at(protoIdsOff + 12 * i);
        DexProto p;
        p.shorty = stringsUtf8_.at(r.u4());
        p.returnType = r.u4();
        uint32_t paramsOff = r.u4();
        if (paramsOff) {
            uint32_t n = at(paramsOff).u4();
            for (uint32_t k = 0; k < n; ++k) p.params.push_back(r.u2());
        }
        protos_.push_back(std::move(p));
    }

    for (uint32_t i = 0; i < fieldIdsSize; ++i) {
        at(fieldIdsOff + 8 * i);
        DexFieldId f;
        f.classIdx = r.u2();
        f.typeIdx = r.u2();
        f.nameIdx = r.u4();
        fields_.push_back(f);
    }

    for (uint32_t i = 0; i < methodIdsSize; ++i) {
        at(methodIdsOff + 8 * i);
        DexMethodId m;
        m.classIdx = r.u2();
        m.protoIdx = r.u2();
        m.nameIdx = r.u4();
        methods_.push_back(m);
    }

    for (uint32_t i = 0; i < classDefsSize; ++i) {
        at(classDefsOff + 32 * i);
        DexClassDef c;
        c.classIdx = r.u4();
        c.access = r.u4();
        c.superIdx = r.u4();
        uint32_t interfacesOff = r.u4();
        c.sourceFileIdx = r.u4();
        r.u4();  // annotations_off
        uint32_t classDataOff = r.u4();
        c.staticValuesOff = r.u4();

        if (interfacesOff) {
            uint32_t n = at(interfacesOff).u4();
            for (uint32_t k = 0; k < n; ++k) c.interfaces.push_back(r.u2());
        }
        if (classDataOff) {
            at(classDataOff);
            uint32_t nStatic = r.uleb(), nInstance = r.uleb(), nDirect = r.uleb(), nVirtual = r.uleb();
            auto readFields = [&](uint32_t n, std::vector<DexEncodedField>& out) {
                uint32_t idx = 0;
                for (uint32_t k = 0; k < n; ++k) {
                    idx += r.uleb();
                    out.push_back({idx, r.uleb()});
                }
            };
            auto readMethods = [&](uint32_t n, std::vector<DexEncodedMethod>& out) {
                uint32_t idx = 0;
                for (uint32_t k = 0; k < n; ++k) {
                    idx += r.uleb();
                    uint32_t access = r.uleb();
                    out.push_back({idx, access, r.uleb()});
                }
            };
            readFields(nStatic, c.staticFields);
            readFields(nInstance, c.instanceFields);
            readMethods(nDirect, c.directMethods);
            readMethods(nVirtual, c.virtualMethods);
        }
        classDefs_.push_back(std::move(c));
    }
}

std::string DexFile::protoSignature(uint32_t protoIdx) const {
    const DexProto& p = protos_.at(protoIdx);
    std::string s = "(";
    for (uint16_t t : p.params) s += typeDescriptor(t);
    s += ")";
    s += typeDescriptor(p.returnType);
    return s;
}

DexCode DexFile::code(uint32_t codeOff) const {
    Reader r{bytes_.data(), bytes_.size(), codeOff};
    DexCode c;
    c.registers = r.u2();
    c.ins = r.u2();
    c.outs = r.u2();
    uint16_t triesSize = r.u2();
    r.u4();  // debug_info_off
    c.insnsSize = r.u4();
    r.need(size_t(c.insnsSize) * 2);
    if (r.pos % 2) throw std::runtime_error("dex: misaligned code item");
    c.insns = reinterpret_cast<const uint16_t*>(bytes_.data() + r.pos);
    r.pos += size_t(c.insnsSize) * 2;

    if (triesSize) {
        if (c.insnsSize % 2) r.pos += 2;  // padding
        size_t triesPos = r.pos;
        size_t handlersBase = triesPos + 8 * size_t(triesSize);
        for (uint16_t i = 0; i < triesSize; ++i) {
            r.pos = triesPos + 8 * i;
            DexTry t;
            t.start = r.u4();
            t.count = r.u2();
            uint16_t handlerOff = r.u2();

            r.pos = handlersBase + handlerOff;
            int32_t size = r.sleb();
            for (int32_t k = 0; k < (size < 0 ? -size : size); ++k) {
                uint32_t type = r.uleb();
                t.handlers.push_back({type, r.uleb()});
            }
            if (size <= 0) t.catchAll = r.uleb();
            c.tries.push_back(std::move(t));
        }
    }
    return c;
}

std::vector<DexValue> DexFile::encodedArray(uint32_t off) const {
    std::vector<DexValue> out;
    if (!off) return out;
    Reader r{bytes_.data(), bytes_.size(), off};
    uint32_t n = r.uleb();
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t head = r.u1();
        auto type = DexValue::Type(head & 0x1f);
        uint32_t arg = head >> 5;
        DexValue v{type, 0};
        switch (type) {
        case DexValue::Null:
            break;
        case DexValue::Boolean:
            v.bits = arg;
            break;
        case DexValue::Array:
        case DexValue::Annotation:
            throw std::runtime_error("dex: nested arrays/annotations in static values unsupported");
        default: {
            uint32_t len = arg + 1;
            uint64_t bits = 0;
            for (uint32_t k = 0; k < len; ++k) bits |= uint64_t(r.u1()) << (8 * k);
            if (type == DexValue::Float || type == DexValue::Double) {
                // Right-zero-extended: the bytes are the high-order ones.
                int width = type == DexValue::Float ? 4 : 8;
                bits <<= 8 * (width - len);
            } else if (type == DexValue::Byte || type == DexValue::Short || type == DexValue::Int ||
                       type == DexValue::Long) {
                // Sign-extend.
                int shift = 64 - 8 * int(len);
                bits = uint64_t(int64_t(bits << shift) >> shift);
            }
            v.bits = bits;
        }
        }
        out.push_back(v);
    }
    return out;
}

}  // namespace droid
