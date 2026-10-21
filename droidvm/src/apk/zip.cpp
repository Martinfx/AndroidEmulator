#include "apk/zip.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace droid {

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16)) | (uint32_t(p[3]) << 24); }

}  // namespace

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

ZipArchive::ZipArchive(const std::string& path) : data_(readFile(path)) {
    const size_t n = data_.size();
    if (n < 22) throw std::runtime_error(path + ": not a zip archive");

    // The end-of-central-directory record sits within the last 64 KiB + 22 bytes.
    size_t eocd = std::string::npos;
    size_t stop = n > 0xFFFF + 22 ? n - 0xFFFF - 22 : 0;
    for (size_t i = n - 22 + 1; i-- > stop;) {
        if (rd32(&data_[i]) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd == std::string::npos) throw std::runtime_error(path + ": zip directory not found");

    uint16_t count = rd16(&data_[eocd + 10]);
    size_t pos = rd32(&data_[eocd + 16]);
    for (uint16_t i = 0; i < count; ++i) {
        if (pos + 46 > n || rd32(&data_[pos]) != 0x02014b50)
            throw std::runtime_error(path + ": corrupt zip directory");
        const uint8_t* h = &data_[pos];
        Entry e{rd16(h + 10), rd32(h + 20), rd32(h + 24), rd32(h + 42)};
        uint16_t nameLen = rd16(h + 28), extraLen = rd16(h + 30), commentLen = rd16(h + 32);
        if (pos + 46 + nameLen > n) throw std::runtime_error(path + ": corrupt zip directory");
        entries_[std::string(reinterpret_cast<const char*>(h + 46), nameLen)] = e;
        pos += 46 + nameLen + extraLen + commentLen;
    }
}

std::vector<std::string> ZipArchive::names() const {
    std::vector<std::string> out;
    for (auto& [k, v] : entries_) out.push_back(k);
    return out;
}

std::optional<std::vector<uint8_t>> ZipArchive::read(const std::string& name) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return std::nullopt;
    const Entry& e = it->second;

    size_t lh = e.localHeaderOffset;
    if (lh + 30 > data_.size() || rd32(&data_[lh]) != 0x04034b50)
        throw std::runtime_error("zip: bad local header for " + name);
    size_t start = lh + 30 + rd16(&data_[lh + 26]) + rd16(&data_[lh + 28]);
    if (start + e.compressedSize > data_.size()) throw std::runtime_error("zip: truncated " + name);

    std::vector<uint8_t> out(e.uncompressedSize);
    if (e.method == 0) {
        std::memcpy(out.data(), &data_[start], e.uncompressedSize);
    } else if (e.method == 8) {
        z_stream zs{};
        zs.next_in = const_cast<Bytef*>(&data_[start]);
        zs.avail_in = e.compressedSize;
        zs.next_out = out.data();
        zs.avail_out = uInt(out.size());
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw std::runtime_error("zip: inflateInit failed");
        int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END) throw std::runtime_error("zip: cannot inflate " + name);
    } else {
        throw std::runtime_error("zip: unsupported compression method for " + name);
    }
    return out;
}

}  // namespace droid
