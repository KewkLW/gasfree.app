#include "resource_provider.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gasfree {

namespace fs = std::filesystem;

#ifdef _WIN32
static fs::path currentModuleDir() {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&currentModuleDir), &mod)) {
        char buf[MAX_PATH] = {};
        if (GetModuleFileNameA(mod, buf, MAX_PATH) > 0) {
            return fs::path(buf).parent_path();
        }
    }
    return {};
}
#else
// Directory of the loaded plugin binary (.so / .clap / Contents/MacOS/<bin>)
static fs::path currentModuleDir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&currentModuleDir), &info) && info.dli_fname)
        return fs::path(info.dli_fname).parent_path();
    return {};
}
#endif

ResourceProvider::ResourceProvider(std::string appDir, std::string bridgeJsPath)
    : appDir_(std::move(appDir)), bridgeJsPath_(std::move(bridgeJsPath)) {
    if (const char* envDir = std::getenv("GASFREE_APP_DIR"); envDir && *envDir) {
        appDir_ = envDir;
    }
    if (appDir_.empty()) {
        appDir_ = GASFREE_DEFAULT_APP_DIR;
    }
    // If the build-time path does not exist (e.g. portable release deployed to another machine),
    // search beside the plugin binary (./app, ../Resources/app, ../../app, ../../../app).
    if (!fs::exists(fs::path(appDir_) / "index.html")) {
        const fs::path dllDir = currentModuleDir();
        if (!dllDir.empty()) {
            const fs::path candidates[] = {
                dllDir / "app",
                dllDir / "gasfree" / "app",
                dllDir.parent_path() / "Resources" / "app",
                dllDir.parent_path() / "app",
                dllDir.parent_path().parent_path() / "Resources" / "app",
                dllDir.parent_path().parent_path().parent_path() / "app",
            };
            for (const auto& c : candidates) {
                if (fs::exists(c / "index.html")) {
                    appDir_ = c.string();
                    break;
                }
            }
        }
    }
    if (!fs::exists(fs::path(bridgeJsPath_))) {
        const fs::path dllDir = currentModuleDir();
        if (!dllDir.empty()) {
            const fs::path candidates[] = {
                dllDir / "plugin-bridge.js",
                dllDir / "assets" / "plugin-bridge.js",
                dllDir.parent_path() / "Resources" / "plugin-bridge.js",
                dllDir.parent_path().parent_path() / "Resources" / "plugin-bridge.js",
                dllDir.parent_path().parent_path().parent_path() / "plugin-bridge.js",
                fs::path(appDir_) / "plugin-bridge.js",
            };
            for (const auto& c : candidates) {
                if (fs::exists(c)) {
                    bridgeJsPath_ = c.string();
                    break;
                }
            }
        }
    }
}

std::string ResourceProvider::urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string ResourceProvider::mimeTypeForPath(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".wav") return "audio/wav";
    if (ext == ".mid" || ext == ".midi") return "audio/midi";
    if (ext == ".scl" || ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

static bool readFileBytes(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto sz = f.tellg();
    if (sz < 0) return false;
    f.seekg(0);
    out.resize(static_cast<size_t>(sz));
    return sz == 0 || static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), sz));
}

std::optional<WebResource> ResourceProvider::fetch(const std::string& rawPath) const {
    // Strip query string and fragment
    std::string clean = rawPath;
    if (auto q = clean.find_first_of("?#"); q != std::string::npos)
        clean.resize(q);
    clean = urlDecode(clean);
    if (clean.empty() || clean == "/") clean = "/index.html";

    // Remove leading slashes
    while (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());

    // Special virtual route: injected native plugin bridge
    if (clean == "plugin-bridge.js") {
        WebResource res;
        res.mimeType = "application/javascript; charset=utf-8";
        if (readFileBytes(fs::path(bridgeJsPath_), res.data))
            return res;
        return std::nullopt;
    }

    // Prevent directory traversal
    fs::path rel(clean);
    for (const auto& part : rel) {
        if (part == "..") return std::nullopt;
    }

    fs::path full = fs::path(appDir_) / rel;
    WebResource res;
    if (!readFileBytes(full, res.data)) return std::nullopt;
    res.mimeType = mimeTypeForPath(full.string());

    // Inject plugin-bridge.js into index.html right after webrack.js
    if (clean == "index.html") {
        std::string html(res.data.begin(), res.data.end());
        // Strip external Google Fonts links so offline DAWs never stall on network requests
        auto stripLink = [&](const std::string& needle) {
            size_t pos = html.find(needle);
            if (pos != std::string::npos) {
                size_t start = html.rfind("<link", pos);
                size_t end = html.find('>', pos);
                if (start != std::string::npos && end != std::string::npos) {
                    html.erase(start, end - start + 1);
                }
            }
        };
        stripLink("fonts.gstatic.com");
        stripLink("fonts.googleapis.com");

        // Inject plugin-bridge.js right after webrack.js
        const std::string marker = "webrack.js";
        size_t pos = html.find(marker);
        if (pos != std::string::npos) {
            size_t endTag = html.find("</script>", pos);
            if (endTag != std::string::npos) {
                html.insert(endTag + 9, "\n<script src=\"plugin-bridge.js\"></script>");
            }
        }
        res.data.assign(html.begin(), html.end());
    }

    return res;
}

} // namespace gasfree
