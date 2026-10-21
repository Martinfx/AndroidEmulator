#include "apk/axml.h"

#include <cstring>
#include <functional>
#include <stdexcept>

#include "apk/dex.h"

namespace droid {

namespace {

enum : uint16_t {
    kStringPool = 0x0001,
    kXml = 0x0003,
    kStartElement = 0x0102,
    kEndElement = 0x0103,
};

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16)) | (uint32_t(p[3]) << 24); }

std::vector<std::string> readStringPool(const uint8_t* chunk, size_t avail) {
    if (avail < 28) throw std::runtime_error("axml: short string pool");
    uint32_t count = rd32(chunk + 8);
    uint32_t flags = rd32(chunk + 16);
    uint32_t stringsStart = rd32(chunk + 20);
    uint32_t chunkSize = rd32(chunk + 4);
    bool utf8 = flags & (1 << 8);
    if (chunkSize > avail) throw std::runtime_error("axml: string pool overflows");

    std::vector<std::string> out;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off = rd32(chunk + 28 + 4 * i);
        const uint8_t* p = chunk + stringsStart + off;
        if (p >= chunk + chunkSize) throw std::runtime_error("axml: bad string offset");
        if (utf8) {
            // utf16 length, then utf8 length, each 1 or 2 bytes.
            p += (p[0] & 0x80) ? 2 : 1;
            size_t len = (p[0] & 0x80) ? (((p[0] & 0x7f) << 8) | p[1]) : p[0];
            p += (p[0] & 0x80) ? 2 : 1;
            out.emplace_back(reinterpret_cast<const char*>(p), len);
        } else {
            size_t len = rd16(p);
            p += 2;
            if (len & 0x8000) { len = ((len & 0x7fff) << 16) | rd16(p); p += 2; }
            std::u16string s;
            for (size_t k = 0; k < len; ++k) s.push_back(char16_t(rd16(p + 2 * k)));
            out.push_back(utf16ToUtf8(s));
        }
    }
    return out;
}

std::string typedValueToString(uint8_t type, uint32_t data) {
    switch (type) {
    case 0x01: return "@" + std::to_string(data);          // reference
    case 0x10: return std::to_string(int32_t(data));       // int dec
    case 0x11: return "0x" + std::to_string(data);         // int hex
    case 0x12: return data ? "true" : "false";             // boolean
    default: return std::to_string(data);
    }
}

}  // namespace

std::unique_ptr<XmlElement> parseBinaryXml(const std::vector<uint8_t>& d) {
    if (d.size() < 8 || rd16(d.data()) != kXml) throw std::runtime_error("axml: not binary xml");
    std::vector<std::string> pool;
    std::unique_ptr<XmlElement> root;
    std::vector<XmlElement*> stack;

    size_t pos = rd16(d.data() + 2);
    while (pos + 8 <= d.size()) {
        const uint8_t* c = d.data() + pos;
        uint16_t type = rd16(c);
        uint16_t headerSize = rd16(c + 2);
        uint32_t size = rd32(c + 4);
        if (size < 8 || pos + size > d.size()) throw std::runtime_error("axml: bad chunk");

        auto str = [&](uint32_t idx) -> std::string {
            return idx < pool.size() ? pool[idx] : std::string();
        };

        if (type == kStringPool) {
            pool = readStringPool(c, d.size() - pos);
        } else if (type == kStartElement) {
            const uint8_t* ext = c + headerSize;
            auto el = std::make_unique<XmlElement>();
            el->name = str(rd32(ext + 4));
            uint16_t attrStart = rd16(ext + 8), attrSize = rd16(ext + 10), attrCount = rd16(ext + 12);
            for (uint16_t i = 0; i < attrCount; ++i) {
                const uint8_t* a = ext + attrStart + size_t(i) * attrSize;
                if (a + 20 > d.data() + pos + size) throw std::runtime_error("axml: bad attribute");
                std::string name = str(rd32(a + 4));
                uint32_t raw = rd32(a + 8);
                uint8_t dataType = a[15];
                uint32_t data = rd32(a + 16);
                el->attrs[name] = raw != 0xFFFFFFFF ? str(raw)
                                  : dataType == 0x03 ? str(data)
                                                     : typedValueToString(dataType, data);
            }
            XmlElement* raw = el.get();
            if (stack.empty()) {
                if (root) throw std::runtime_error("axml: multiple roots");
                root = std::move(el);
            } else {
                stack.back()->children.push_back(std::move(el));
            }
            stack.push_back(raw);
        } else if (type == kEndElement) {
            if (stack.empty()) throw std::runtime_error("axml: unbalanced end tag");
            stack.pop_back();
        }
        pos += size;
    }
    if (!root) throw std::runtime_error("axml: empty document");
    return root;
}

ManifestInfo readManifest(const std::vector<uint8_t>& axml) {
    auto root = parseBinaryXml(axml);
    ManifestInfo info;
    info.package = root->attr("package");

    std::function<void(const XmlElement&)> walk = [&](const XmlElement& e) {
        if (e.name == "activity" || e.name == "activity-alias") {
            bool main = false, launcher = false;
            for (auto& f : e.children) {
                if (f->name != "intent-filter") continue;
                for (auto& x : f->children) {
                    std::string n = x->attr("name");
                    if (x->name == "action" && n == "android.intent.action.MAIN") main = true;
                    if (x->name == "category" && n == "android.intent.category.LAUNCHER") launcher = true;
                }
            }
            std::string name = e.name == "activity" ? e.attr("name") : e.attr("targetActivity");
            if (main && launcher && info.launcherActivity.empty() && !name.empty()) {
                info.launcherActivity = name[0] == '.' ? info.package + name
                                        : name.find('.') == std::string::npos ? info.package + "." + name
                                                                              : name;
            }
        }
        for (auto& c : e.children) walk(*c);
    };
    walk(*root);
    return info;
}

}  // namespace droid
