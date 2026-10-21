// Reader for Android binary XML (the compiled AndroidManifest.xml inside APKs).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace droid {

struct XmlElement {
    std::string name;
    std::map<std::string, std::string> attrs;  // keyed by local name, e.g. "name"
    std::vector<std::unique_ptr<XmlElement>> children;

    std::string attr(const std::string& key) const {
        auto it = attrs.find(key);
        return it == attrs.end() ? std::string() : it->second;
    }
};

// Throws std::runtime_error on malformed input.
std::unique_ptr<XmlElement> parseBinaryXml(const std::vector<uint8_t>& data);

struct ManifestInfo {
    std::string package;
    std::string launcherActivity;  // fully qualified, e.g. com.example.MainActivity
};

ManifestInfo readManifest(const std::vector<uint8_t>& axml);

}  // namespace droid
