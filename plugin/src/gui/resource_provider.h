#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gasfree {

struct WebResource {
    std::vector<uint8_t> data;
    std::string mimeType;
};

class ResourceProvider {
public:
    ResourceProvider(std::string appDir, std::string bridgeJsPath);

    std::optional<WebResource> fetch(const std::string& rawPath) const;
    const std::string& appDir() const noexcept { return appDir_; }

private:
    static std::string urlDecode(const std::string& s);
    static std::string mimeTypeForPath(const std::string& path);

    std::string appDir_;
    std::string bridgeJsPath_;
};

} // namespace gasfree
