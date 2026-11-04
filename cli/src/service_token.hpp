#pragma once

#include <string>
#include <string_view>

namespace rfmap::cli {

/// A new service token for the server's services.tokens: the secret the client sends, and the
/// SHA-256 the server keeps. Made here, never by the server, so the secret exists in one place.
struct ServiceToken {
    /// Every token starts with it, so secret scanners and people recognise one in a log.
    static constexpr std::string_view kPrefix = "rfmap_svc_";

    std::string token;   ///< kPrefix + 32 random bytes as unpadded base64url
    std::string sha256;  ///< of `token`, 64 lowercase hex digits

    /// 32 bytes from the operating system's random generator.
    static ServiceToken generate();
    /// SHA-256 of `text` as lowercase hex.
    static std::string sha256Hex(std::string_view text);
    /// Unpadded base64url, as the server's own tokens.
    static std::string base64url(std::string_view bytes);
};

}  // namespace rfmap::cli
