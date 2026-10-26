#pragma once

// The fpv.blackbox decoder: finds the logs in a Blackbox file (.bbl, .bfl, .txt), decodes each
// with its own header and returns them as one normalized flight log (flightlog/v1).

#include "log_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fpv::blackbox {

/// Standard base64 (RFC 4648), as the file travels inside the JSON request.
class Base64 {
public:
    /// nullopt for a character outside the alphabet or a bad length. Whitespace is skipped.
    static std::optional<std::vector<std::uint8_t>> decode(std::string_view text);
};

/// Where each log starts and ends in a file. Every log opens with the Product header line.
class BlackboxFile {
public:
    static constexpr std::string_view kProductLine = "H Product:Blackbox flight data recorder by Nicholas Sherlock";
    static constexpr std::size_t kMaxLogs = 256;

    struct Segment {
        std::size_t begin = 0;
        std::size_t end = 0;
    };

    explicit BlackboxFile(std::span<const std::uint8_t> data);
    [[nodiscard]] const std::vector<Segment>& logs() const { return logs_; }
    /// More logs than kMaxLogs; the rest are not decoded.
    [[nodiscard]] bool truncated() const { return truncated_; }

private:
    std::vector<Segment> logs_;
    bool truncated_ = false;
};

class BlackboxDecoder {
public:
    static constexpr const char* kFormat = "betaflight-blackbox";

    BlackboxDecoder(std::string moduleId, std::string version) : moduleId_(std::move(moduleId)), version_(std::move(version)) {}

    /// {format, match, logs, firmware?}: whether this decoder reads the file.
    [[nodiscard]] json probe(std::span<const std::uint8_t> data) const;
    /// The normalized flight log (schemas/flightlog.schema.json).
    [[nodiscard]] json decode(std::span<const std::uint8_t> data, std::string_view fileName, const BuildOptions& options) const;

private:
    [[nodiscard]] static json decodeLog(std::span<const std::uint8_t> data, const BlackboxFile::Segment& segment, int index,
                                        const BuildOptions& options);

    std::string moduleId_;
    std::string version_;
};

}  // namespace fpv::blackbox
