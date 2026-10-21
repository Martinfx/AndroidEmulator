// Minimal ZIP reader for APK files (stored and deflated entries).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace droid {

class ZipArchive {
public:
    // Throws std::runtime_error when the file is not a readable ZIP archive.
    explicit ZipArchive(const std::string& path);

    bool contains(const std::string& name) const { return entries_.count(name) != 0; }
    std::vector<std::string> names() const;
    // Returns the decompressed bytes of an entry, or nullopt if it is missing.
    std::optional<std::vector<uint8_t>> read(const std::string& name) const;

private:
    struct Entry {
        uint16_t method;
        uint32_t compressedSize;
        uint32_t uncompressedSize;
        uint32_t localHeaderOffset;
    };
    std::vector<uint8_t> data_;
    std::map<std::string, Entry> entries_;
};

std::vector<uint8_t> readFile(const std::string& path);

}  // namespace droid
